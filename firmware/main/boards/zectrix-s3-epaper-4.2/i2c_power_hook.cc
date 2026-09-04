#include <driver/gpio.h>
#include <esp_rom_sys.h>

#include "boards/common/i2c_power_hook.h"
#include "config.h"

extern "C" bool BoardI2cForcePowerOn() {
    const gpio_num_t pin = static_cast<gpio_num_t>(Audio_PWR_PIN);
    const bool needs_settle = gpio_get_level(pin) != AUDIO_PWR_FORCE_LEVEL;
    gpio_hold_dis(pin);
    gpio_set_level(pin, AUDIO_PWR_FORCE_LEVEL);
    gpio_hold_en(pin);
    if (needs_settle) {
        // This rail also supplies the shared I2C pull-ups.  A scheduled wake
        // deliberately switches it off until an RTC access is needed; wait
        // for the rail and pull-ups to recover before issuing the first byte.
        esp_rom_delay_us(20 * 1000);
    }
    return needs_settle;
}
