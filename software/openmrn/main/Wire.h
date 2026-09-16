// The slice of the Arduino Wire API that Expander.cpp uses.
//
// See Arduino.h for why this exists. The semantics that matter are kept
// faithfully, in particular the repeated start: Expander::readRegister does
//
//     beginTransmission(addr); write(reg); endTransmission(false);
//     requestFrom(addr, 1);
//
// which on a real Wire is one transaction with a repeated start rather than a
// write, a stop and then a read. endTransmission(false) here holds the bytes
// back and the following requestFrom() issues the combined write-then-read, so
// the bus sees what the MCP23018 expects.
//
//   Uncommon Models — https://uncommonmodels.com

#pragma once

#include <stdint.h>
#include <stddef.h>

class TwoWire
{
public:
    TwoWire();

    /// Starts the bus on the given pins at `frequency` Hz.
    bool begin(int sda, int scl, uint32_t frequency);

    /// Per-transfer timeout, in milliseconds.
    void setTimeOut(uint16_t ms);

    /// Changes the bus clock.
    void setClock(uint32_t frequency);

    /// Starts a write to `address`. The bytes are buffered until
    /// endTransmission().
    void beginTransmission(uint8_t address);

    /// Buffers one byte.
    size_t write(uint8_t value);

    /// Buffers `len` bytes.
    size_t write(const uint8_t *data, size_t len);

    /// Sends the buffered bytes. `stop` true ends the transaction; false holds
    /// them for the requestFrom() that follows, which then issues a repeated
    /// start.
    /// @return 0 on success, non-zero on error, as Wire does.
    uint8_t endTransmission(bool stop = true);

    /// Reads `len` bytes from `address` into the receive buffer.
    /// @return the number of bytes actually read.
    size_t requestFrom(int address, int len);

    /// Bytes still unread.
    int available();

    /// Next byte from the receive buffer, or -1.
    int read();

private:
    static const size_t BUFFER_SIZE = 66;   // the longest transfer plus slack

    uint8_t txAddress_;
    uint8_t txBuffer_[BUFFER_SIZE];
    size_t txLength_;
    bool txPending_;            // endTransmission(false) is waiting for a read

    uint8_t rxBuffer_[BUFFER_SIZE];
    size_t rxLength_;
    size_t rxIndex_;

    uint16_t timeoutMs_;
    bool started_;
};

extern TwoWire Wire;
