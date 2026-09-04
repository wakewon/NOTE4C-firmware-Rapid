#pragma once

// Ensure the board rail that supplies the shared I2C pull-ups is on. Returns
// true when this call restored the rail and the I2C controller should reset its
// state before the first transaction.
extern "C" bool BoardI2cForcePowerOn();
