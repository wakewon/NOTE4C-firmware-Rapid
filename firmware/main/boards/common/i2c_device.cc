#include "i2c_device.h"

#include <esp_log.h>
#include <algorithm>
#include <array>

#include "i2c_bus_lock.h"
#include "i2c_power_hook.h"

#define TAG "I2cDevice"

constexpr int kI2cTimeoutMs = 100;
constexpr size_t kMaxRegisterWriteLength = 256;

I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : i2c_bus_(i2c_bus), device_address_(addr) {
    ScopedI2cBusLock bus_lock("I2cDevice::I2cDevice");
    ESP_ERROR_CHECK(bus_lock.status());
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_));
    assert(i2c_device_ != NULL);
}

esp_err_t I2cDevice::ResetBus(const char* reason) {
    ScopedI2cBusLock bus_lock("I2cDevice::ResetBus");
    if (!bus_lock.locked()) {
        return bus_lock.status();
    }
    ESP_LOGW(TAG, "i2c bus reset: reason=%s addr=0x%02X",
             reason ? reason : "unknown",
             static_cast<unsigned>(device_address_));
    esp_err_t ret = i2c_master_bus_reset(i2c_bus_);
    ESP_LOGW(TAG, "i2c bus reset done: ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t I2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    return WriteRegs(reg, &value, 1);
}

esp_err_t I2cDevice::WriteRegs(uint8_t start_reg, const uint8_t* values,
                               size_t length) {
    if (values == nullptr || length == 0 ||
        length > kMaxRegisterWriteLength) {
        return ESP_ERR_INVALID_ARG;
    }
    ScopedI2cBusLock bus_lock("I2cDevice::WriteReg");
    if (!bus_lock.locked()) {
        return bus_lock.status();
    }
    std::array<uint8_t, kMaxRegisterWriteLength + 1> buffer{};
    buffer[0] = start_reg;
    std::copy(values, values + length, buffer.begin() + 1);
    esp_err_t ret = ESP_OK;
    if (BoardI2cForcePowerOn()) {
        // The external pull-ups disappeared while the rail was off. Reset the
        // controller FSM after they have settled instead of making the first
        // real transaction fail and relying on its retry for recovery.
        ret = i2c_master_bus_reset(i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2c reset after power restore failed: ret=%s",
                     esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "i2c bus ready after shared-rail restore");
    }
    ret = i2c_master_transmit(i2c_device_, buffer.data(), length + 1,
                              kI2cTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "i2c write failed: addr=0x%02X reg=0x%02X len=%u ret=%s",
                 static_cast<unsigned>(device_address_),
                 static_cast<unsigned>(start_reg),
                 static_cast<unsigned>(length),
                 esp_err_to_name(ret));
        if (ResetBus("write_retry") == ESP_OK) {
            BoardI2cForcePowerOn();
            ret = i2c_master_transmit(i2c_device_, buffer.data(), length + 1,
                                      kI2cTimeoutMs);
            ESP_LOGW(TAG,
                     "i2c write retry result: addr=0x%02X reg=0x%02X len=%u ret=%s",
                     static_cast<unsigned>(device_address_),
                     static_cast<unsigned>(start_reg),
                     static_cast<unsigned>(length),
                     esp_err_to_name(ret));
        }
    }
    return ret;
}

esp_err_t I2cDevice::ReadReg(uint8_t reg, uint8_t* value) {
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return ReadRegs(reg, value, 1);
}

esp_err_t I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    if (buffer == nullptr || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ScopedI2cBusLock bus_lock("I2cDevice::ReadRegs");
    if (!bus_lock.locked()) {
        return bus_lock.status();
    }
    esp_err_t ret = ESP_OK;
    if (BoardI2cForcePowerOn()) {
        ret = i2c_master_bus_reset(i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2c reset after power restore failed: ret=%s",
                     esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "i2c bus ready after shared-rail restore");
    }
    ret = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length,
                                      kI2cTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "i2c read failed: addr=0x%02X reg=0x%02X len=%u ret=%s",
                 static_cast<unsigned>(device_address_),
                 static_cast<unsigned>(reg),
                 static_cast<unsigned>(length),
                 esp_err_to_name(ret));
        if (ResetBus("read_retry") == ESP_OK) {
            BoardI2cForcePowerOn();
            ret = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer,
                                              length, kI2cTimeoutMs);
            ESP_LOGW(TAG,
                     "i2c read retry result: addr=0x%02X reg=0x%02X len=%u ret=%s",
                     static_cast<unsigned>(device_address_),
                     static_cast<unsigned>(reg),
                     static_cast<unsigned>(length),
                     esp_err_to_name(ret));
        }
    }
    return ret;
}
