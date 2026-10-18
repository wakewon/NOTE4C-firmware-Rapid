#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "audio_service.h"
#include "device_state.h"

namespace ui {
class RawDrawUiManager;
}

enum class NetworkEvent;

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // display_stall_recovery_boot: the runtime guard restarted the device.
    // user_restart_bounce: this is the deep-sleep bounce main.cc inserts after
    // a requested esp_restart(); it reports a timer wake but is not a
    // scheduled one.
    void Initialize(bool display_stall_recovery_boot = false,
                    bool user_restart_bounce = false);
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

    // Cross-task request surface. Every one of these may be called from an
    // HTTP handler, a network event task or the button task; each only
    // publishes intent, and the app-task supervisor loop applies it. Nothing
    // outside the app task may drive the radio or the schedule directly.
    void RequestSlideshowIntervalChange(int minutes);
    void RequestWifiDisabled(const char* reason);
    void RequestManualSleep(bool disable_wifi = false);

    enum class ButtonAction : uint8_t {
        UpClick,
        DownClick,
        BootClick,
        UpLongPress,
        DownLongPress,
        BootLongPress,
        WifiConfigCombo,
    };
    // The only entry point for physical buttons. Called on the button task at
    // the moment of the event, which is where input readiness has to be
    // sampled (see the definition).
    void PostButtonAction(ButtonAction action);

private:
    Application();
    ~Application();

    // Why this boot happened, which decides what the first frame is and
    // whether the session is interactive. See Initialize().
    enum class BootKind : uint8_t {
        ColdBoot,       // power-on, USB/EN reset, or no usable retained state
        UserRestart,    // the settings restart, after main.cc's deep-sleep bounce
        ScheduledWake,  // RTC timer wake with a valid retained schedule
        ButtonWake,     // BOOT/DOWN EXT1 wake; the press is replayed
        ChargerWake,    // charger CHRG/FULL pin woke the device
        Recovery,       // panic, watchdog, brownout or stall-guard restart
    };

    std::atomic<DeviceState> state_{kDeviceStateUnknown};
    std::atomic<bool> wifi_connected_{false};
    std::atomic<bool> wifi_enabled_{false};
    std::atomic<bool> input_ready_{false};
    // True while a scheduled (timer) wake is still able to turn interactive:
    // a press during it promotes the session instead of being dropped.
    std::atomic<bool> scheduled_wake_accepts_input_{false};
    AudioService audio_service_;
    std::unique_ptr<ui::RawDrawUiManager> rawdraw_ui_manager_;
    BootKind boot_kind_ = BootKind::ColdBoot;
    bool audio_started_ = false;
    bool network_initialized_ = false;
    bool scheduler_timer_wake_ = false;
    bool slideshow_due_ = false;
    bool slideshow_refresh_requested_ = false;
    bool display_invalidation_due_ = false;
    bool display_invalidation_refresh_requested_ = false;
    bool network_sync_pending_ = false;
    uint32_t scheduled_network_jobs_pending_ = 0;
    static constexpr uint32_t kScheduledNetworkJobTimeSync = 1u << 0;
    std::atomic<bool> manual_sleep_requested_{false};
    std::atomic<bool> sntp_sync_pending_{false};
    std::atomic<bool> wifi_disable_requested_{false};
    // -1 means "no request". Written by cross-task requesters, consumed once
    // by the app task.
    std::atomic<int> requested_slideshow_interval_{-1};
    int slideshow_interval_minutes_ = 0;
    int network_sync_interval_minutes_ = 0;
    int64_t network_sync_deadline_us_ = 0;
    int64_t wifi_connect_deadline_us_ = 0;
    int64_t interactive_sleep_deadline_us_ = 0;
    int64_t external_power_probe_deadline_us_ = 0;
    int64_t display_freshness_probe_deadline_us_ = 0;
    // esp_timer time of the last physical button action (or of the start of
    // the interactive session). Drives the awake slideshow's return from idle
    // pages and the local-service idle timeout.
    int64_t last_input_us_ = 0;
    // Settings "重启" needs a second press inside this window.
    int64_t restart_confirm_deadline_us_ = 0;
    bool restart_requested_ = false;
    int64_t restart_requested_at_us_ = 0;
    bool external_power_present_ = false;
    // Battery protection (see kCriticalBatteryPercent).
    bool battery_critical_ = false;
    // Too many consecutive unexpected resets: paint nothing, start no radio.
    bool quiet_recovery_ = false;
    int battery_level_at_boot_ = -1;

    enum class SleepPhase {
        Awake,
        WaitingForDisplay,
        ReadyToCommit,
    };
    SleepPhase sleep_phase_ = SleepPhase::Awake;
    const char* pending_sleep_reason_ = nullptr;
    std::mutex scheduled_callbacks_mutex_;
    std::vector<std::function<void()>> scheduled_callbacks_;

    // Slideshow / network-sync schedule. One owner (this class), one deadline
    // per job, expressed in the RTC microsecond counter so a single absolute
    // value stays correct across deep sleep, an early GPIO wake and a long
    // awake session alike.
    void ResetSlideshowDeadline();
    void ResetNetworkSyncDeadline();
    void NormalizeScheduleDeadlines();
    void ApplySlideshowInterval(int minutes);
    bool IsSlideshowDue() const;
    int SecondsUntilSlideshow() const;
    int SecondsUntilNetworkSync() const;
    void PumpSlideshowSchedule();

    void EnsureNetworkInitialized();
    void ApplyPendingWifiDisableRequest();
    void DisableWifiAndPersist(const char* reason);
    void StartInteractiveWifiAttempt();
    void StartScheduledNetworkSync();
    void CompleteScheduledNetworkJob(uint32_t job);
    void FinishScheduledNetworkSession(const char* reason);
    void EnterLowPowerSleep(const char* reason);
    void CommitLowPowerSleep();
    void EnterScheduledSleep();
    void EnterManualSleep();
    void NoteButtonActivity();
    void EnterWifiConfigMode();
    void OnUpClick();
    void OnDownClick();
    void OnUpLongPress();
    void OnDownLongPress();
    void OnWifiConfigComboLongPress();
    void OnBootClick();
    void OnBootLongPress();
    void DispatchButtonAction(ButtonAction action);
    void HandleInputDuringScheduledWake(ButtonAction action);
    void PromoteScheduledWakeToInteractive(const char* reason);
    void StopIdleLocalServices(int64_t idle_us);
    void StopConfigApIfRunning();
    void OnRestartItemActivated();
    void ReturnToSlideshowBeforeSleep();
    void UpdateBatteryProtection(int level, bool external_power);
    void ShowLowBatteryNoticeBeforeSleep();
    void DrainScheduledCallbacks();
    void HandleNetworkEvent(NetworkEvent event, const std::string& data);
    void HandleSntpSynchronized();
    void UpdateStatusBarForUi(bool request_refresh, bool fast_refresh);

    // Held for the duration of a button handler. Every path reachable from one
    // ends by rendering the resulting state and refreshing it with the menu's
    // FAST_BW intent, so anything that runs underneath must not also queue a
    // background redraw -- that would repaint identical content a second time,
    // and RequestActivePageRefresh() escalates it to FULL_COLOR, preempting the
    // FAST_BW the handler chose.
    //
    // A flag rather than a parameter because the suppressed callers are not
    // the handler itself but everything it reaches: NoteButtonActivity() and
    // the settings-item callbacks all fold a fresh battery/Wi-Fi sample into
    // the frame through UpdateStatusBarForUi(), several levels down from the
    // On*Click() that will repaint on its way out. Those pushes see genuine
    // state changes, so comparing values cannot filter them out.
    //
    // (Board network events used to add a second, harder case by arriving
    // *inside* a handler: WifiManager::StopStation() raises
    // WifiEvent::Disconnected synchronously. They are now routed through
    // Schedule() and drained on a later tick, so only the direct callers above
    // remain.)
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
