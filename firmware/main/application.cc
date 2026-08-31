#include "application.h"

#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "common/photo_storage.h"
#include "display.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "wifi_manager.h"
#include "boards/zectrix-s3-epaper-4.2/rtc_pcf8563.h"

#include <driver/gpio.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <ctime>
#include <algorithm>
#include <sys/time.h>

namespace {

constexpr char kTag[] = "Application";
constexpr char kSyncNamespace[] = "sync";
constexpr char kSyncIntervalKey[] = "sync_interval";
constexpr char kWifiEnabledKey[] = "wifi_enabled";
constexpr char kGalleryNamespace[] = "gallery";
constexpr char kSlideshowIntervalKey[] = "slide_min";
constexpr int kSettingsSlideshowIndex = 3;
constexpr int kSettingsNetworkSyncIndex = 5;
constexpr int kSettingsWifiIndex = 6;
constexpr int kSettingsHttpServerIndex = 7;
constexpr int kSettingsLanIpIndex = 8;
constexpr uint32_t kRetainedMagic = 0x4E344352;  // "N4CR" (layout v3)
constexpr int64_t kInteractiveAwakeUs = 120LL * 1000 * 1000;
constexpr int64_t kNetworkSyncTimeoutUs = 35LL * 1000 * 1000;

constexpr bool ShouldRunScheduledNetworkSync(bool wifi_enabled,
                                             int interval_minutes,
                                             int remaining_seconds) {
    return wifi_enabled && interval_minutes > 0 && remaining_seconds <= 0;
}

// The periodic setting is only a schedule. It must never override the user's
// persisted Wi-Fi switch, including on a timer wake with an already-due sync.
static_assert(!ShouldRunScheduledNetworkSync(false, 10, 0));
static_assert(!ShouldRunScheduledNetworkSync(true, 0, 0));
static_assert(!ShouldRunScheduledNetworkSync(true, 10, 1));
static_assert(ShouldRunScheduledNetworkSync(true, 10, 0));

constexpr bool ShouldEnterIdleSleep(bool idle_deadline_reached,
                                    bool external_power_present) {
    return idle_deadline_reached && !external_power_present;
}

static_assert(!ShouldEnterIdleSleep(true, true));
static_assert(ShouldEnterIdleSleep(true, false));
static_assert(!ShouldEnterIdleSleep(false, false));
static_assert(BOOT_BUTTON_GPIO <= GPIO_NUM_21);
static_assert(TODO_DOWN_BUTTON_GPIO <= GPIO_NUM_21);
static_assert(CHARGE_DETECT_GPIO <= GPIO_NUM_21);
static_assert(CHARGE_FULL_GPIO <= GPIO_NUM_21);
static_assert(TODO_UP_BUTTON_GPIO > GPIO_NUM_21,
              "UP is not an RTC IO on ESP32-S3 and cannot wake deep sleep");

struct RetainedSchedule {
    uint32_t magic;
    int slideshow_config_minutes;
    int sync_config_minutes;
    int slideshow_remaining_seconds;
    int sync_remaining_seconds;
    int selected_photo_index;
    int gallery_fullscreen;
    ui::PersistentDisplayMetadata display_metadata;
};

RTC_DATA_ATTR RetainedSchedule s_retained_schedule{};
bool s_sntp_started = false;

extern "C" RtcPcf8563* ZectrixGetRtc();
extern "C" void ZectrixPrepareForDeepSleep();
extern "C" bool ZectrixIsExternalPowerPresent();

int CurrentLocalDateKey() {
    time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    return (local_tm.tm_year + 1900) * 10000 +
           (local_tm.tm_mon + 1) * 100 + local_tm.tm_mday;
}

int SecondsUntilNextLocalMidnight() {
    time_t now = time(nullptr);
    struct tm next_tm = {};
    localtime_r(&now, &next_tm);
    if (next_tm.tm_year + 1900 < 2020) return 0;
    next_tm.tm_hour = 0;
    next_tm.tm_min = 0;
    next_tm.tm_sec = 0;
    next_tm.tm_mday += 1;
    next_tm.tm_isdst = -1;
    const time_t next_midnight = mktime(&next_tm);
    if (next_midnight <= now) return 0;
    return static_cast<int>(next_midnight - now);
}

bool HasDateWakeDependency(const ui::PersistentDisplayContract& contract) {
    const auto date_dependencies =
        rawdraw::PersistentDependencyMask(rawdraw::PersistentDisplayDependency::Date) |
        rawdraw::PersistentDependencyMask(rawdraw::PersistentDisplayDependency::PageDate);
    return contract.restorable &&
           (contract.wake_dependencies & date_dependencies) != 0;
}

void InitializeLocalClockFromRtc() {
    setenv("TZ", "CST-8", 1);
    tzset();
    auto* rtc = ZectrixGetRtc();
    if (!rtc) return;
    struct tm rtc_tm = {};
    if (!rtc->GetTime(rtc_tm) || rtc_tm.tm_year + 1900 < 2020) {
        ESP_LOGW(kTag, "RTC time unavailable or invalid; midnight scheduling deferred");
        return;
    }
    rtc_tm.tm_isdst = -1;
    const time_t rtc_epoch = mktime(&rtc_tm);
    if (rtc_epoch <= 0) return;
    const timeval tv{.tv_sec = rtc_epoch, .tv_usec = 0};
    settimeofday(&tv, nullptr);
    ESP_LOGI(kTag, "System clock restored from PCF8563: date_key=%d",
             CurrentLocalDateKey());
}

std::string FormatMinutesLabel(int minutes) {
    if (minutes <= 0) return "关闭";
    if (minutes == 1440) return "1天";
    if (minutes >= 60 && minutes % 60 == 0) {
        char hour_buf[16];
        snprintf(hour_buf, sizeof(hour_buf), "%dh", minutes / 60);
        return hour_buf;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%dmin", minutes);
    return buf;
}

std::string FormatNetworkSyncLabel(int minutes, bool wifi_enabled) {
    if (minutes <= 0) return "关闭";
    if (!wifi_enabled) return "暂停/Wi-Fi关闭";
    return FormatMinutesLabel(minutes);
}

const char* FormatMinutesLogLabel(int minutes) {
    return minutes <= 0 ? "关闭" : "开启";
}

int NextSlideshowInterval(int current) {
    static constexpr int kOptions[] = {0, 5, 10, 30};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 5;
}

int NextNetworkSyncInterval(int current) {
    static constexpr int kOptions[] = {0, 10, 30, 60, 360, 720, 1440};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 0;
}

bool IsValidSlideshowInterval(int value) {
    return value == 0 || value == 5 || value == 10 || value == 30;
}

bool IsValidNetworkSyncInterval(int value) {
    return value == 0 || value == 10 || value == 30 || value == 60 ||
           value == 360 || value == 720 || value == 1440;
}

// The three helpers below report whether they actually changed anything, so
// UpdateStatusBarForUi() can tell a real state transition from an echo of one
// a button handler already applied.
bool UpdateWifiSettingsItem(rawdraw::SettingsRenderer* renderer, bool enabled, bool connected,
                            const char* value = nullptr) {
    if (!renderer) return false;
    const bool checked_changed = renderer->UpdateChecked(kSettingsWifiIndex, enabled);
    const bool value_changed = renderer->UpdateItem(
        kSettingsWifiIndex,
        value ? value : (!enabled ? "已关闭" : (connected ? "已连接" : "未连接")));
    return checked_changed || value_changed;
}

bool UpdateHttpServerSettingsItem(rawdraw::SettingsRenderer* renderer, bool running,
                                  const std::string& ip_address = "") {
    if (!renderer) return false;
    std::string value;
    if (running && !ip_address.empty()) {
        value = "http://" + ip_address;
    } else if (!ip_address.empty()) {
        value = ip_address;
    } else {
        value = running ? "已开启" : "已关闭";
    }
    const bool checked_changed = renderer->UpdateChecked(kSettingsHttpServerIndex, running);
    const bool value_changed = renderer->UpdateItem(kSettingsHttpServerIndex, value);
    return checked_changed || value_changed;
}

bool UpdateLanIpSettingsItem(rawdraw::SettingsRenderer* renderer, const std::string& ip_address) {
    if (!renderer) return false;
    return renderer->UpdateItem(kSettingsLanIpIndex,
                                ip_address.empty() ? "未获取" : ip_address);
}

void StartSntpClockSyncOnce() {
    if (s_sntp_started) return;

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        time_t now = 0;
        time(&now);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        char time_buf[32] = {};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        ESP_LOGI(kTag, "SNTP time synchronized: %s", time_buf);
        Application::GetInstance().OnSntpSynchronized();
    });
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=Asia/Shanghai servers=ntp.aliyun.com,cn.pool.ntp.org,pool.ntp.org");
}

void StopSntpIfStarted() {
    if (!s_sntp_started) return;
    esp_sntp_stop();
    s_sntp_started = false;
}

bool IsLocalHttpServiceRunning(const ui::RawDrawUiManager* manager) {
    return manager != nullptr && manager->IsHttpServerRunning();
}

}  // namespace

Application::Application() = default;

Application::~Application() = default;

void Application::Initialize() {
    input_ready_.store(false, std::memory_order_release);
    Settings gallery_nvs(kGalleryNamespace, false);
    slideshow_interval_minutes_ = gallery_nvs.GetInt(kSlideshowIntervalKey, 5);
    if (!IsValidSlideshowInterval(slideshow_interval_minutes_)) {
        slideshow_interval_minutes_ = 5;
    }
    Settings sync_nvs(kSyncNamespace, false);
    network_sync_interval_minutes_ = sync_nvs.GetInt(kSyncIntervalKey, 0);
    wifi_enabled_.store(sync_nvs.GetBool(kWifiEnabledKey, false),
                        std::memory_order_release);
    if (!IsValidNetworkSyncInterval(network_sync_interval_minutes_)) {
        network_sync_interval_minutes_ = 0;
    }
    auto& board = Board::GetInstance();
    InitializeLocalClockFromRtc();

    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    const bool timer_wakeup =
        (wakeup_causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0;
    uint64_t button_wakeup_mask = 0;
    if ((wakeup_causes & (1UL << ESP_SLEEP_WAKEUP_EXT1)) != 0) {
        button_wakeup_mask = esp_sleep_get_ext1_wakeup_status();
    }
    const bool interactive_button_wake = !timer_wakeup &&
        (button_wakeup_mask & ((1ULL << BOOT_BUTTON_GPIO) |
                               (1ULL << TODO_DOWN_BUTTON_GPIO))) != 0;
    ESP_LOGI(kTag, "Wake sources=0x%08lx ext1_gpio_mask=0x%llx",
             static_cast<unsigned long>(wakeup_causes),
             static_cast<unsigned long long>(button_wakeup_mask));
    scheduler_timer_wake_ = timer_wakeup &&
                            s_retained_schedule.magic == kRetainedMagic;
    if (s_retained_schedule.magic != kRetainedMagic ||
        s_retained_schedule.slideshow_config_minutes != slideshow_interval_minutes_ ||
        s_retained_schedule.sync_config_minutes != network_sync_interval_minutes_) {
        s_retained_schedule = {
            .magic = kRetainedMagic,
            .slideshow_config_minutes = slideshow_interval_minutes_,
            .sync_config_minutes = network_sync_interval_minutes_,
            .slideshow_remaining_seconds = slideshow_interval_minutes_ * 60,
            .sync_remaining_seconds = network_sync_interval_minutes_ * 60,
            .selected_photo_index = 0,
            .gallery_fullscreen = 0,
        };
        scheduler_timer_wake_ = false;
    }
    if (scheduler_timer_wake_) {
        slideshow_due_ = slideshow_interval_minutes_ > 0 &&
                         s_retained_schedule.slideshow_remaining_seconds <= 0;
        network_sync_pending_ = ShouldRunScheduledNetworkSync(
            wifi_enabled_.load(std::memory_order_acquire),
            network_sync_interval_minutes_,
            s_retained_schedule.sync_remaining_seconds);
        if (slideshow_due_) {
            s_retained_schedule.slideshow_remaining_seconds =
                slideshow_interval_minutes_ * 60;
        }
        if (network_sync_pending_) {
            s_retained_schedule.sync_remaining_seconds =
                network_sync_interval_minutes_ * 60;
        }
        display_invalidation_due_ =
            HasDateWakeDependency(s_retained_schedule.display_metadata.contract) &&
            s_retained_schedule.display_metadata.snapshot.date_key !=
                CurrentLocalDateKey();
        ESP_LOGI(kTag,
                 "Timer wake reason: slideshow=%d network=%d display_invalidation=%d photo=%d",
                 slideshow_due_ ? 1 : 0, network_sync_pending_ ? 1 : 0,
                 display_invalidation_due_ ? 1 : 0,
                 s_retained_schedule.selected_photo_index);
    }
    ESP_LOGI(kTag, "Network policy: wifi_enabled=%d sync_interval=%d scheduled_sync=%d",
             wifi_enabled_.load(std::memory_order_acquire) ? 1 : 0,
             network_sync_interval_minutes_, network_sync_pending_ ? 1 : 0);

    SetDeviceState(kDeviceStateStarting);

    if (!scheduler_timer_wake_) {
        AudioCodec* codec = board.GetAudioCodec();
        if (codec == nullptr) {
            ESP_LOGE(kTag, "Audio codec is null");
            SetDeviceState(kDeviceStateFatalError);
            return;
        }
        audio_service_.Initialize(codec);
        audio_service_.Start();
        audio_started_ = true;
    } else {
        ESP_LOGI(kTag, "Timer wake: audio codec and audio tasks skipped");
    }

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    // Seed the very first framebuffer with a real battery sample. Previously
    // this happened accidentally through the automatic Wi-Fi status callback;
    // with radio-idle startup that callback does not run, leaving level=-1 and
    // hiding the icon. Timer wakes sample here too, so the battery indication
    // advances only together with an already-due slideshow refresh.
    int initial_battery_level = -1;
    bool initial_charging = false;
    bool initial_discharging = false;
    board.GetBatteryLevel(initial_battery_level, initial_charging,
                          initial_discharging);
    ui::RawDrawStatusBarData initial_status =
        rawdraw_ui_manager_->GetStatusBarData();
    initial_status.battery_level = initial_battery_level;
    initial_status.battery_charging = initial_charging;
    rawdraw_ui_manager_->UpdateStatusBar(initial_status);
    external_power_present_ = !initial_discharging;
    external_power_probe_deadline_us_ =
        esp_timer_get_time() + 1000LL * 1000;
    display_freshness_probe_deadline_us_ =
        esp_timer_get_time() + 30LL * 1000 * 1000;
    ESP_LOGI(kTag,
             "Initial battery sample: level=%d charging=%d external_power=%d",
             initial_battery_level, initial_charging ? 1 : 0,
             external_power_present_ ? 1 : 0);
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, ui::RefreshIntent intent) {
        if (intent != ui::RefreshIntent::FullColor) {
            // Buttons, menus and cursor movement keep the low-latency path.
            // Photo/content changes recover after 10 s; menu-only operations
            // deliberately defer the same recovery to 30 s.
            const auto recovery_mode =
                intent == ui::RefreshIntent::FastBwDeferredInteraction
                    ? ssd2683_fast_bw::RecoveryMode::DeferredInteraction
                    : ssd2683_fast_bw::RecoveryMode::Quality;
            lcd->RequestFastBwRefresh(recovery_mode);
        } else {
            // Background state changes have no interactive latency target.
            // Render them correctly once instead of FAST_BW plus a recovery.
            lcd->RequestUrgentFullRefresh();
        }
    }, scheduler_timer_wake_ || interactive_button_wake);
    if ((scheduler_timer_wake_ || interactive_button_wake) &&
        s_retained_schedule.magic == kRetainedMagic) {
        rawdraw_ui_manager_->AdoptRetainedPersistentMetadata(
            s_retained_schedule.display_metadata);
    }
    rawdraw_ui_manager_->SetLowPowerSlideshowMode(true);
    rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(slideshow_interval_minutes_);
    if (scheduler_timer_wake_ && slideshow_due_) {
        slideshow_refresh_requested_ = rawdraw_ui_manager_->PrepareGallerySlideshowFrame(
            s_retained_schedule.selected_photo_index, true);
        ESP_LOGI(kTag, "Scheduled slideshow frame requested=%d",
                 slideshow_refresh_requested_ ? 1 : 0);
    } else if (scheduler_timer_wake_ && display_invalidation_due_) {
        display_invalidation_refresh_requested_ =
            rawdraw_ui_manager_->PrepareRetainedDisplayInvalidation(
                s_retained_schedule.selected_photo_index,
                s_retained_schedule.gallery_fullscreen != 0);
        ESP_LOGI(kTag,
                 "Scheduled display invalidation caused refresh=%d",
                 display_invalidation_refresh_requested_ ? 1 : 0);
    } else if (interactive_button_wake) {
        const bool down_wake =
            (button_wakeup_mask & (1ULL << TODO_DOWN_BUTTON_GPIO)) != 0;
        const rawdraw::ButtonEvent wake_event{
            down_wake ? rawdraw::ButtonEvent::kDownClick
                      : rawdraw::ButtonEvent::kBootClick};
        slideshow_refresh_requested_ =
            rawdraw_ui_manager_->PrepareGalleryInteractiveWakeFrame(
                s_retained_schedule.selected_photo_index,
                s_retained_schedule.gallery_fullscreen != 0, wake_event);
        ESP_LOGI(kTag,
                 "Interactive button wake replayed: gpio=%d requested=%d",
                 down_wake ? static_cast<int>(TODO_DOWN_BUTTON_GPIO)
                           : static_cast<int>(BOOT_BUTTON_GPIO),
                 slideshow_refresh_requested_ ? 1 : 0);
    }

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        int slideshow_interval = slideshow_interval_minutes_;
        ESP_LOGI(kTag, "Startup gallery fullscreen slideshow: %s, interval=%s",
                 FormatMinutesLogLabel(slideshow_interval),
                 FormatMinutesLabel(slideshow_interval).c_str());
        rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(slideshow_interval);

        std::vector<rawdraw::SettingsItemDef> items;
        items.push_back({"系统", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"重启", "执行", nullptr, rawdraw::SettingsItemType::Action, false,
                         []() { esp_restart(); }});
        items.push_back({"相册", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"轮播间隔", FormatMinutesLabel(slideshow_interval), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             Settings nvs(kGalleryNamespace, true);
                             const int current = nvs.GetInt(kSlideshowIntervalKey, 5);
                             const int next = NextSlideshowInterval(current);
                             nvs.SetInt(kSlideshowIntervalKey, next);
                             slideshow_interval_minutes_ = next;
                             s_retained_schedule.slideshow_config_minutes = next;
                             s_retained_schedule.slideshow_remaining_seconds = next * 60;
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(next);
                             }
                             sr->UpdateItem(kSettingsSlideshowIndex, FormatMinutesLabel(next));
                         }});
        items.push_back({"网络", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"定时校时",
                         FormatNetworkSyncLabel(
                             network_sync_interval_minutes_,
                             wifi_enabled_.load(std::memory_order_acquire)),
                         nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             Settings nvs(kSyncNamespace, true);
                             const int current = nvs.GetInt(kSyncIntervalKey, 0);
                             const int next = NextNetworkSyncInterval(current);
                             nvs.SetInt(kSyncIntervalKey, next);
                             network_sync_interval_minutes_ = next;
                             s_retained_schedule.sync_config_minutes = next;
                             s_retained_schedule.sync_remaining_seconds = next * 60;
                             sr->UpdateItem(kSettingsNetworkSyncIndex,
                                            FormatNetworkSyncLabel(
                                                next,
                                                wifi_enabled_.load(
                                                    std::memory_order_acquire)));
                             ESP_LOGI(kTag, "Periodic network sync set to %s",
                                      FormatMinutesLabel(next).c_str());
                         }});
        items.push_back({"Wi-Fi",
                         wifi_enabled_.load(std::memory_order_acquire)
                             ? "未连接" : "已关闭",
                         nullptr, rawdraw::SettingsItemType::Checkbox,
                         wifi_enabled_.load(std::memory_order_acquire),
                         [this, sr]() {
                             auto& wifi = WifiManager::GetInstance();
                             const bool enabled =
                                 wifi_enabled_.load(std::memory_order_acquire);
                             Settings nvs(kSyncNamespace, true);
                             if (enabled) {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled OFF");
                                 // Publish the policy first. StopStation emits a
                                 // synchronous Disconnected event, which must
                                 // already observe the switch as disabled.
                                 wifi_enabled_.store(false, std::memory_order_release);
                                 nvs.SetBool(kWifiEnabledKey, false);
                                 wifi_connect_deadline_us_ = 0;
                                 if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                     rawdraw_ui_manager_->StopLanHttpServer();
                                     UpdateHttpServerSettingsItem(sr, false);
                                 }
                                 wifi.StopStation();
                                 wifi_connected_.store(false, std::memory_order_release);
                                 UpdateWifiSettingsItem(sr, false, false);
                                 UpdateLanIpSettingsItem(sr, "");
                             } else {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled ON");
                                 wifi_enabled_.store(true, std::memory_order_release);
                                 nvs.SetBool(kWifiEnabledKey, true);
                                 s_retained_schedule.sync_remaining_seconds =
                                     network_sync_interval_minutes_ * 60;
                                 UpdateWifiSettingsItem(sr, true, false, "连接中");
                                 StartInteractiveWifiAttempt();
                             }
                             sr->UpdateItem(
                                 kSettingsNetworkSyncIndex,
                                 FormatNetworkSyncLabel(
                                     network_sync_interval_minutes_,
                                     wifi_enabled_.load(
                                         std::memory_order_acquire)));
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网服务", "已关闭", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             if (!rawdraw_ui_manager_) return;
                             if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                 ESP_LOGI(kTag, "LAN HTTP server toggled OFF");
                                 rawdraw_ui_manager_->StopLanHttpServer();
                                 UpdateHttpServerSettingsItem(sr, false);
                                 UpdateStatusBarForUi();
                                 return;
                             }

                             auto& wifi = WifiManager::GetInstance();
                             if (!wifi_connected_.load(std::memory_order_acquire) && !wifi.IsConnected()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires WiFi connection");
                                 UpdateHttpServerSettingsItem(sr, false, "需先连接WiFi");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const std::string ip = wifi.GetIpAddress();
                             if (ip.empty()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires station IP");
                                 UpdateHttpServerSettingsItem(sr, false, "等待IP");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                             ESP_LOGI(kTag, "LAN HTTP server toggled ON: started=%d url=http://%s/",
                                      started ? 1 : 0, ip.c_str());
                             UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                             UpdateLanIpSettingsItem(sr, started ? ip : WifiManager::GetInstance().GetIpAddress());
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网IP", "未获取", nullptr, rawdraw::SettingsItemType::Normal, false});
        items.push_back({"省电模式", "手动进入", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ESP_LOGI(kTag, "Manual sleep requested from settings");
                             EnterManualSleep();
                         }});
        items.push_back({"关于", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"固件", PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});
        sr->SetItems(items);
        sr->SetFirmwareVersion("v" PROJECT_VER);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");
    }

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    if (interactive_button_wake) {
        ESP_LOGI(kTag, "Interactive wake from deep sleep");
        board.FlashActivityLed();
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Connected:
                ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
                if (!wifi_enabled_.load(std::memory_order_acquire)) {
                    ESP_LOGW(kTag, "Ignoring late Wi-Fi connection because switch is OFF");
                    WifiManager::GetInstance().StopStation();
                    break;
                }
                wifi_connect_deadline_us_ = 0;
                wifi_connected_.store(true, std::memory_order_release);
                StartSntpClockSyncOnce();
                // LAN transfer is intentionally opt-in. A scheduled sync only
                // keeps the radio up long enough for SNTP, then turns it off.
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                UpdateStatusBarForUi(true, true);
                break;
            case NetworkEvent::Disconnected:
                ESP_LOGI(kTag, "WiFi disconnected");
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    rawdraw_ui_manager_->StopLanHttpServer();
                }
                UpdateStatusBarForUi(true, true);
                break;
            case NetworkEvent::Connecting:
            case NetworkEvent::Scanning:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi(false, false);
                if (rawdraw_ui_manager_) {
                    UpdateWifiSettingsItem(
                        rawdraw_ui_manager_->GetSettingsRenderer(),
                        wifi_enabled_.load(std::memory_order_acquire), false,
                        "连接中");
                }
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_) {
                    auto& wifi = WifiManager::GetInstance();
                    rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                            wifi.GetApPassword(),
                                                            wifi.GetApWebUrl());
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeExit:
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi config AP exited, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                wifi_connected_.store(WifiManager::GetInstance().IsConnected(),
                                      std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
        }
    });

    if (network_sync_pending_) {
        StartScheduledNetworkSync();
    } else if (!scheduler_timer_wake_ &&
               wifi_enabled_.load(std::memory_order_acquire)) {
        StartInteractiveWifiAttempt();
    }

    if (!scheduler_timer_wake_) {
        interactive_sleep_deadline_us_ = esp_timer_get_time() + kInteractiveAwakeUs;
        if (interactive_button_wake) {
            const gpio_num_t wake_gpio =
                (button_wakeup_mask & (1ULL << TODO_DOWN_BUTTON_GPIO)) != 0
                    ? static_cast<gpio_num_t>(TODO_DOWN_BUTTON_GPIO)
                    : static_cast<gpio_num_t>(BOOT_BUTTON_GPIO);
            // The wake action above is deterministic. Keep normal callbacks
            // gated until release so the same physical press is not delivered
            // a second time by the button component during startup.
            const int64_t release_deadline_us =
                esp_timer_get_time() + 5LL * 1000 * 1000;
            while (gpio_get_level(wake_gpio) == 0 &&
                   esp_timer_get_time() < release_deadline_us) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (gpio_get_level(wake_gpio) == 0) {
                ESP_LOGW(kTag,
                         "Wake key GPIO%d still held after 5 s; enabling input anyway",
                         static_cast<int>(wake_gpio));
            }
        }
        input_ready_.store(true, std::memory_order_release);
    }

    SetDeviceState(kDeviceStateIdle);
}

void Application::OnUpClick() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "UP click");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpClick});
    }
}

void Application::OnDownClick() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "DOWN click");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kDownClick});
    }
}

void Application::OnUpLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "UP long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings) {
        ESP_LOGI(kTag, "UP long press - leaving settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
    }
}

void Application::OnDownLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "DOWN long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        ESP_LOGI(kTag, "DOWN long press - entering settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
    }
}

void Application::OnWifiConfigComboLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "UP+DOWN long press");
    NoteButtonActivity();
    EnterWifiConfigMode();
}

void Application::OnBootClick() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "BOOT click");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootClick});
    }
}

void Application::OnBootLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "BOOT long press");
    NoteButtonActivity();
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "BOOT long press - exiting WiFi config AP");
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
        }
        if (wifi_enabled_.load(std::memory_order_acquire)) {
            StartInteractiveWifiAttempt();
        } else {
            WifiManager::GetInstance().StopStation();
        }
        return;
    }
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootLongPress});
    }
}

void Application::NoteButtonActivity() {
    interactive_sleep_deadline_us_ = esp_timer_get_time() + kInteractiveAwakeUs;
    manual_sleep_requested_.store(false, std::memory_order_release);
    // Fold a fresh battery/network sample into the button handler's one
    // already-planned render. This is opportunistic and never queues another
    // refresh of its own (fullscreen photos still have no such dependency).
    UpdateStatusBarForUi(false, false);
    // The actual button handler renders the resulting UI state with its
    // explicit FAST_BW intent. Queueing an additional background redraw here
    // used to turn long presses into a same-content automatic refresh and,
    // after refresh intents were separated, would immediately override the
    // 10/30-second interaction policy with FULL_COLOR.
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    wifi_connected_.store(false, std::memory_order_release);
    wifi_connect_deadline_us_ = 0;
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    EnsureNetworkInitialized();
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

void Application::EnsureNetworkInitialized() {
    if (network_initialized_) return;
    Board::GetInstance().RequestNetwork();
    network_initialized_ = true;
}

void Application::StartInteractiveWifiAttempt() {
    if (!wifi_enabled_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Wi-Fi attempt suppressed because switch is OFF");
        return;
    }
    EnsureNetworkInitialized();
    wifi_connect_deadline_us_ = esp_timer_get_time() + kNetworkSyncTimeoutUs;
    ESP_LOGI(kTag, "Interactive Wi-Fi attempt started with %lld s timeout",
             static_cast<long long>(kNetworkSyncTimeoutUs / 1000000));
    WifiManager::GetInstance().StartStation();
}

void Application::StartScheduledNetworkSync() {
    if (!wifi_enabled_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Scheduled network sync suppressed because Wi-Fi switch is OFF");
        network_sync_pending_ = false;
        return;
    }
    ESP_LOGI(kTag, "Scheduled network sync: starting Wi-Fi with %lld s timeout",
             static_cast<long long>(kNetworkSyncTimeoutUs / 1000000));
    EnsureNetworkInitialized();
    network_sync_deadline_us_ = esp_timer_get_time() + kNetworkSyncTimeoutUs;
    WifiManager::GetInstance().StartStation();
}

void Application::EnterScheduledSleep() {
    ESP_LOGI(kTag, "Scheduled wake caused display refresh=%d",
             (slideshow_refresh_requested_ ||
              display_invalidation_refresh_requested_) ? 1 : 0);
    EnterLowPowerSleep("scheduled work complete");
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Manual sleep queued; waiting for current EPD refresh to finish");
    manual_sleep_requested_.store(true, std::memory_order_release);
}

void Application::RequestManualSleep(bool disable_wifi) {
    if (disable_wifi) {
        wifi_enabled_.store(false, std::memory_order_release);
        Settings sync_nvs(kSyncNamespace, true);
        sync_nvs.SetBool(kWifiEnabledKey, false);
        ESP_LOGI(kTag, "Wi-Fi switch persisted OFF for web sleep request");
    }
    EnterManualSleep();
}

void Application::EnterLowPowerSleep(const char* reason) {
    if (sleep_phase_ != SleepPhase::Awake) return;
    pending_sleep_reason_ = reason ? reason : "unspecified";
    input_ready_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Sleep preparation beginning: %s", pending_sleep_reason_);

    if (rawdraw_ui_manager_) {
        char dependencies[96];
        const auto metadata = rawdraw_ui_manager_->GetDisplayedPersistentMetadata();
        ESP_LOGI(kTag,
                 "Current display dependencies=%s wake=0x%lx restorable=%d kind=%d",
                 ui::RawDrawUiManager::DescribePersistentDependencies(
                     metadata.contract.visible_dependencies,
                     dependencies, sizeof(dependencies)),
                 static_cast<unsigned long>(metadata.contract.wake_dependencies),
                 metadata.contract.restorable ? 1 : 0,
                 static_cast<int>(metadata.contract.restore_kind));
        rawdraw_ui_manager_->BeginPersistentSleepPreparation();
        rawdraw_ui_manager_->StopHttpServicesForSleep();
    }
    if (network_initialized_) WifiManager::GetInstance().StopStation();
    wifi_connected_.store(false, std::memory_order_release);
    network_sync_pending_ = false;
    network_sync_deadline_us_ = 0;
    wifi_connect_deadline_us_ = 0;
    StopSntpIfStarted();
    esp_wifi_disconnect();
    esp_wifi_stop();
    UpdateStatusBarForUi(false, false);

    ui::PersistentFramePreparation frame;
    if (rawdraw_ui_manager_) {
        frame = rawdraw_ui_manager_->PreparePersistentFrame();
    }
    char changed[96];
    char visible[96];
    ESP_LOGI(kTag,
             "Final visible synchronization: changed=%s visible=%s framebuffer_changed=%d",
             ui::RawDrawUiManager::DescribePersistentDependencies(
                 frame.changed_dependencies, changed, sizeof(changed)),
             ui::RawDrawUiManager::DescribePersistentDependencies(
                 frame.visible_changes, visible, sizeof(visible)),
             frame.framebuffer_changed ? 1 : 0);
    if (frame.refresh_requested) {
        ESP_LOGI(kTag,
                 "[FULL_COLOR] pre-sleep refresh requested; waiting for EPD idle");
        sleep_phase_ = SleepPhase::WaitingForDisplay;
    } else {
        ESP_LOGI(kTag, "Pre-sleep full-color refresh skipped: no visible pixel change");
        sleep_phase_ = SleepPhase::ReadyToCommit;
    }
}

void Application::CommitLowPowerSleep() {
    // A late station callback may race the asynchronous EPD waveform. Reassert
    // the final radio state immediately before power is committed; the glass
    // already contains the disconnected state selected by preflight.
    if (network_initialized_) WifiManager::GetInstance().StopStation();
    wifi_connected_.store(false, std::memory_order_release);
    StopSntpIfStarted();
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (audio_started_) {
        audio_service_.Stop();
        audio_started_ = false;
    }

    s_retained_schedule.magic = kRetainedMagic;
    s_retained_schedule.slideshow_config_minutes = slideshow_interval_minutes_;
    s_retained_schedule.sync_config_minutes = network_sync_interval_minutes_;
    if (!scheduler_timer_wake_ || slideshow_refresh_requested_ ||
        display_invalidation_refresh_requested_) {
        s_retained_schedule.selected_photo_index =
            rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetGallerySelectedIndex() : 0;
        s_retained_schedule.gallery_fullscreen =
            rawdraw_ui_manager_ && rawdraw_ui_manager_->IsGalleryFullscreen() ? 1 : 0;
    }
    if (rawdraw_ui_manager_) {
        s_retained_schedule.display_metadata =
            rawdraw_ui_manager_->GetDisplayedPersistentMetadata();
    }
    if (slideshow_interval_minutes_ <= 0) {
        s_retained_schedule.slideshow_remaining_seconds = 0;
    } else if (s_retained_schedule.slideshow_remaining_seconds <= 0) {
        s_retained_schedule.slideshow_remaining_seconds =
            slideshow_interval_minutes_ * 60;
    }
    if (network_sync_interval_minutes_ <= 0) {
        s_retained_schedule.sync_remaining_seconds = 0;
    } else if (s_retained_schedule.sync_remaining_seconds <= 0) {
        s_retained_schedule.sync_remaining_seconds =
            network_sync_interval_minutes_ * 60;
    }

    int display_invalidation_seconds = 0;
    if (HasDateWakeDependency(s_retained_schedule.display_metadata.contract)) {
        display_invalidation_seconds =
            s_retained_schedule.display_metadata.snapshot.date_key !=
                    CurrentLocalDateKey()
                ? 1
                : SecondsUntilNextLocalMidnight();
    }
    ESP_LOGI(kTag, "Calculated display invalidation deadline: %d seconds",
             display_invalidation_seconds);

    int next_wake_seconds = 0;
    auto consider_deadline = [&next_wake_seconds](int remaining) {
        if (remaining > 0 &&
            (next_wake_seconds == 0 || remaining < next_wake_seconds)) {
            next_wake_seconds = remaining;
        }
    };
    consider_deadline(s_retained_schedule.slideshow_remaining_seconds);
    if (wifi_enabled_.load(std::memory_order_acquire)) {
        consider_deadline(s_retained_schedule.sync_remaining_seconds);
    }
    consider_deadline(display_invalidation_seconds);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (next_wake_seconds > 0) {
        if (slideshow_interval_minutes_ > 0) {
            s_retained_schedule.slideshow_remaining_seconds = std::max(
                0, s_retained_schedule.slideshow_remaining_seconds - next_wake_seconds);
        }
        if (wifi_enabled_.load(std::memory_order_acquire) &&
            network_sync_interval_minutes_ > 0) {
            s_retained_schedule.sync_remaining_seconds = std::max(
                0, s_retained_schedule.sync_remaining_seconds - next_wake_seconds);
        }
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
            static_cast<uint64_t>(next_wake_seconds) * 1000000ULL));
        ESP_LOGI(kTag,
                 "Next wake in %d seconds (slide_remaining=%d sync_remaining=%d display=%d)",
                 next_wake_seconds,
                 s_retained_schedule.slideshow_remaining_seconds,
                 s_retained_schedule.sync_remaining_seconds,
                 display_invalidation_seconds);
    } else {
        ESP_LOGI(kTag, "All schedules disabled; sleeping until GPIO wake");
    }
    // BOOT, DOWN and the charger's active-low CHARGE output can share EXT1.
    // The active-high FULL output uses EXT0 so USB insertion also wakes a
    // device whose battery is already full. Together these cover the charger
    // states without running a ULP program during deep sleep.
    constexpr uint64_t kLowLevelWakeMask =
        (1ULL << BOOT_BUTTON_GPIO) |
        (1ULL << TODO_DOWN_BUTTON_GPIO) |
        (1ULL << CHARGE_DETECT_GPIO);
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
        kLowLevelWakeMask, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(
        static_cast<gpio_num_t>(CHARGE_FULL_GPIO), 1));

    ESP_LOGI(kTag, "Final deep-sleep commit: %s",
             pending_sleep_reason_ ? pending_sleep_reason_ : "unspecified");
    // Shut down the rails only after all users (display, Wi-Fi, audio and NFC)
    // have become idle. Per-pin holds already configured by the board BSP are
    // then made effective throughout deep sleep on ESP32-S3.
    ZectrixPrepareForDeepSleep();
    gpio_deep_sleep_hold_en();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

void Application::OnSntpSynchronized() {
    time_t now = 0;
    time(&now);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    if (auto* rtc = ZectrixGetRtc()) {
        if (!rtc->SetTime(local_tm)) {
            ESP_LOGW(kTag, "SNTP succeeded but writing PCF8563 failed");
        } else {
            ESP_LOGI(kTag, "PCF8563 updated from SNTP");
        }
    }
    if (scheduler_timer_wake_) {
        StopSntpIfStarted();
        if (network_initialized_) {
            WifiManager::GetInstance().StopStation();
        }
        wifi_connected_.store(false, std::memory_order_release);
        network_sync_pending_ = false;
        ESP_LOGI(kTag, "Scheduled network sync complete; Wi-Fi stopped");
    } else {
        // Clock and status-bar text are monochrome. A successful sync must not
        // turn a background network event into an expensive color refresh.
        UpdateStatusBarForUi(true, true);
    }
}

void Application::Run() {
    while (true) {
        if (rawdraw_ui_manager_ && sleep_phase_ == SleepPhase::Awake) {
            rawdraw_ui_manager_->PumpClockRefresh();
        }
        auto* lcd = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        bool display_busy = lcd != nullptr && lcd->IsRefreshPending();
        const int64_t now_us = esp_timer_get_time();

        if (!scheduler_timer_wake_ && sleep_phase_ == SleepPhase::Awake &&
            now_us >= display_freshness_probe_deadline_us_) {
            display_freshness_probe_deadline_us_ = now_us + 30LL * 1000 * 1000;
            const auto date_dependencies =
                rawdraw::PersistentDependencyMask(
                    rawdraw::PersistentDisplayDependency::Date) |
                rawdraw::PersistentDependencyMask(
                    rawdraw::PersistentDisplayDependency::PageDate);
            if (!display_busy && rawdraw_ui_manager_ &&
                (rawdraw_ui_manager_->GetVisiblePersistentChanges() &
                 date_dependencies) != 0) {
                const auto refresh = rawdraw_ui_manager_->PreparePersistentFrame();
                ESP_LOGI(kTag,
                         "Awake display invalidation: refresh=%d visible=0x%lx",
                         refresh.refresh_requested ? 1 : 0,
                         static_cast<unsigned long>(refresh.visible_changes));
                display_busy = lcd != nullptr && lcd->IsRefreshPending();
            }
        }

        if (sleep_phase_ == SleepPhase::WaitingForDisplay) {
            if (!display_busy) {
                ESP_LOGI(kTag, "EPD idle after final full-color refresh");
                sleep_phase_ = SleepPhase::ReadyToCommit;
            }
        }
        if (sleep_phase_ == SleepPhase::ReadyToCommit) {
            CommitLowPowerSleep();
        }

        if (!scheduler_timer_wake_ &&
            now_us >= external_power_probe_deadline_us_) {
            external_power_probe_deadline_us_ = now_us + 1000LL * 1000;
            const bool power_present = ZectrixIsExternalPowerPresent();
            if (power_present != external_power_present_) {
                ESP_LOGI(kTag, "External power %s",
                         power_present ? "connected; automatic sleep paused"
                                       : "removed; automatic sleep countdown resumed");
            }
            external_power_present_ = power_present;
            if (external_power_present_) {
                // Continuously move the idle deadline forward. After USB is
                // removed the device gets one normal interaction window, then
                // returns to the configured deep-sleep schedule.
                interactive_sleep_deadline_us_ = now_us + kInteractiveAwakeUs;
            }
        }

        if (scheduler_timer_wake_ && network_sync_pending_ &&
            network_sync_deadline_us_ > 0 && now_us >= network_sync_deadline_us_) {
            ESP_LOGW(kTag, "Scheduled network sync timed out; Wi-Fi stopped until next interval");
            if (network_initialized_) {
                WifiManager::GetInstance().StopStation();
            }
            wifi_connected_.store(false, std::memory_order_release);
            network_sync_pending_ = false;
        }

        if (!scheduler_timer_wake_ && wifi_connect_deadline_us_ > 0 &&
            now_us >= wifi_connect_deadline_us_ &&
            !wifi_connected_.load(std::memory_order_acquire)) {
            ESP_LOGW(kTag, "Interactive Wi-Fi attempt timed out; radio stopped");
            wifi_connect_deadline_us_ = 0;
            if (network_initialized_) {
                WifiManager::GetInstance().StopStation();
            }
            if (rawdraw_ui_manager_) {
                const bool changed = UpdateWifiSettingsItem(
                    rawdraw_ui_manager_->GetSettingsRenderer(),
                    wifi_enabled_.load(std::memory_order_acquire), false,
                    "连接超时");
                if (changed && !InInputScope()) {
                    rawdraw_ui_manager_->RequestActivePageRefresh(
                        ui::RefreshIntent::FastBwDeferredInteraction);
                }
            }
        }

        if (scheduler_timer_wake_) {
            if (!display_busy && !network_sync_pending_) {
                EnterScheduledSleep();
            }
        } else {
            const bool manual_sleep_requested =
                manual_sleep_requested_.load(std::memory_order_acquire);
            const bool idle_deadline_reached = interactive_sleep_deadline_us_ > 0 &&
                                               now_us >= interactive_sleep_deadline_us_;
            const bool local_service_running =
                IsLocalHttpServiceRunning(rawdraw_ui_manager_.get()) ||
                (network_initialized_ && WifiManager::GetInstance().IsConfigMode());
            const bool idle_sleep_allowed =
                ShouldEnterIdleSleep(idle_deadline_reached,
                                     external_power_present_) &&
                !local_service_running;
            if ((manual_sleep_requested || idle_sleep_allowed) && !display_busy) {
                EnterLowPowerSleep(manual_sleep_requested ? "manual request"
                                                          : "interactive idle timeout");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (callback) {
        callback();
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    auto* display = Board::GetInstance().GetDisplay();
    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    return !network_sync_pending_ &&
           !IsLocalHttpServiceRunning(rawdraw_ui_manager_.get()) &&
           (lcd == nullptr || !lcd->IsRefreshPending());
}

void Application::UpdateStatusBarForUi() {
    UpdateStatusBarForUi(true, false);
}

void Application::UpdateStatusBarForUi(bool request_refresh,
                                       bool fast_refresh) {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        const bool http_server_running = rawdraw_ui_manager_->IsHttpServerRunning();
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = http_server_running;
        data.battery_level = battery_level;
        data.battery_charging = charging;
        bool dirty = rawdraw_ui_manager_->UpdateStatusBar(data);
        auto* sr = rawdraw_ui_manager_->GetSettingsRenderer();
        // Evaluate every helper before OR-ing: || would short-circuit and skip
        // the remaining state pushes as soon as one reported a change.
        dirty |= UpdateWifiSettingsItem(
            sr, wifi_enabled_.load(std::memory_order_acquire), wifi_connected);
        dirty |= sr && sr->UpdateItem(
            kSettingsNetworkSyncIndex,
            FormatNetworkSyncLabel(
                network_sync_interval_minutes_,
                wifi_enabled_.load(std::memory_order_acquire)));
        const std::string lan_ip = wifi_connected ? WifiManager::GetInstance().GetIpAddress() : "";
        dirty |= UpdateLanIpSettingsItem(sr, lan_ip);
        dirty |= UpdateHttpServerSettingsItem(sr,
                                              rawdraw_ui_manager_->IsLanHttpServerRunning(),
                                              rawdraw_ui_manager_->IsLanHttpServerRunning()
                                                  ? lan_ip
                                                  : "");
        // Two independent reasons to stay quiet, both needed:
        //   - inside a button handler, that handler repaints on its way out;
        //   - nothing rendered actually changed, so a repaint would be a
        //     pixel-identical frame (repeat Scanning/Connecting events, or a
        //     battery poll that landed on the same percentage).
        const auto visible_changes =
            rawdraw_ui_manager_->GetVisiblePersistentChanges();
        if (dirty && visible_changes != 0 && request_refresh &&
            !scheduler_timer_wake_ && sleep_phase_ == SleepPhase::Awake &&
            !InInputScope()) {
            rawdraw_ui_manager_->RequestActivePageRefresh(
                fast_refresh
                    ? ui::RefreshIntent::FastBwDeferredInteraction
                    : ui::RefreshIntent::FullColor);
        }
    }
    return;
}
