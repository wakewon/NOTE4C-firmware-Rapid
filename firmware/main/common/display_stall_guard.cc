#include "display_stall_guard.h"

#include "application.h"
#include "board.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "DisplayStallGuard";
constexpr int64_t kPollIntervalUs = 1000LL * 1000;
// Normal NOTE4C full-color refreshes are roughly 20-30 s. If input is blocked
// and the display still reports busy for 90 s, the scheduled wake/sleep state
// machine is no longer making useful progress and must not be allowed to hang
// forever.
constexpr int64_t kBlockedInputTimeoutUs = 90LL * 1000 * 1000;
// Also recover an abnormal display transaction during an interactive session.
// This threshold is deliberately more conservative because input remains live.
constexpr int64_t kAbsoluteDisplayBusyTimeoutUs = 180LL * 1000 * 1000;

RTC_DATA_ATTR bool s_display_stall_recovery_boot = false;
TaskHandle_t s_guard_task = nullptr;

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

[[noreturn]] void RecoverFromDisplayStall(int64_t busy_for_us,
                                          int64_t blocked_for_us,
                                          bool input_ready) {
    ESP_LOGE(kTag,
             "Display stall detected: busy=%lldms blocked_input=%lldms input_ready=%d; restarting cleanly",
             static_cast<long long>(busy_for_us / 1000),
             static_cast<long long>(blocked_for_us / 1000),
             input_ready ? 1 : 0);
    s_display_stall_recovery_boot = true;
    QuiescePanelBeforeRestart();
    esp_restart();
    abort();
}

void GuardTask(void*) {
    int64_t display_busy_since_us = 0;
    int64_t blocked_input_busy_since_us = 0;

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        auto& app = Application::GetInstance();
        auto* display = Board::GetInstance().GetDisplay();
        auto* lcd = static_cast<CustomLcdDisplay*>(display);
        const bool input_ready = app.IsInputReady();
        const bool display_busy = lcd != nullptr && lcd->IsRefreshPending();

        if (display_busy) {
            if (display_busy_since_us == 0) {
                display_busy_since_us = now_us;
            }
        } else {
            display_busy_since_us = 0;
        }

        if (display_busy && !input_ready) {
            if (blocked_input_busy_since_us == 0) {
                blocked_input_busy_since_us = now_us;
            }
        } else {
            blocked_input_busy_since_us = 0;
        }

        const int64_t busy_for_us = display_busy_since_us == 0
            ? 0 : now_us - display_busy_since_us;
        const int64_t blocked_for_us = blocked_input_busy_since_us == 0
            ? 0 : now_us - blocked_input_busy_since_us;

        if (blocked_for_us >= kBlockedInputTimeoutUs ||
            busy_for_us >= kAbsoluteDisplayBusyTimeoutUs) {
            RecoverFromDisplayStall(busy_for_us, blocked_for_us, input_ready);
        }

        vTaskDelay(pdMS_TO_TICKS(kPollIntervalUs / 1000));
    }
}

}  // namespace

void StartDisplayStallGuard() {
    if (s_guard_task != nullptr) return;
    BaseType_t created = xTaskCreatePinnedToCore(
        GuardTask, "display_stall_guard", 3072, nullptr, 2, &s_guard_task, 0);
    if (created != pdPASS) {
        s_guard_task = nullptr;
        ESP_LOGE(kTag, "Failed to start display stall guard");
        return;
    }
    ESP_LOGI(kTag,
             "Display stall guard started: blocked-input=%llds absolute=%llds",
             static_cast<long long>(kBlockedInputTimeoutUs / 1000000),
             static_cast<long long>(kAbsoluteDisplayBusyTimeoutUs / 1000000));
}

bool ConsumeDisplayStallRecoveryBoot() {
    const bool pending = s_display_stall_recovery_boot;
    s_display_stall_recovery_boot = false;
    return pending;
}
