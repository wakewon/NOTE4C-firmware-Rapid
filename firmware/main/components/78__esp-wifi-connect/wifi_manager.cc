/*
 * WiFi Manager Implementation
 */

#include "wifi_manager.h"
#include "wifi_station.h"
#include "wifi_configuration_ap.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <esp_timer.h>

#define TAG "WifiManager"
#define FAST_RC_TAG "FAST_RC"

WifiManager& WifiManager::GetInstance() {
    static WifiManager instance;
    return instance;
}

WifiManager::WifiManager() = default;

WifiManager::~WifiManager() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    WifiStation* station = nullptr;
    WifiConfigurationAp* config_ap = nullptr;
    bool initialized = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        station = station_active_ ? station_.get() : nullptr;
        config_ap = config_mode_active_ ? config_ap_.get() : nullptr;
        station_active_ = false;
        config_mode_active_ = false;
        initialized = initialized_;
        initialized_ = false;
    }
    if (station != nullptr) station->Stop();
    if (config_ap != nullptr) config_ap->Stop();
    if (initialized) {
        esp_wifi_deinit();
    }
}

void WifiManager::NotifyEvent(WifiEvent event) {
    // Copy callback under lock, invoke without lock to avoid deadlock
    std::function<void(WifiEvent)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // An ESP event may already be in flight when Stop() unregisters its
        // handler. Suppress stale positive station/config events after the
        // corresponding state has been published inactive.
        if ((event == WifiEvent::Scanning || event == WifiEvent::Connecting ||
             event == WifiEvent::Connected) && !station_active_) {
            return;
        }
        if (event == WifiEvent::ConfigModeEnter && !config_mode_active_) {
            return;
        }
        callback = event_callback_;
    }
    if (callback) {
        callback(event);
    }
}

bool WifiManager::Initialize(const WifiManagerConfig& config) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    config_ = config;
    ESP_LOGI(TAG, "Initializing...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize netif
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Create event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    station_ = std::make_unique<WifiStation>();
    station_->SetFastProbeTarget(probe_target_);
    config_ap_ = std::make_unique<WifiConfigurationAp>();

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized");
    return true;
}

bool WifiManager::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

// ==================== Station Mode ====================

void WifiManager::StartStation() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    WifiStation* station = nullptr;
    WifiConfigurationAp* config_ap = nullptr;
    WifiManagerConfig config;
    bool stop_config_ap = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            ESP_LOGE(TAG, "Not initialized");
            return;
        }
        if (station_active_) {
            ESP_LOGW(TAG, "Station already active");
            return;
        }
        station = station_.get();
        config_ap = config_ap_.get();
        config = config_;
        stop_config_ap = config_mode_active_;
        config_mode_active_ = false;
        // Publish the target state before the driver can emit events.
        station_active_ = true;
    }

    if (stop_config_ap) {
        ESP_LOGI(TAG, "Stopping config AP before starting station");
        config_ap->Stop();
    }

    ESP_LOGI(TAG, "Starting station");

    // Apply configuration
    station->SetScanIntervalRange(config.station_scan_min_interval_seconds,
                                  config.station_scan_max_interval_seconds);
    NetworkProbeTarget probe_target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        probe_target = probe_target_;
    }
    station->SetFastProbeTarget(probe_target);

    // Setup callbacks
    station->OnScanBegin([this]() {
        NotifyEvent(WifiEvent::Scanning);
    });
    station->OnConnect([this](const std::string&) {
        NotifyEvent(WifiEvent::Connecting);
    });
    station->OnConnected([this](const std::string&) {
        NotifyEvent(WifiEvent::Connected);
    });
    station->OnDisconnected([this]() {
        NotifyEvent(WifiEvent::Disconnected);
    });

    station->Start();
    lifecycle_lock.unlock();
    if (stop_config_ap) {
        NotifyEvent(WifiEvent::ConfigModeExit);
    }
}

void WifiManager::StopStation() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    WifiStation* station = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!station_active_) return;
        station_active_ = false;
        station = station_.get();
    }

    ESP_LOGI(TAG, "Stopping station");
    station->Stop();
    ESP_LOGI(TAG, "Station stopped");
    lifecycle_lock.unlock();
    NotifyEvent(WifiEvent::Disconnected);
}

void WifiManager::SuspendStationForExternalAp() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    WifiStation* station = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!station_active_ || !station_) {
            ESP_LOGI(TAG, "Suspend station for external AP skipped: station inactive");
            return;
        }
        station = station_.get();
    }
    station->SuspendForExternalAp();
}

void WifiManager::ResumeStationAfterExternalAp() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    WifiStation* station = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!station_active_ || !station_) {
            ESP_LOGI(TAG, "Resume station after external AP skipped: station inactive");
            return;
        }
        station = station_.get();
    }
    station->ResumeAfterExternalAp();
}

bool WifiManager::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_active_ && station_ && station_->IsConnected();
}

std::string WifiManager::GetSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) return "";
    return station_->GetSsid();
}

std::string WifiManager::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) return "";
    return station_->GetIpAddress();
}

int WifiManager::GetRssi() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected()) return 0;
    return station_->GetRssi();
}

int WifiManager::GetChannel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected()) return 0;
    return station_->GetChannel();
}

std::string WifiManager::GetMacAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mac_address_.empty()) {
        return mac_address_;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        mac_address_ = buf;
    }
    return mac_address_;
}

void WifiManager::ClearFastReconnectCache(const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (station_) {
        station_->ClearFastReconnectCache(reason);
    } else {
        ESP_LOGW(FAST_RC_TAG,
                 "stage=wifi event=cache_clear reason=%s result=skip_no_station t_ms=%lld",
                 reason ? reason : "unknown",
                 static_cast<long long>(esp_timer_get_time() / 1000));
    }
}

void WifiManager::ClearIpFastCache(const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (station_) {
        station_->ClearIpFastCache(reason);
    } else {
        ESP_LOGW(FAST_RC_TAG,
                 "stage=ip event=cache_clear scope=ip reason=%s result=skip_no_station t_ms=%lld",
                 reason ? reason : "unknown",
                 static_cast<long long>(esp_timer_get_time() / 1000));
    }
}

void WifiManager::SetFastProbeTarget(NetworkProbeTarget probe_target) {
    std::lock_guard<std::mutex> lock(mutex_);
    probe_target_ = probe_target;
    if (station_) {
        station_->SetFastProbeTarget(probe_target_);
    }
}

// ==================== Config AP Mode ====================

void WifiManager::StartConfigAp() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    WifiStation* station = nullptr;
    WifiConfigurationAp* config_ap = nullptr;
    WifiManagerConfig config;
    bool stop_station = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            ESP_LOGE(TAG, "Not initialized");
            return;
        }
        if (config_mode_active_) {
            ESP_LOGW(TAG, "Config AP already active");
            return;
        }
        station = station_.get();
        config_ap = config_ap_.get();
        config = config_;
        stop_station = station_active_;
        station_active_ = false;
        // Publish the target state before the driver can emit events.
        config_mode_active_ = true;
    }

    if (stop_station) {
        ESP_LOGI(TAG, "Stopping station before starting config AP");
        station->Stop();
    }

    ESP_LOGI(TAG, "Starting config AP");

    config_ap->SetSsidPrefix(config.ssid_prefix);
    config_ap->SetPassword(config.ap_password);
    config_ap->SetLanguage(config.language);
    
    // Web handler calls this when user submits config
    config_ap->OnExitRequested([this]() {
        ESP_LOGI(TAG, "Config exit requested from web");
        StopConfigAp();
        StartStation();
    });
    
    config_ap->Start();
    lifecycle_lock.unlock();
    if (stop_station) {
        NotifyEvent(WifiEvent::Disconnected);
    }
    NotifyEvent(WifiEvent::ConfigModeEnter);
}

void WifiManager::StopConfigAp() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    WifiConfigurationAp* config_ap = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!config_mode_active_) return;
        config_mode_active_ = false;
        config_ap = config_ap_.get();
    }

    ESP_LOGI(TAG, "Stopping config AP");
    config_ap->Stop();
    lifecycle_lock.unlock();
    NotifyEvent(WifiEvent::ConfigModeExit);
}

bool WifiManager::IsConfigMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_mode_active_;
}

std::string WifiManager::GetApSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_) return "";
    return config_ap_->GetSsid();
}

std::string WifiManager::GetApPassword() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_) return "";
    return config_ap_->GetPassword();
}

std::string WifiManager::GetApWebUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_) return "";
    return config_ap_->GetWebServerUrl();
}

// ==================== Power ====================

void WifiManager::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) {
        return;
    }
    station_->SetPowerSaveLevel(level);
}

// ==================== Event ====================

void WifiManager::SetEventCallback(std::function<void(WifiEvent)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}
