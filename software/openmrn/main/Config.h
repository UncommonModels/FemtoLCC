// The setting types the hardware drivers take.
//
// The drivers copied from ../FemtoLCC — Turnouts and IoPins — include
// "Config.h" for the handful of enums and structs describing what an output or
// an I/O line is set to. In the AOLCB firmware that header is the whole
// configuration-space layout and it pulls in <ConfigStorage.h> from AOLCB.
//
// This firmware keeps its settings in OpenMRN's own configuration file instead
// (see config.hxx), so nothing here describes a byte layout. It is just the
// vocabulary: the same enum names and values, and the same struct fields, so
// the driver copies compile unchanged. That is why it keeps the name Config.h —
// the copies are byte-identical to the originals, which is what makes
// sync-drivers.sh's drift check meaningful.
//
// The values match ../FemtoLCC/Config.h. Keep them in step: a stall motor is
// still 0 and a pulse motor still 1.
//
//   Uncommon Models — https://uncommonmodels.com

#pragma once

#include <stdint.h>

#include "board.h"

#define FEMTOLCC_OPENMRN_VERSION "0.1.0"

// ---------------------------------------------------------------------------
// What an output is for
// ---------------------------------------------------------------------------

enum class ChannelRole : uint8_t {
    Unused  = 0,    // driver held off, no events
    Block   = 1,    // a track block: DC or DCC, with occupancy detection
    Turnout = 2     // a turnout motor
};

enum class PowerOnMode : uint8_t { Off = 0, DC = 1, DCC = 2 };

enum class MotorType : uint8_t {
    Stall = 0,      // Tortoise and friends: driven continuously, one polarity per position
    Pulse = 1       // latching two-wire motor: one polarity for a moment, then off
};

enum class TurnoutPowerOn : uint8_t { Closed = 0, Thrown = 1, Leave = 2 };

enum class PinMode : uint8_t { Unused = 0, Input = 1, InputPullup = 2, Output = 3 };

// ---------------------------------------------------------------------------
// Settings, as the drivers use them
//
// Only the fields the copied drivers actually read are here. Turnouts::attach
// takes a ChannelSettings and uses motor, pulseMs, duty and reverse;
// PinBank::load takes PinSettings and uses mode, invert and debounceMs. The
// event IDs that the AOLCB structs also carry live in the OpenMRN configuration
// instead, so they are not repeated.
// ---------------------------------------------------------------------------

struct ChannelSettings {
    ChannelRole role;

    PowerOnMode powerOn;
    bool dccReversed;
    uint16_t occupiedMa;
    uint16_t clearMa;

    MotorType motor;
    uint16_t pulseMs;
    uint8_t duty;
    bool reverse;
    TurnoutPowerOn turnoutPowerOn;
};

struct PinSettings {
    PinMode mode;
    bool invert;
    uint16_t debounceMs;
};
