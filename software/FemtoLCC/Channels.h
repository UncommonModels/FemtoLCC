// The four block driver channels.
//
// Each channel is an SN74HC253 multiplexer feeding a DRV8874 H-bridge. See
// board.h for the truth table; in short, dcc_en picks DC or DCC mode and dir
// picks direction (DC) or polarity (DCC).

#pragma once

#include <stdint.h>
#include "board.h"
#include "Expander.h"

enum class ChannelMode : uint8_t {
    Off,        // driver idle, block dead
    DC,         // PWM drive, dir selects forward or reverse
    DCC         // pass the isolated track signal through, dir selects polarity
};

class Channels {
public:
    explicit Channels(Expander& expander);

    void begin();

    // Drive a block. duty is 0-255 and only means anything in DC mode.
    void set(uint8_t channel, ChannelMode mode, bool reverse, uint8_t duty);

    void setMode(uint8_t channel, ChannelMode mode);
    void setReverse(uint8_t channel, bool reverse);
    void setDuty(uint8_t channel, uint8_t duty);

    // Cut every channel. Called on a driver fault and available to the LCC
    // emergency-off event.
    void allOff();

    ChannelMode mode(uint8_t channel) const;
    bool reversed(uint8_t channel) const;
    uint8_t duty(uint8_t channel) const;

    // DRV8874 nFAULT is open drain and active low: true here means the driver
    // is reporting overcurrent, overtemperature or undervoltage.
    bool faulted(uint8_t channel) const;

    // Current through the block, from the driver's IPROPI output.
    // Returns milliamps, using the DRV8874 IPROPI ratio and the 1.43k sense
    // resistor fitted at R7/R9/R11/R13.
    uint32_t currentMilliamps(uint8_t channel) const;

    // Push any pending expander writes. Call once per loop after setting
    // channels, so a multi-channel change costs a single I2C transaction.
    bool flush();

private:
    void applyDir(uint8_t channel, bool reverse);
    void applyMode(uint8_t channel, ChannelMode mode);

    Expander& expander_;
    ChannelMode mode_[NUM_CHANNELS];
    bool reverse_[NUM_CHANNELS];
    uint8_t duty_[NUM_CHANNELS];
};
