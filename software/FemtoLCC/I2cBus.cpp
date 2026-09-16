#include "I2cBus.h"
#include <Wire.h>
#include "board.h"

// The longest transfer is 65 bytes to a servo board, about 6 ms at 100 kHz.
// The core's default is 50 ms.
static const uint16_t TIMEOUT_MS = 10;
static const uint32_t SLOW_HZ = 100000;
static const uint32_t FAST_HZ = 400000;

static bool fast = false;

void i2cBegin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, SLOW_HZ);
    Wire.setTimeOut(TIMEOUT_MS);
    fast = false;
}

void i2cSetFast(bool on) {
    if (on != fast) {
        Wire.setClock(on ? FAST_HZ : SLOW_HZ);
        fast = on;
    }
}

bool i2cFast() {
    return fast;
}

bool i2cProbe(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

bool i2cWrite(uint8_t address, uint8_t reg, const uint8_t* data, size_t len) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

bool i2cRead(uint8_t address, uint8_t reg, uint8_t* data, size_t len) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    return i2cReadRaw(address, data, len);
}

bool i2cWriteRaw(uint8_t address, const uint8_t* data, size_t len) {
    Wire.beginTransmission(address);
    Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

bool i2cReadRaw(uint8_t address, uint8_t* data, size_t len) {
    if (Wire.requestFrom((int)address, (int)len) != (int)len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        data[i] = Wire.read();
    }
    return true;
}
