// MCP23018 port expander at U3.
//
// Port B carries the block driver control lines that did not fit on the ESP32:
// the four dcc_en lines, three of the four dir lines, and the status LED.
// Port A is uncommitted general-purpose I/O brought out as /P0../P7.
//
// The MCP23018's outputs are open drain. A low bit sinks current; a high bit
// lets the line float, so "on" for a load wired from the supply into the pin is
// a low bit. The internal pull-ups (GPPU) are about 100k.

#pragma once

#include <stdint.h>

class Expander {
public:
    explicit Expander(uint8_t address);

    bool begin();

    // Port B — driver control. Writes are cached and only pushed when a bit
    // actually changes, so setting a channel that is already in that state
    // costs nothing on the I2C bus.
    void setPortBBit(uint8_t bit, bool value);
    bool getPortBBit(uint8_t bit) const;
    bool flush();

    // Port A — general-purpose. Direction is per-bit: a 1 means input.
    bool setPortADirection(uint8_t mask);
    // Pull-ups per bit: a 1 turns the pull-up on.
    bool setPortAPullups(uint8_t mask);
    // Writes the output latch. Bits that are inputs ignore it, so this never
    // disturbs an input.
    bool writePortA(uint8_t value);
    // Reads the pins as they are, inputs and outputs both.
    bool readPortA(uint8_t& value);

    bool ok() const { return ok_; }

private:
    bool writeRegister(uint8_t reg, uint8_t value);
    bool readRegister(uint8_t reg, uint8_t& value);

    uint8_t address_;
    uint8_t portBShadow_;
    bool portBDirty_;
    bool ok_;
};
