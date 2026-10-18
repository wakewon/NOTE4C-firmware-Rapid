#include "charge_status.h"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "config.h"

namespace {
constexpr char kTag[] = "ChargeStatus";
int64_t NowMs() { return esp_timer_get_time() / 1000; }
}  // namespace

void ChargeStatus::Init(gpio_num_t detect_gpio, gpio_num_t full_gpio) {
    detect_gpio_ = detect_gpio;
    full_gpio_ = full_gpio;

    gpio_config_t cfg = {};
    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pin_bit_mask = (1ULL << detect_gpio_) | (1ULL << full_gpio_);
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));

    UpdateSnapshot(State::kNoPower, false, false, false);
    // Publish one real sample before returning. The LED task is constructed
    // ahead of this call, and the application seeds its first framebuffer from
    // a battery reading during initialization; both must see measured pins
    // rather than the kNoPower placeholder above.
    configured_.store(true, std::memory_order_release);
    Sample(NowMs());
}

void ChargeStatus::StartSampler() {
    if (sampler_task_ != nullptr) return;
    if (!configured_.load(std::memory_order_acquire)) {
        ESP_LOGE(kTag, "StartSampler() before Init(); pins not configured");
        return;
    }
    sampler_stop_requested_.store(false, std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(
        SamplerTask, "charge_status", 2048, this, 2, &sampler_task_, 0);
    if (created != pdPASS) {
        sampler_task_ = nullptr;
        ESP_LOGE(kTag, "Failed to start charge sampler; snapshot will not update");
        return;
    }
    ESP_LOGI(kTag, "Charge sampler started at %d ms", kSamplePeriodMs);
}

void ChargeStatus::StopSampler() {
    if (sampler_task_ == nullptr) return;
    sampler_stop_requested_.store(true, std::memory_order_release);
    // One sample period plus slack is enough for a cooperative exit. Falling
    // back to vTaskDelete() is safe -- the sampler owns no lock and no
    // allocation -- but it should not be the normal path.
    for (int waited_ms = 0; waited_ms < 300 && sampler_task_ != nullptr; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (sampler_task_ != nullptr) {
        TaskHandle_t task = sampler_task_;
        sampler_task_ = nullptr;
        vTaskDelete(task);
    }
}

void ChargeStatus::SamplerTask(void* arg) {
    auto* self = static_cast<ChargeStatus*>(arg);
    while (!self->sampler_stop_requested_.load(std::memory_order_acquire)) {
        self->Sample(NowMs());
        vTaskDelay(pdMS_TO_TICKS(self->low_power_cadence_.load(std::memory_order_acquire)
                                     ? kLowPowerSamplePeriodMs
                                     : kSamplePeriodMs));
    }
    self->sampler_task_ = nullptr;
    vTaskDelete(nullptr);
}

void ChargeStatus::Sample(int64_t now_ms) {
    if (!configured_.load(std::memory_order_acquire)) return;

    const bool detect_high = gpio_get_level(detect_gpio_) == CHARGE_DETECT_CHARGING_LEVEL;
    const bool full_high = gpio_get_level(full_gpio_) == 1;

    if (detect_high) {
        last_power_present_ms_ = now_ms;
        last_detect_seen_ms_ = now_ms;
        if (detect_high_start_ms_ < 0) {
            detect_high_start_ms_ = now_ms;
        }
    } else {
        detect_high_start_ms_ = -1;
    }

    if (full_high) {
        last_power_present_ms_ = now_ms;
        last_full_seen_ms_ = now_ms;
        if (full_high_start_ms_ < 0) {
            full_high_start_ms_ = now_ms;
        }
    } else {
        full_high_start_ms_ = -1;
    }

    const bool power_present = (last_power_present_ms_ >= 0) &&
        ((now_ms - last_power_present_ms_) <= kPowerPresentHoldMs);

    const bool detect_stable = (detect_high_start_ms_ >= 0) &&
        ((now_ms - detect_high_start_ms_) >= kStableHighMs);
    const bool full_stable = (full_high_start_ms_ >= 0) &&
        ((now_ms - full_high_start_ms_) >= kStableHighMs);

    const bool alt_seen = power_present &&
        (last_detect_seen_ms_ >= 0) && (last_full_seen_ms_ >= 0) &&
        ((now_ms - last_detect_seen_ms_) <= kAltWindowMs) &&
        ((now_ms - last_full_seen_ms_) <= kAltWindowMs);

    const bool no_battery = alt_seen && !detect_stable && !full_stable;

    State state = State::kNoPower;
    if (!power_present) {
        state = State::kNoPower;
    } else if (full_stable && !no_battery) {
        state = State::kFull;
    } else if (detect_stable || no_battery) {
        state = no_battery ? State::kNoBattery : State::kCharging;
    } else {
        state = State::kCharging;
    }

    UpdateSnapshot(state, power_present, state == State::kFull, no_battery);
}

void ChargeStatus::UpdateSnapshot(State state, bool power_present, bool full, bool no_battery) {
    const bool charging = (state == State::kCharging || state == State::kNoBattery);
    snapshot_.store(Pack(state, power_present, charging, full, no_battery),
                    std::memory_order_release);
}

uint32_t ChargeStatus::Pack(State state, bool power_present, bool charging, bool full, bool no_battery) {
    return (uint32_t)state |
        ((uint32_t)power_present << 8) |
        ((uint32_t)charging << 9) |
        ((uint32_t)full << 10) |
        ((uint32_t)no_battery << 11);
}

ChargeStatus::Snapshot ChargeStatus::Unpack(uint32_t v) {
    Snapshot s{};
    s.state = static_cast<State>(v & 0xFF);
    s.power_present = (v >> 8) & 0x1;
    s.charging = (v >> 9) & 0x1;
    s.full = (v >> 10) & 0x1;
    s.no_battery = (v >> 11) & 0x1;
    return s;
}

ChargeStatus::Snapshot ChargeStatus::Get() const {
    return Unpack(snapshot_.load(std::memory_order_acquire));
}
