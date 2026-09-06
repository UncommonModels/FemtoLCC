#include "Expander.h"
#include <Wire.h>

// MCP23018 registers, IOCON.BANK = 0 (the reset default).
static const uint8_t REG_IODIRA = 0x00;
static const uint8_t REG_IODIRB = 0x01;
static const uint8_t REG_GPIOA  = 0x12;
static const uint8_t REG_GPIOB  = 0x13;
static const uint8_t REG_OLATB  = 0x15;

Expander::Expander(uint8_t address)
    : address_(address), portBShadow_(0), portBDirty_(true), ok_(false) {}

bool Expander::begin() {
    // Port B drives the multiplexers, so every bit is an output and the shadow
    // starts at zero: all channels off, DC mode, forward.
    ok_ = writeRegister(REG_IODIRB, 0x00);
    if (!ok_) {
        return false;
    }
    portBShadow_ = 0x00;
    portBDirty_ = true;
    ok_ = flush();

    // Port A defaults to inputs — safe until the application decides otherwise.
    ok_ = ok_ && writeRegister(REG_IODIRA, 0xFF);
    return ok_;
}

void Expander::setPortBBit(uint8_t bit, bool value) {
    if (bit > 7) {
        return;
    }
    const uint8_t mask = (uint8_t)(1u << bit);
    const uint8_t next = value ? (uint8_t)(portBShadow_ | mask)
                               : (uint8_t)(portBShadow_ & ~mask);
    if (next != portBShadow_) {
        portBShadow_ = next;
        portBDirty_ = true;
    }
}

bool Expander::getPortBBit(uint8_t bit) const {
    return bit <= 7 && (portBShadow_ & (1u << bit)) != 0;
}

bool Expander::flush() {
    if (!portBDirty_) {
        return true;
    }
    if (!writeRegister(REG_OLATB, portBShadow_)) {
        ok_ = false;
        return false;
    }
    portBDirty_ = false;
    return true;
}

bool Expander::setPortADirection(uint8_t mask) {
    return writeRegister(REG_IODIRA, mask);
}

bool Expander::writePortA(uint8_t value) {
    return writeRegister(REG_GPIOA, value);
}

bool Expander::readPortA(uint8_t& value) {
    return readRegister(REG_GPIOA, value);
}

bool Expander::writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address_);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool Expander::readRegister(uint8_t reg, uint8_t& value) {
    Wire.beginTransmission(address_);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((int)address_, 1) != 1) {
        return false;
    }
    value = Wire.read();
    return true;
}
