#pragma once

#include <driver/gpio.h>

#include <mutex>

// The GPIO ISR service is shared by every pin interrupt on the board. Install
// it through here only: gpio_install_isr_service() logs an error for every
// call after the first, and the first call's flags apply to every handler.
// It is installed without ESP_INTR_FLAG_IRAM, so handlers may live in flash
// (the interrupt stays masked while the cache is disabled).
inline esp_err_t EnsureGpioIsrService() {
    static std::mutex mutex;
    static bool installed = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (installed) {
        return ESP_OK;
    }
    const esp_err_t ret = gpio_install_isr_service(0);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        installed = true;
        return ESP_OK;
    }
    return ret;
}
