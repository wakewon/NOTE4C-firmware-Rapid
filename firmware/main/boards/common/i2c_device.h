#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <esp_err.h>
#include <driver/i2c_master.h>

class I2cDevice {
public:
    I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

protected:
    esp_err_t ResetBus(const char* reason);
    i2c_master_dev_handle_t i2c_device_;
    i2c_master_bus_handle_t i2c_bus_;
    uint8_t device_address_ = 0;

    esp_err_t WriteReg(uint8_t reg, uint8_t value);
    esp_err_t WriteRegs(uint8_t start_reg, const uint8_t* values, size_t length);
    esp_err_t ReadReg(uint8_t reg, uint8_t* value);
    esp_err_t ReadRegs(uint8_t reg, uint8_t* buffer, size_t length);
};

#endif // I2C_DEVICE_H
