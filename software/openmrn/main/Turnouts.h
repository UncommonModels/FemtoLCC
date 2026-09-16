// Turnout motors on the block driver outputs.
//
// Any of the four channels can drive a turnout motor instead of a track block.
// The H-bridge is what makes this work: DC drive in one direction or the other
// is exactly what a motor needs to throw or close.
//
//   stall motor   Tortoise, Cobalt and similar. Driven continuously in one
//                 direction per position, at a configurable duty so the motor
//                 can be run below the supply voltage.
//   pulse motor   latching motors that want one polarity for a moment. Driven
//                 for the pulse length, then the output is switched off.
//
// Closed is forward and thrown is reverse, unless the channel is configured
// reversed. Position is what was last commanded; there is no feedback.

#pragma once

#include <stdint.h>
#include "board.h"
#include "Channels.h"
#include "Config.h"

enum class TurnoutPosition : uint8_t {
    Unknown,    // not commanded since power-on
    Closed,
    Thrown
};

class Turnouts {
public:
    explicit Turnouts(Channels& channels);

    // Take a channel on as a turnout, or update its settings. A stall motor
    // already in position is re-driven so a new duty or direction applies.
    void attach(uint8_t channel, const ChannelSettings& settings);

    // Stop treating the channel as a turnout. The caller decides what the
    // channel does next.
    void detach(uint8_t channel);

    bool attached(uint8_t channel) const;

    // Drive to a position.
    void set(uint8_t channel, bool thrown);

    // Stop driving, keeping the position. Used after a driver fault; the
    // caller has already switched the channel off.
    void release(uint8_t channel);

    TurnoutPosition position(uint8_t channel) const;
    bool pulsing(uint8_t channel) const;

    // Ends pulses when they are due. Call every loop.
    void update();

private:
    void drive(uint8_t channel);

    struct Motor {
        bool attached;
        MotorType type;
        uint16_t pulseMs;
        uint8_t duty;
        bool reverse;
        TurnoutPosition position;
        bool pulsing;
        uint32_t pulseStart;
    };

    Channels& channels_;
    Motor motor_[NUM_CHANNELS];
};
