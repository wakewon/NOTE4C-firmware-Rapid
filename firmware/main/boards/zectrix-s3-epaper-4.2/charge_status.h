#pragma once

#include <driver/gpio.h>
#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Debounced view of the charger's CHRG/FULL outputs.
//
// Ownership: exactly one task samples the pins. `Init()` configures them and
// takes the first sample on the caller's task, `StartSampler()` hands the pins
// to a dedicated task from then on, and every other task -- the LED task, the
// application supervisor loop, the factory test -- only ever calls `Get()`,
// which reads a packed atomic snapshot.
//
// The debounce state used to be advanced by whoever happened to ask for a
// reading: the LED task between its 0.2-3 s blink delays, the 1 Hz supervisor
// poll, the status-bar battery sample and the factory test. Those tasks
// interleaved unsynchronized read-modify-write sequences over the same
// timestamps, and the effective sample interval was whatever their delays
// happened to add up to, so the windows below (which assume a regular, fast
// cadence) could not do their job.
class ChargeStatus {
public:
    enum class State : uint8_t {
        kNoPower = 0,
        kCharging = 1,
        kFull = 2,
        kNoBattery = 3,
    };

    struct Snapshot {
        State state;
        bool power_present;
        bool charging;   // UI/LED charging indicator (includes no-battery)
        bool full;
        bool no_battery;
    };

    // Configures the pins and publishes a first snapshot synchronously, so
    // readers that run before StartSampler() still see a real sample.
    void Init(gpio_num_t detect_gpio, gpio_num_t full_gpio);

    // Starts / stops the single sampling task. Stopping is part of deep-sleep
    // preparation; the pins keep their level and EXT0/EXT1 take over as the
    // wake sources.
    void StartSampler();
    void StopSampler();

    // Slows sampling to kLowPowerSamplePeriodMs while the application's
    // low-power wait lets the chip light-sleep (see the constant).
    void SetLowPowerCadence(bool enabled) {
        low_power_cadence_.store(enabled, std::memory_order_release);
    }

    Snapshot Get() const;

private:
    void Sample(int64_t now_ms);
    void UpdateSnapshot(State state, bool power_present, bool full, bool no_battery);
    static void SamplerTask(void* arg);
    static uint32_t Pack(State state, bool power_present, bool charging, bool full, bool no_battery);
    static Snapshot Unpack(uint32_t v);

    gpio_num_t detect_gpio_ = GPIO_NUM_NC;
    gpio_num_t full_gpio_ = GPIO_NUM_NC;

    // Debounce state. Owned by Init() until StartSampler() runs, by the
    // sampler task afterwards; never touched by a reader.
    int64_t detect_high_start_ms_ = -1;
    int64_t full_high_start_ms_ = -1;
    int64_t last_detect_seen_ms_ = -1;
    int64_t last_full_seen_ms_ = -1;
    int64_t last_power_present_ms_ = -1;

    std::atomic<uint32_t> snapshot_{0};
    std::atomic<bool> configured_{false};
    std::atomic<bool> sampler_stop_requested_{false};
    std::atomic<bool> low_power_cadence_{false};
    TaskHandle_t sampler_task_ = nullptr;

    static constexpr int kPowerPresentHoldMs = 1000;
    static constexpr int kStableHighMs = 400;
    static constexpr int kAltWindowMs = 1500;
    // Fast enough that kStableHighMs is several samples wide and the
    // charger's CHRG/FULL alternation used for no-battery detection cannot
    // slip through kAltWindowMs unseen.
    static constexpr int kSamplePeriodMs = 100;
    // Only used on battery, while both pins are idle: 100 ms sampling was the
    // most frequent wake-up of the light-sleeping chip. A charger plugged in
    // is still seen within half a second, and the windows above are
    // timestamp-based, so they stay valid at this cadence.
    static constexpr int kLowPowerSamplePeriodMs = 500;
};
