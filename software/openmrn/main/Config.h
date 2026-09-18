// The setting types the hardware drivers take.
//
// Turnouts and IoPins include "Config.h" for the handful of enums and structs
// describing what an output or an I/O line is set to.
//
// The settings themselves live in OpenMRN's own configuration file (see
// config.hxx), so nothing here describes a byte layout. This is only the
// vocabulary: the enum names and values, and the struct fields, that the
// hardware drivers are written against.
//
// The numbers are load-bearing. FemtoCdi.hxx renders them as the <relation>
// entries a configuration tool draws its drop-downs from, and a saved setting
// is stored as the number, so renumbering one silently changes what every
// board already configured means by it. A stall motor is 0; a pulse motor 1.
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
// Boards on the Qwiic connector J3
//
// The IoBoards and ServoBoards drivers size their arrays from these.
// ---------------------------------------------------------------------------

static const uint8_t NUM_XIO_BOARDS   = 4;
static const uint8_t XIO_LINES        = 16;   // 8-line chips use the first 8
static const uint8_t NUM_SERVO_BOARDS = 2;
static const uint8_t SERVO_CHANNELS   = 16;

/// The chip on an I/O expansion board.
enum class IoBoardType : uint8_t {
    None     = 0,
    MCP23017 = 1,   // 16 lines, pull-ups, 0x21-0x27
    PCF8574  = 2,   // 8 lines, quasi-bidirectional, 0x21-0x27, or 0x38-0x3F for the A
    TCA9534  = 3    // 8 lines, no pull-ups, 0x21-0x27, or 0x38-0x3F for the A (and PCA9554)
};

enum class ServoBoardType : uint8_t { None = 0, PCA9685 = 1 };

// What a servo board channel drives, and how it starts. Light polarity rides
// along too: an inverted light is lit when its output is low.
enum class ServoUse : uint8_t {
    Unused          = 0,
    ServoClosed     = 1,    // servo turnout, closed at power-on
    ServoThrown     = 2,    //                thrown at power-on
    ServoLeave      = 3,    //                not driven until commanded
    LightOff        = 4,    // light, off at power-on
    LightOn         = 5,    //        on at power-on
    LightInvertOff  = 6,    // inverted light, off at power-on
    LightInvertOn   = 7     //                 on at power-on
};
static const uint8_t SERVO_USE_COUNT = 8;

static inline bool useIsServo(ServoUse u) { return u >= ServoUse::ServoClosed && u <= ServoUse::ServoLeave; }
static inline bool useIsLight(ServoUse u) { return u >= ServoUse::LightOff; }
static inline bool useInverted(ServoUse u) { return u >= ServoUse::LightInvertOff; }
// Where the channel goes at power-on: thrown/on, closed/off, or Leave.
static inline TurnoutPowerOn usePowerOn(ServoUse u) {
    switch (u) {
    case ServoUse::ServoThrown: case ServoUse::LightOn: case ServoUse::LightInvertOn:
        return TurnoutPowerOn::Thrown;
    case ServoUse::ServoLeave:
        return TurnoutPowerOn::Leave;
    default:
        return TurnoutPowerOn::Closed;
    }
}

// Whether a board of this type may sit at this address. 0x20 is the on-board
// MCP23018, so no I/O board may use it, whatever the chip. Defined in
// Config.cxx.
bool ioBoardAddressValid(IoBoardType type, uint8_t address);
bool servoBoardAddressValid(uint8_t address);

// Lines the chip has: 16, 8, or 0 for none.
uint8_t ioBoardLines(IoBoardType type);
const char* ioBoardName(IoBoardType type);

// ---------------------------------------------------------------------------
// Settings, as the drivers use them
//
// Only the fields the drivers actually read are here. Turnouts::attach takes a
// ChannelSettings and uses motor, pulseMs, duty and reverse; PinBank::load
// takes PinSettings and uses mode, invert and debounceMs. Event IDs live in the
// OpenMRN configuration instead, so they are not repeated here.
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

struct IoBoardSettings {
    IoBoardType type;
    uint8_t address;
    PinSettings lines[XIO_LINES];
};

struct ServoChannelSettings {
    uint64_t evThrow, evClose;      // also light on, light off
    uint64_t evThrown, evClosed;
    uint16_t timeMs;                // travel, or fade
    uint16_t closedUs, thrownUs;
    ServoUse use;
    uint8_t brightness;
};

struct ServoBoardSettings {
    ServoBoardType type;
    uint8_t address;
    bool hold;                      // keep pulsing servos at rest
    ServoChannelSettings channels[SERVO_CHANNELS];
};
