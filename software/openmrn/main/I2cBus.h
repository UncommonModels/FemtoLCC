// The I2C bus: the on-board MCP23018 at U3, and whatever is plugged into the
// Qwiic connector J3 (GND, 3.3V, SDA, SCL; pulled up by R2/R3 on the board).
//
// Expansion boards can be missing or unplugged at any moment. A missing one
// answers with a NACK at once, so none of these wait; the bus timeout set by
// i2cBegin() only matters if something holds the bus low.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Start the bus at 100 kHz, with the short timeout.
void i2cBegin();

// 400 kHz, or back to 100 kHz for a device that cannot keep up.
void i2cSetFast(bool fast);
bool i2cFast();

// True if something acknowledges the address.
bool i2cProbe(uint8_t address);

// Write or read registers: the register number, then the data. Chips that
// step the register on by themselves take several in one transfer.
bool i2cWrite(uint8_t address, uint8_t reg, const uint8_t* data, size_t len);
bool i2cRead(uint8_t address, uint8_t reg, uint8_t* data, size_t len);

// Bytes straight to and from a chip with no registers, such as the PCF8574.
bool i2cWriteRaw(uint8_t address, const uint8_t* data, size_t len);
bool i2cReadRaw(uint8_t address, uint8_t* data, size_t len);
