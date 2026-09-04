#include "display_stall_guard.h"

#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"

#include <atomic>
#include <cstdlib>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "RuntimeStallGuard";
constexpr int64_t kPollIntervalUs = 1000LL * 1000;
// A normal full-color refresh is roughly 20-30 s and the scheduled network
// budget is 35 s. These thresholds leave ample margin without allowing a bad
// lock, protocol callback, or controller BUSY signal to drain the battery for
// hours.
constexpr int64_t kScheduledDisplayBusyTimeoutUs = 90LL * 1000 * 1000;
constexpr int64_t kAbsoluteDisplayBusyTimeoutUs = 180LL * 1000 * 1000;
constexpr int64_t kMainProgressTimeoutUs = 150LL * 1000 * 1000;
constexpr int64_t kScheduledWakeResidencyTimeoutUs = 180LL * 1000 * 1000;

constexpr uint32_t kRecoveryMagic = 0x4E344752;  // "N4GR"

enum class RecoveryReason : uint32_t {
    None = 0,
    DisplayBusy,
    MainTaskStalled,
    ScheduledWakeOverrun,
};

struct RecoveryRecord {
    uint32_t magic;
    uint32_t pending;
    uint32_t total_count;
    RecoveryReason reason;
    RuntimeGuardPhase phase;
    uint32_t uptime_ms;
    uint32_t display_busy_ms;
    uint32_t main_stale_ms;
};

// The guard recovers with esp_restart(), not deep sleep. RTC_DATA is only
// guaranteed to retain its value across deep sleep; RTC_NOINIT also avoids
// startup reinitialization on software/watchdog resets. The magic field below
// makes consuming uninitialized power-on contents safe.
RTC_NOINIT_ATTR RecoveryRecord s_recovery_record;

std::atomic<int64_t> s_main_progress_us{0};
std::atomic<int64_t> s_phase_since_us{0};
std::atomic<int64_t> s_scheduled_wake_since_us{0};
std::atomic<RuntimeGuardPhase> s_phase{RuntimeGuardPhase::Boot};
std::atomic<CustomLcdDisplay*> s_display{nullptr};
std::atomic<bool> s_scheduled_wake{false};

TaskHandle_t s_guard_task = nullptr;

const char* PhaseName(RuntimeGuardPhase phase) {
    switch (phase) {
        case RuntimeGuardPhase::Boot: return "boot";
        case RuntimeGuardPhase::Initializing: return "initializing";
        case RuntimeGuardPhase::Running: return "running";
        case RuntimeGuardPhase::PreparingSleep: return "preparing_sleep";
        case RuntimeGuardPhase::CommittingSleep: return "committing_sleep";
    }
    return "unknown";
}

const char* ReasonName(RecoveryReason reason) {
    switch (reason) {
        case RecoveryReason::None: return "none";
        case RecoveryReason::DisplayBusy: return "display_busy";
        case RecoveryReason::MainTaskStalled: return "main_task_stalled";
        case RecoveryReason::ScheduledWakeOverrun: return "scheduled_wake_overrun";
    }
    return "unknown";
}

void QuiescePanelBeforeRestart() {
    // The refresh task may be stuck in a controller BUSY wait. Assert reset and
    // remove panel power before restarting the MCU so the SSD2683 is not left
    // driving an unfinished waveform across the software reset.
    gpio_hold_dis(static_cast<gpio_num_t>(EPD_RST_PIN));
    gpio_set_direction(static_cast<gpio_num_t>(EPD_RST_PIN), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(EPD_RST_PIN), 0);

    gpio_hold_dis(static_cast<gpio_num_t>(EPD_PWR_PIN));
    gpio_set_direction(static_cast<gpio_num_t>(EPD_PWR_PIN), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(EPD_PWR_PIN), 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

[[noreturn]] void RecoverFromStall(RecoveryReason reason,
                                   RuntimeGuardPhase phase,
                                   int64_t uptime_us,
                                   int64_t busy_for_us,
                                   int64_t main_stale_us) {
    // Do not log on the failing path: the stalled task could own the logging
    // lock or a blocked USB console could be the stall itself. Persist first;
    // the next clean boot reports the complete record.
    if (s_recovery_record.magic != kRecoveryMagic) {
        s_recovery_record = {};
        s_recovery_record.magic = kRecoveryMagic;
    }
    s_recovery_record.pending = 1;
    ++s_recovery_record.total_count;
    s_recovery_record.reason = reason;
    s_recovery_record.phase = phase;
    s_recovery_record.uptime_ms = static_cast<uint32_t>(uptime_us / 1000);
    s_recovery_record.display_busy_ms = static_cast<uint32_t>(busy_for_us / 1000);
    s_recovery_record.main_stale_ms = static_cast<uint32_t>(main_stale_us / 1000);
    QuiescePanelBeforeRestart();
    esp_restart();
    abort();
}

void GuardTask(void*) {
    int64_t display_busy_since_us = 0;

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        const RuntimeGuardPhase phase = s_phase.load(std::memory_order_acquire);
        CustomLcdDisplay* display = s_display.load(std::memory_order_acquire);
        const bool display_busy = display != nullptr && display->IsRefreshPending();

        if (display_busy) {
            if (display_busy_since_us == 0) {
                display_busy_since_us = now_us;
            }
        } else {
            display_busy_since_us = 0;
        }

        const int64_t busy_for_us = display_busy_since_us == 0
            ? 0 : now_us - display_busy_since_us;
        const int64_t last_progress_us =
            s_main_progress_us.load(std::memory_order_acquire);
        const int64_t main_stale_us = last_progress_us > 0
            ? now_us - last_progress_us : 0;
        const bool scheduled = s_scheduled_wake.load(std::memory_order_acquire);
        const int64_t scheduled_since_us =
            s_scheduled_wake_since_us.load(std::memory_order_acquire);
        const int64_t scheduled_for_us = scheduled && scheduled_since_us > 0
            ? now_us - scheduled_since_us : 0;
        const int64_t display_timeout_us = scheduled
            ? kScheduledDisplayBusyTimeoutUs : kAbsoluteDisplayBusyTimeoutUs;

        if (main_stale_us >= kMainProgressTimeoutUs) {
            RecoverFromStall(RecoveryReason::MainTaskStalled, phase, now_us,
                             busy_for_us, main_stale_us);
        }
        if (scheduled_for_us >= kScheduledWakeResidencyTimeoutUs) {
            RecoverFromStall(RecoveryReason::ScheduledWakeOverrun, phase, now_us,
                             busy_for_us, main_stale_us);
        }
        if (busy_for_us >= display_timeout_us) {
            RecoverFromStall(RecoveryReason::DisplayBusy, phase, now_us,
                             busy_for_us, main_stale_us);
        }

        vTaskDelay(pdMS_TO_TICKS(kPollIntervalUs / 1000));
    }
}

}  // namespace

void StartDisplayStallGuard() {
    if (s_guard_task != nullptr) return;
    const int64_t now_us = esp_timer_get_time();
    if (s_main_progress_us.load(std::memory_order_acquire) == 0) {
        s_main_progress_us.store(now_us, std::memory_order_release);
        s_phase_since_us.store(now_us, std::memory_order_release);
    }
    BaseType_t created = xTaskCreatePinnedToCore(
        GuardTask, "runtime_stall_guard", 3072, nullptr, 4, &s_guard_task, 0);
    if (created != pdPASS) {
        s_guard_task = nullptr;
        ESP_LOGE(kTag, "Failed to start display stall guard");
        return;
    }
    ESP_LOGI(kTag,
             "Guard started: main=%llds display=%lld/%llds scheduled_residency=%llds",
             static_cast<long long>(kMainProgressTimeoutUs / 1000000),
             static_cast<long long>(kScheduledDisplayBusyTimeoutUs / 1000000),
             static_cast<long long>(kAbsoluteDisplayBusyTimeoutUs / 1000000),
             static_cast<long long>(kScheduledWakeResidencyTimeoutUs / 1000000));
}

void RuntimeGuardNoteProgress(RuntimeGuardPhase phase) {
    const int64_t now_us = esp_timer_get_time();
    const RuntimeGuardPhase previous = s_phase.exchange(phase, std::memory_order_acq_rel);
    if (previous != phase || s_phase_since_us.load(std::memory_order_acquire) == 0) {
        s_phase_since_us.store(now_us, std::memory_order_release);
    }
    s_main_progress_us.store(now_us, std::memory_order_release);
}

void RuntimeGuardSetScheduledWake(bool scheduled_wake) {
    const bool previous = s_scheduled_wake.exchange(scheduled_wake,
                                                    std::memory_order_acq_rel);
    if (scheduled_wake && !previous) {
        s_scheduled_wake_since_us.store(esp_timer_get_time(),
                                        std::memory_order_release);
    } else if (!scheduled_wake) {
        s_scheduled_wake_since_us.store(0, std::memory_order_release);
    }
}

void RuntimeGuardRegisterDisplay(CustomLcdDisplay* display) {
    s_display.store(display, std::memory_order_release);
}

bool ConsumeDisplayStallRecoveryBoot() {
    if (s_recovery_record.magic != kRecoveryMagic) {
        s_recovery_record = {};
        s_recovery_record.magic = kRecoveryMagic;
        return false;
    }
    const bool pending = s_recovery_record.pending != 0;
    if (pending) {
        ESP_LOGW(kTag,
                 "Previous recovery: reason=%s phase=%s uptime=%ums display_busy=%ums main_stale=%ums total=%u",
                 ReasonName(s_recovery_record.reason),
                 PhaseName(s_recovery_record.phase),
                 static_cast<unsigned>(s_recovery_record.uptime_ms),
                 static_cast<unsigned>(s_recovery_record.display_busy_ms),
                 static_cast<unsigned>(s_recovery_record.main_stale_ms),
                 static_cast<unsigned>(s_recovery_record.total_count));
    }
    s_recovery_record.pending = 0;
    return pending;
}
