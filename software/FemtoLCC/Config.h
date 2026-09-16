// Configuration: the layout of the LCC configuration space, and the settings
// read out of it.
//
// A configuration tool such as JMRI edits the node through the OpenLCB Memory
// Configuration protocol. It reads the CDI (Cdi.cpp), which describes every
// field below by offset and size, and reads and writes memory space 253 (0xFD)
// directly. The space is kept in NVS flash by AOLCB::NvsConfigStorage.
//
// Tools read integers and event IDs most significant byte first, so the space
// is a byte layout, not a C struct: every field has an offset here and is read
// and written through the big-endian helpers. software/tools/check_cdi.py walks
// the CDI and checks each field lands on the offset named here - run it after
// changing either.
//
//   0     ACDI user block: version byte, node name, description (AOLCB)
//   128   layout magic, 4 bytes, not shown in the CDI
//   132   output channels A-D, CFG_CH_SIZE bytes each
//   504   expander pins P0-P7, CFG_PIN_SIZE bytes each
//   792   WiFi
//   989   USB port
//   990   expansion magic, 4 bytes, not shown in the CDI
//   994   I/O expansion boards 1-4, CFG_XIO_SIZE bytes each
//   3306  servo/light boards 1-2, CFG_SVB_SIZE bytes each
//   4592  detection while off, CFG_PD_SIZE bytes per output channel
//   4608  end of layout, and of the space
//
// Everything from 990 on was added after the first layout, so it has a magic
// word of its own. A board updated from a firmware without it loads the old
// 1024-byte space into the start of this one, finds that magic missing, and
// has only the expansion defaults written (addExpansionDefaults), keeping its
// name, WiFi and outputs.
//
// The detection fields at 4592 came later still. They sit inside that same
// expansion area, so a board coming from a firmware older than the expansion
// magic has their defaults written along with everything else - but a board
// that already has the magic does not, because nothing rewrites an area whose
// magic is already right. Zero therefore means "the default" in every field
// there, and the default for the mode byte is on: an upgraded board starts
// pulsing its blocks without being configured.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <ConfigStorage.h>
#include "board.h"

class Print;

#define FEMTOLCC_VERSION "0.3.0"

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// NVS is 20 KB and a commit writes the new copy before dropping the old, so
// keep this at or below 4608.
static const uint32_t CFG_SPACE_SIZE = 4608;

// Written with the defaults and checked at boot. Bump the low byte whenever a
// field moves, so a firmware update resets an old layout instead of misreading
// it. "FLC" then the layout number.
static const uint32_t CFG_MAGIC_OFFSET = 128;
static const uint32_t CFG_LAYOUT_MAGIC = 0x464C4301;

// Output channels. Offsets inside one channel's group.
static const uint32_t CFG_CHANNELS            = 132;
static const uint32_t CFG_CH_ROLE             = 0;    // u8, ChannelRole
static const uint32_t CFG_CH_EV_FAULT         = 1;    // event, produced
static const uint32_t CFG_CH_BLK_POWER_ON     = 9;    // u8, PowerOnMode
static const uint32_t CFG_CH_BLK_DCC_REVERSED = 10;   // u8, 0/1
static const uint32_t CFG_CH_BLK_OCCUPIED_MA  = 11;   // u16
static const uint32_t CFG_CH_BLK_CLEAR_MA     = 13;   // u16
static const uint32_t CFG_CH_BLK_EV_ON        = 15;   // event, consumed
static const uint32_t CFG_CH_BLK_EV_OFF       = 23;   // event, consumed
static const uint32_t CFG_CH_BLK_EV_DCC       = 31;   // event, consumed
static const uint32_t CFG_CH_BLK_EV_OCCUPIED  = 39;   // event, produced
static const uint32_t CFG_CH_BLK_EV_CLEAR     = 47;   // event, produced
static const uint32_t CFG_CH_TO_MOTOR         = 55;   // u8, MotorType
static const uint32_t CFG_CH_TO_PULSE_MS      = 56;   // u16
static const uint32_t CFG_CH_TO_DUTY          = 58;   // u8
static const uint32_t CFG_CH_TO_REVERSE       = 59;   // u8, 0/1
static const uint32_t CFG_CH_TO_POWER_ON      = 60;   // u8, TurnoutPowerOn
static const uint32_t CFG_CH_TO_EV_THROW      = 61;   // event, consumed
static const uint32_t CFG_CH_TO_EV_CLOSE      = 69;   // event, consumed
static const uint32_t CFG_CH_TO_EV_THROWN     = 77;   // event, produced
static const uint32_t CFG_CH_TO_EV_CLOSED     = 85;   // event, produced
static const uint32_t CFG_CH_SIZE             = 93;

// Expander port A pins. Offsets inside one pin's group.
static const uint32_t CFG_PINS                = 504;
static const uint32_t CFG_PIN_MODE            = 0;    // u8, PinMode
static const uint32_t CFG_PIN_INVERT          = 1;    // u8, 0/1
static const uint32_t CFG_PIN_DEBOUNCE_MS     = 2;    // u16
static const uint32_t CFG_PIN_EV_ACTIVE       = 4;    // event, produced
static const uint32_t CFG_PIN_EV_INACTIVE     = 12;   // event, produced
static const uint32_t CFG_PIN_EV_ON           = 20;   // event, consumed
static const uint32_t CFG_PIN_EV_OFF          = 28;   // event, consumed
static const uint32_t CFG_PIN_SIZE            = 36;

// WiFi. Strings are NUL-terminated inside a fixed field, so each holds one
// character fewer than its size.
static const uint32_t CFG_WIFI                = 792;
static const uint32_t CFG_WIFI_ENABLE         = 0;    // u8, 0/1
static const uint32_t CFG_WIFI_SSID           = 1;
static const uint32_t CFG_WIFI_PASSWORD       = 34;
static const uint32_t CFG_WIFI_HOSTNAME       = 98;
static const uint32_t CFG_WIFI_HUB_MODE       = 130;  // u8, HubMode
static const uint32_t CFG_WIFI_HUB_HOST       = 131;
static const uint32_t CFG_WIFI_HUB_PORT       = 195;  // u16
static const uint32_t CFG_WIFI_SIZE           = 197;

static const uint32_t CFG_WIFI_SSID_SIZE      = 33;
static const uint32_t CFG_WIFI_PASSWORD_SIZE  = 64;
static const uint32_t CFG_WIFI_HOSTNAME_SIZE  = 32;
static const uint32_t CFG_WIFI_HUB_HOST_SIZE  = 64;

// USB port.
static const uint32_t CFG_USB                 = 989;
static const uint32_t CFG_USB_GRIDCONNECT     = 0;    // u8, UsbGridConnect
static const uint32_t CFG_USB_SIZE            = 1;

// Expansion area magic. "FLX" then the expansion layout number; bump it only
// if a field after 990 moves.
static const uint32_t CFG_EXP_MAGIC_OFFSET    = 990;
static const uint32_t CFG_EXP_MAGIC           = 0x464C5801;

// Boards on the Qwiic connector J3.
static const uint8_t NUM_XIO_BOARDS           = 4;
static const uint8_t XIO_LINES                = 16;   // 8-line chips use the first 8
static const uint8_t NUM_SERVO_BOARDS         = 2;
static const uint8_t SERVO_CHANNELS           = 16;

// I/O expansion boards. Each line is laid out exactly like an expander pin,
// CFG_PIN_* offsets and all.
static const uint32_t CFG_XIO                 = 994;
static const uint32_t CFG_XIO_TYPE            = 0;    // u8, IoBoardType
static const uint32_t CFG_XIO_ADDRESS         = 1;    // u8, 7-bit I2C address
static const uint32_t CFG_XIO_LINES           = 2;    // XIO_LINES pin groups
static const uint32_t CFG_XIO_SIZE            = 578;

// Servo/light boards. Offsets inside one board, then inside one channel.
static const uint32_t CFG_SVB                 = 3306;
static const uint32_t CFG_SVB_TYPE            = 0;    // u8, ServoBoardType
static const uint32_t CFG_SVB_ADDRESS         = 1;    // u8, 7-bit I2C address
static const uint32_t CFG_SVB_HOLD            = 2;    // u8, 0 stop pulses at rest, 1 hold
static const uint32_t CFG_SVB_CHANNELS        = 3;    // SERVO_CHANNELS channel groups
static const uint32_t CFG_SVB_SIZE            = 643;

// A channel is 40 bytes so that both boards fit under 4608. That is why power-on
// and light polarity are folded into the use, and travel and fade share a field.
static const uint32_t CFG_SV_USE              = 0;    // u8, ServoUse
static const uint32_t CFG_SV_EV_THROW         = 1;    // event, consumed: throw / light on
static const uint32_t CFG_SV_EV_CLOSE         = 9;    // event, consumed: close / light off
static const uint32_t CFG_SV_TIME_MS          = 17;   // u16, servo travel or light fade
static const uint32_t CFG_SV_CLOSED_US        = 19;   // u16
static const uint32_t CFG_SV_THROWN_US        = 21;   // u16
static const uint32_t CFG_SV_EV_THROWN        = 23;   // event, produced
static const uint32_t CFG_SV_EV_CLOSED        = 31;   // event, produced
static const uint32_t CFG_SV_BRIGHTNESS       = 39;   // u8
static const uint32_t CFG_SV_SIZE             = 40;

// Detection while a block is switched off, one group per output channel, in
// the sixteen bytes left at the end of the space. Zero means the default in
// every field here - see the note at the top of this file.
static const uint32_t CFG_PULSE               = 4592;
static const uint32_t CFG_PD_MODE             = 0;    // u8, PulseDetect
static const uint32_t CFG_PD_LENGTH           = 1;    // u8, pulse length, 100 us units
static const uint32_t CFG_PD_INTERVAL         = 2;    // u16, ms between pulses
static const uint32_t CFG_PD_SIZE             = 4;

static const uint32_t CFG_END = CFG_PULSE + NUM_CHANNELS * CFG_PD_SIZE;

static_assert(CFG_CHANNELS == CFG_MAGIC_OFFSET + 4, "channels follow the magic");
static_assert(CFG_PINS == CFG_CHANNELS + NUM_CHANNELS * CFG_CH_SIZE, "pins follow channels");
static_assert(CFG_WIFI == CFG_PINS + EXP_GPIO_COUNT * CFG_PIN_SIZE, "WiFi follows pins");
static_assert(CFG_USB == CFG_WIFI + CFG_WIFI_SIZE, "USB follows WiFi");
static_assert(CFG_EXP_MAGIC_OFFSET == CFG_USB + CFG_USB_SIZE, "expansion magic follows USB");
static_assert(CFG_XIO == CFG_EXP_MAGIC_OFFSET + 4, "I/O boards follow the expansion magic");
static_assert(CFG_XIO_SIZE == CFG_XIO_LINES + XIO_LINES * CFG_PIN_SIZE, "I/O board size");
static_assert(CFG_SVB == CFG_XIO + NUM_XIO_BOARDS * CFG_XIO_SIZE, "servo boards follow I/O boards");
static_assert(CFG_SVB_SIZE == CFG_SVB_CHANNELS + SERVO_CHANNELS * CFG_SV_SIZE, "servo board size");
static_assert(CFG_SV_SIZE == CFG_SV_BRIGHTNESS + 1, "servo channel size");
static_assert(CFG_PULSE == CFG_SVB + NUM_SERVO_BOARDS * CFG_SVB_SIZE,
              "detection while off follows the servo boards");
static_assert(CFG_END <= CFG_SPACE_SIZE, "layout overflows the configuration space");
static_assert(CFG_SPACE_SIZE <= 4608, "NVS has room for two copies of 4608 bytes, no more");

// Absolute offset of a field in channel ch or pin p.
static inline uint32_t cfgChannel(uint8_t ch, uint32_t field) { return CFG_CHANNELS + ch * CFG_CH_SIZE + field; }
static inline uint32_t cfgPin(uint8_t pin, uint32_t field)    { return CFG_PINS + pin * CFG_PIN_SIZE + field; }
static inline uint32_t cfgWifi(uint32_t field)                { return CFG_WIFI + field; }
static inline uint32_t cfgUsb(uint32_t field)                 { return CFG_USB + field; }

// Absolute offset of a field in I/O board b, of a line's pin field on it, of
// a field in servo board b, and of a field in one of its channels.
static inline uint32_t cfgXio(uint8_t b, uint32_t field) { return CFG_XIO + b * CFG_XIO_SIZE + field; }
static inline uint32_t cfgXioLine(uint8_t b, uint8_t line, uint32_t field) {
    return cfgXio(b, CFG_XIO_LINES) + line * CFG_PIN_SIZE + field;
}
static inline uint32_t cfgSvb(uint8_t b, uint32_t field) { return CFG_SVB + b * CFG_SVB_SIZE + field; }
static inline uint32_t cfgSv(uint8_t b, uint8_t ch, uint32_t field) {
    return cfgSvb(b, CFG_SVB_CHANNELS) + ch * CFG_SV_SIZE + field;
}

// Absolute offset of a detection field for channel ch.
static inline uint32_t cfgPulse(uint8_t ch, uint32_t field) { return CFG_PULSE + ch * CFG_PD_SIZE + field; }

// ---------------------------------------------------------------------------
// Field values
// ---------------------------------------------------------------------------

enum class ChannelRole : uint8_t {
    Unused  = 0,    // driver held off, no events
    Block   = 1,    // a track block: DC or DCC, with occupancy detection
    Turnout = 2     // a turnout motor
};

enum class PowerOnMode : uint8_t { Off = 0, DC = 1, DCC = 2 };

// Whether a block that is switched off is pulsed so it can still be detected.
// On is zero, the other way round from every other setting here, because these
// bytes are already zero on a board that has the expansion magic: see the top
// of this file.
enum class PulseDetect : uint8_t { On = 0, Off = 1 };

enum class MotorType : uint8_t {
    Stall = 0,      // Tortoise and friends: driven continuously, one polarity per position
    Pulse = 1       // latching twin-coil or two-wire motor: one polarity for a moment, then off
};

enum class TurnoutPowerOn : uint8_t { Closed = 0, Thrown = 1, Leave = 2 };

enum class PinMode : uint8_t { Unused = 0, Input = 1, InputPullup = 2, Output = 3 };

enum class HubMode : uint8_t {
    Server = 0,     // listen for JMRI and other nodes on the TCP port
    Client = 1      // connect out to a hub at host:port
};

// Whether LCC traffic is copied to the USB console as GridConnect.
enum class UsbGridConnect : uint8_t {
    Auto = 0,       // off until a GridConnect frame arrives from the computer
    On   = 1,       // from power-on
    Off  = 2        // console only
};

// The chip on an I/O expansion board.
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
// MCP23018, so no I/O board may use it, whatever the chip.
bool ioBoardAddressValid(IoBoardType type, uint8_t address);
bool servoBoardAddressValid(uint8_t address);

// Lines the chip has: 16, 8, or 0 for none.
uint8_t ioBoardLines(IoBoardType type);
const char* ioBoardName(IoBoardType type);

// ---------------------------------------------------------------------------
// Settings, as the firmware uses them
// ---------------------------------------------------------------------------

struct ChannelSettings {
    ChannelRole role;
    uint64_t evFault;

    PowerOnMode powerOn;
    bool dccReversed;
    uint16_t occupiedMa;
    uint16_t clearMa;
    uint64_t evOn, evOff, evDcc, evOccupied, evClear;

    bool detectWhileOff;            // pulse the block when it is not powered
    uint16_t detectPulseUs;         // already resolved from the stored units
    uint16_t detectIntervalMs;

    MotorType motor;
    uint16_t pulseMs;
    uint8_t duty;
    bool reverse;
    TurnoutPowerOn turnoutPowerOn;
    uint64_t evThrow, evClose, evThrown, evClosed;
};

struct PinSettings {
    PinMode mode;
    bool invert;
    uint16_t debounceMs;
    uint64_t evActive, evInactive, evOn, evOff;
};

struct WifiSettings {
    bool enabled;
    char ssid[CFG_WIFI_SSID_SIZE];
    char password[CFG_WIFI_PASSWORD_SIZE];
    char hostname[CFG_WIFI_HOSTNAME_SIZE];
    HubMode hubMode;
    char hubHost[CFG_WIFI_HUB_HOST_SIZE];
    uint16_t hubPort;
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

struct Settings {
    ChannelSettings channels[NUM_CHANNELS];
    PinSettings pins[EXP_GPIO_COUNT];
    WifiSettings wifi;
    UsbGridConnect usbGridConnect;
    IoBoardSettings ioBoards[NUM_XIO_BOARDS];
    ServoBoardSettings servoBoards[NUM_SERVO_BOARDS];
};

// Read every field. Returns false, leaving `out` untouched, if the layout
// magic is missing - a blank space, or one written by an older firmware.
// Without the expansion magic the expansion boards read as none fitted.
bool loadSettings(AOLCB::ConfigStorage& storage, Settings& out);

// Fill the space with defaults and both magics. keepUserInfo leaves the
// node name and description alone, so a layout change does not lose them.
// Call commit() afterwards.
void writeDefaults(AOLCB::ConfigStorage& storage, uint64_t nodeId, bool keepUserInfo);

// Write the expansion area's defaults and magic if the magic is missing,
// touching nothing before it. True if it wrote them; call commit() then.
bool addExpansionDefaults(AOLCB::ConfigStorage& storage, uint64_t nodeId);

// A readable summary for the console. The WiFi password is not printed.
void printSettings(const Settings& s, Print& out);

// Event IDs: node ID in the top six bytes, suffix in the low two.
static inline uint64_t eventFor(uint64_t nodeId, uint16_t suffix) {
    return (nodeId << 16) | suffix;
}

// ---------------------------------------------------------------------------
// Big-endian field access
// ---------------------------------------------------------------------------

uint8_t  cfgRead8(AOLCB::ConfigStorage& s, uint32_t offset);
uint16_t cfgRead16(AOLCB::ConfigStorage& s, uint32_t offset);
uint32_t cfgRead32(AOLCB::ConfigStorage& s, uint32_t offset);
uint64_t cfgReadEvent(AOLCB::ConfigStorage& s, uint32_t offset);
void     cfgReadString(AOLCB::ConfigStorage& s, uint32_t offset, char* out, size_t size);

void cfgWrite8(AOLCB::ConfigStorage& s, uint32_t offset, uint8_t value);
void cfgWrite16(AOLCB::ConfigStorage& s, uint32_t offset, uint16_t value);
void cfgWrite32(AOLCB::ConfigStorage& s, uint32_t offset, uint32_t value);
void cfgWriteEvent(AOLCB::ConfigStorage& s, uint32_t offset, uint64_t value);
// Writes the whole field: the string, then zeros to the end.
void cfgWriteString(AOLCB::ConfigStorage& s, uint32_t offset, const char* value, size_t size);

// ---------------------------------------------------------------------------
// The configuration space as the LCC node sees it
// ---------------------------------------------------------------------------

// Forwards to the real storage, except that the WiFi password is write-only:
// it reads back as zeros, and a write that starts with a zero byte - a tool
// sending back the blank it read - is dropped rather than wiping the password.
// The firmware itself reads the real storage.
class LccConfigView : public AOLCB::ConfigStorage {
public:
    explicit LccConfigView(AOLCB::ConfigStorage& backing) : backing_(backing) {}

    uint32_t size() const override { return backing_.size(); }
    size_t read(uint32_t offset, uint8_t* data, size_t len) override;
    size_t write(uint32_t offset, const uint8_t* data, size_t len) override;
    void commit() override { backing_.commit(); }

private:
    AOLCB::ConfigStorage& backing_;
};

// The CDI XML, served from memory space 0xFF. Defined in Cdi.cpp.
extern const char CDI_XML[];
