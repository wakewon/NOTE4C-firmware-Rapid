#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include "audio_service.h"
#include "device_state.h"

namespace ui {
class RawDrawUiManager;
}

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_.load(std::memory_order_acquire); }
    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);
    void PlaySound(const std::string_view& sound);
    void PlaySound(const std::string_view& sound, int duration_ms);
    void MuteSound();
    void StopSound();
    bool CanEnterSleepMode() const;

    AudioService& GetAudioService() { return audio_service_; }
    ui::RawDrawUiManager* GetRawDrawUiManager() { return rawdraw_ui_manager_.get(); }
    bool IsInputReady() const {
        return input_ready_.load(std::memory_order_acquire);
    }

    void UpdateStatusBarForUi();
    void OnSntpSynchronized();
    void OnUpClick();
    void OnDownClick();
    void OnUpLongPress();
    void OnDownLongPress();
    void OnWifiConfigComboLongPress();
    void OnBootClick();
    void OnBootLongPress();
    void RequestManualSleep(bool disable_wifi = false);

private:
    Application();
    ~Application();

    std::atomic<DeviceState> state_{kDeviceStateUnknown};
    std::atomic<bool> wifi_connected_{false};
    std::atomic<bool> wifi_enabled_{false};
    std::atomic<bool> input_ready_{false};
    AudioService audio_service_;
    std::unique_ptr<ui::RawDrawUiManager> rawdraw_ui_manager_;
    bool audio_started_ = false;
    bool network_initialized_ = false;
    bool scheduler_timer_wake_ = false;
    bool slideshow_due_ = false;
    bool slideshow_refresh_requested_ = false;
    bool network_sync_pending_ = false;
    std::atomic<bool> manual_sleep_requested_{false};
    bool refresh_seen_busy_ = false;
    int slideshow_interval_minutes_ = 0;
    int network_sync_interval_minutes_ = 0;
    int64_t network_sync_deadline_us_ = 0;
    int64_t wifi_connect_deadline_us_ = 0;
    int64_t interactive_sleep_deadline_us_ = 0;
    int64_t external_power_probe_deadline_us_ = 0;
    bool external_power_present_ = false;

    void EnsureNetworkInitialized();
    void StartInteractiveWifiAttempt();
    void StartScheduledNetworkSync();
    void EnterLowPowerSleep(const char* reason);
    void EnterScheduledSleep();
    void EnterManualSleep();
    void NoteButtonActivity();
    void EnterWifiConfigMode();
    void UpdateStatusBarForUi(bool request_refresh, bool fast_refresh);

    // Held for the duration of a button handler. Every path reachable from one
    // ends by rendering the resulting state and refreshing it with the menu's
    // FAST_BW intent, so anything that runs underneath must not also queue a
    // background redraw -- that would repaint identical content a second time,
    // and RequestActivePageRefresh() escalates it to FULL_COLOR, preempting the
    // FAST_BW the handler chose.
    //
    // A flag rather than a parameter because the culprits are nested: calling
    // WifiManager::StopStation() from the Wi-Fi settings toggle raises
    // WifiEvent::Disconnected *synchronously*, so the network-event handler
    // runs inside the button handler, before it has finished applying the new
    // values -- it sees a genuine state change and cannot be filtered out by
    // comparing values alone.
    class InputScope {
    public:
        explicit InputScope(Application& app) : app_(app) {
            app_.input_scope_depth_.fetch_add(1, std::memory_order_acq_rel);
        }
        ~InputScope() { app_.input_scope_depth_.fetch_sub(1, std::memory_order_acq_rel); }
        InputScope(const InputScope&) = delete;
        InputScope& operator=(const InputScope&) = delete;

    private:
        Application& app_;
    };

    bool InInputScope() const {
        return input_scope_depth_.load(std::memory_order_acquire) > 0;
    }

    std::atomic<int> input_scope_depth_{0};
};

#endif  // _APPLICATION_H_
