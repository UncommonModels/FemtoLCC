#include "Config.h"
#include <Arduino.h>
#include <string.h>

// Defaults. Thresholds match what Occupancy used before they were per channel.
static const uint16_t DEFAULT_OCCUPIED_MA = 5;
static const uint16_t DEFAULT_CLEAR_MA    = 3;

// Detection while a block is off. The length is stored in 100 us units, and a
// zero in any of these fields means the default - see Config.h.
static const uint8_t  DEFAULT_DETECT_LEN_100US   = 20;      // 2 ms
static const uint8_t  MIN_DETECT_LEN_100US       = 10;      // 1 ms
static const uint8_t  MAX_DETECT_LEN_100US       = 50;      // 5 ms
static const uint16_t DEFAULT_DETECT_INTERVAL_MS = 300;
static const uint16_t MIN_DETECT_INTERVAL_MS     = 100;
static const uint16_t MAX_DETECT_INTERVAL_MS     = 10000;
static const uint16_t DEFAULT_PULSE_MS    = 200;
static const uint8_t  DEFAULT_TURNOUT_DUTY = 255;
static const uint16_t DEFAULT_DEBOUNCE_MS = 20;
static const uint16_t DEFAULT_HUB_PORT    = 12021;

// Expansion boards. I/O board b defaults to 0x21 + b, servo board b to 0x40 + b.
// Positions a little either side of centre, so a servo fitted under a
// turnout does not strain against the throwbar before it is set up.
static const uint8_t  DEFAULT_XIO_ADDRESS   = 0x21;
static const uint8_t  DEFAULT_SVB_ADDRESS   = 0x40;
static const uint16_t DEFAULT_SERVO_TIME_MS = 1000;
static const uint16_t DEFAULT_CLOSED_US     = 1300;
static const uint16_t DEFAULT_THROWN_US     = 1700;
static const uint8_t  DEFAULT_BRIGHTNESS    = 255;

// Version byte at the start of the ACDI user block, fixed by the standard.
static const uint8_t ACDI_USER_VERSION = 2;

// Default event suffixes. n is the channel (0-3), p the expander pin (0-7).
static const uint16_t EV_CHANNEL_ON     = 0x0100;
static const uint16_t EV_CHANNEL_OFF    = 0x0110;
static const uint16_t EV_CHANNEL_DCC    = 0x0120;
static const uint16_t EV_CHANNEL_FAULT  = 0x0200;
static const uint16_t EV_BLOCK_OCCUPIED = 0x0300;
static const uint16_t EV_BLOCK_CLEAR    = 0x0310;
static const uint16_t EV_TURNOUT_THROW  = 0x0400;
static const uint16_t EV_TURNOUT_CLOSE  = 0x0410;
static const uint16_t EV_TURNOUT_THROWN = 0x0420;
static const uint16_t EV_TURNOUT_CLOSED = 0x0430;
static const uint16_t EV_PIN_ACTIVE     = 0x0500;
static const uint16_t EV_PIN_INACTIVE   = 0x0510;
static const uint16_t EV_PIN_ON         = 0x0600;
static const uint16_t EV_PIN_OFF        = 0x0610;

// Expansion boards: i is 16 * board + line, or 16 * board + channel, from 0.
static const uint16_t EV_XIO_ACTIVE     = 0x0700;
static const uint16_t EV_XIO_INACTIVE   = 0x0800;
static const uint16_t EV_XIO_ON         = 0x0900;
static const uint16_t EV_XIO_OFF        = 0x0A00;
static const uint16_t EV_SERVO_THROW    = 0x0B00;   // also light on
static const uint16_t EV_SERVO_CLOSE    = 0x0C00;   // also light off
static const uint16_t EV_SERVO_THROWN   = 0x0D00;
static const uint16_t EV_SERVO_CLOSED   = 0x0E00;

// --- expansion boards ----------------------------------------------------------

bool ioBoardAddressValid(IoBoardType type, uint8_t address) {
    const bool low = address >= 0x21 && address <= 0x27;     // 0x20 is U3
    const bool high = address >= 0x38 && address <= 0x3F;    // the A variants
    switch (type) {
    case IoBoardType::MCP23017: return low;
    case IoBoardType::PCF8574:
    case IoBoardType::TCA9534:  return low || high;
    default:                    return false;
    }
}

bool servoBoardAddressValid(uint8_t address) {
    return address >= 0x40 && address <= 0x7F;
}

uint8_t ioBoardLines(IoBoardType type) {
    switch (type) {
    case IoBoardType::MCP23017: return 16;
    case IoBoardType::PCF8574:
    case IoBoardType::TCA9534:  return 8;
    default:                    return 0;
    }
}

const char* ioBoardName(IoBoardType type) {
    switch (type) {
    case IoBoardType::MCP23017: return "MCP23017";
    case IoBoardType::PCF8574:  return "PCF8574";
    case IoBoardType::TCA9534:  return "TCA9534";
    default:                    return "none";
    }
}

// --- big-endian field access -------------------------------------------------

static uint64_t readBE(AOLCB::ConfigStorage& s, uint32_t offset, size_t bytes) {
    uint8_t buf[8] = {};
    s.read(offset, buf, bytes);
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; ++i) {
        v = (v << 8) | buf[i];
    }
    return v;
}

static void writeBE(AOLCB::ConfigStorage& s, uint32_t offset, uint64_t value, size_t bytes) {
    uint8_t buf[8];
    for (size_t i = bytes; i > 0; --i) {
        buf[i - 1] = (uint8_t)value;
        value >>= 8;
    }
    s.write(offset, buf, bytes);
}

uint8_t  cfgRead8(AOLCB::ConfigStorage& s, uint32_t o)     { return (uint8_t)readBE(s, o, 1); }
uint16_t cfgRead16(AOLCB::ConfigStorage& s, uint32_t o)    { return (uint16_t)readBE(s, o, 2); }
uint32_t cfgRead32(AOLCB::ConfigStorage& s, uint32_t o)    { return (uint32_t)readBE(s, o, 4); }
uint64_t cfgReadEvent(AOLCB::ConfigStorage& s, uint32_t o) { return readBE(s, o, 8); }

void cfgWrite8(AOLCB::ConfigStorage& s, uint32_t o, uint8_t v)     { writeBE(s, o, v, 1); }
void cfgWrite16(AOLCB::ConfigStorage& s, uint32_t o, uint16_t v)   { writeBE(s, o, v, 2); }
void cfgWrite32(AOLCB::ConfigStorage& s, uint32_t o, uint32_t v)   { writeBE(s, o, v, 4); }
void cfgWriteEvent(AOLCB::ConfigStorage& s, uint32_t o, uint64_t v) { writeBE(s, o, v, 8); }

void cfgReadString(AOLCB::ConfigStorage& s, uint32_t offset, char* out, size_t size) {
    if (size == 0) {
        return;
    }
    memset(out, 0, size);
    s.read(offset, (uint8_t*)out, size);
    out[size - 1] = '\0';     // a tool may fill the field to the last byte
}

void cfgWriteString(AOLCB::ConfigStorage& s, uint32_t offset, const char* value, size_t size) {
    uint8_t buf[CFG_WIFI_HUB_HOST_SIZE];     // the largest string field
    if (size > sizeof(buf)) {
        size = sizeof(buf);
    }
    memset(buf, 0, sizeof(buf));
    strncpy((char*)buf, value, size - 1);
    s.write(offset, buf, size);
}

// --- load and defaults -------------------------------------------------------

// Enumerations are range-checked, so a stray byte cannot select a mode the
// firmware has no code for.
static uint8_t readEnum(AOLCB::ConfigStorage& s, uint32_t offset, uint8_t count, uint8_t fallback) {
    const uint8_t v = cfgRead8(s, offset);
    return v < count ? v : fallback;
}

// Zero stands for the default, since a field nothing ever wrote reads as zero;
// anything else is held to the range the firmware can honour, because a tool
// can write whatever it likes.
static uint16_t orDefault(uint32_t v, uint16_t fallback, uint16_t lo, uint16_t hi) {
    if (v == 0) {
        v = fallback;
    }
    return v < lo ? lo : (v > hi ? hi : (uint16_t)v);
}

// One pin group, P0-P7 or an expansion line, starting at `base`.
static void readPin(AOLCB::ConfigStorage& s, uint32_t base, PinSettings& pin) {
    pin.mode       = (PinMode)readEnum(s, base + CFG_PIN_MODE, 4, 0);
    pin.invert     = cfgRead8(s, base + CFG_PIN_INVERT) != 0;
    pin.debounceMs = cfgRead16(s, base + CFG_PIN_DEBOUNCE_MS);
    pin.evActive   = cfgReadEvent(s, base + CFG_PIN_EV_ACTIVE);
    pin.evInactive = cfgReadEvent(s, base + CFG_PIN_EV_INACTIVE);
    pin.evOn       = cfgReadEvent(s, base + CFG_PIN_EV_ON);
    pin.evOff      = cfgReadEvent(s, base + CFG_PIN_EV_OFF);
}

static void writePinDefaults(AOLCB::ConfigStorage& s, uint32_t base, uint64_t nodeId,
                             uint16_t evActive, uint16_t evInactive, uint16_t evOn, uint16_t evOff) {
    cfgWrite8(s, base + CFG_PIN_MODE, (uint8_t)PinMode::Unused);
    cfgWrite8(s, base + CFG_PIN_INVERT, 0);
    cfgWrite16(s, base + CFG_PIN_DEBOUNCE_MS, DEFAULT_DEBOUNCE_MS);
    cfgWriteEvent(s, base + CFG_PIN_EV_ACTIVE,   eventFor(nodeId, evActive));
    cfgWriteEvent(s, base + CFG_PIN_EV_INACTIVE, eventFor(nodeId, evInactive));
    cfgWriteEvent(s, base + CFG_PIN_EV_ON,       eventFor(nodeId, evOn));
    cfgWriteEvent(s, base + CFG_PIN_EV_OFF,      eventFor(nodeId, evOff));
}

// Without the expansion magic the area holds zeros, or whatever an older
// firmware left there, so every board reads as not fitted.
static void loadExpansion(AOLCB::ConfigStorage& s, Settings& out) {
    const bool valid = cfgRead32(s, CFG_EXP_MAGIC_OFFSET) == CFG_EXP_MAGIC;

    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b) {
        IoBoardSettings& x = out.ioBoards[b];
        x.type    = valid ? (IoBoardType)readEnum(s, cfgXio(b, CFG_XIO_TYPE), 4, 0) : IoBoardType::None;
        x.address = cfgRead8(s, cfgXio(b, CFG_XIO_ADDRESS));
        for (uint8_t l = 0; l < XIO_LINES; ++l) {
            readPin(s, cfgXioLine(b, l, 0), x.lines[l]);
        }
    }

    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b) {
        ServoBoardSettings& v = out.servoBoards[b];
        v.type    = valid ? (ServoBoardType)readEnum(s, cfgSvb(b, CFG_SVB_TYPE), 2, 0) : ServoBoardType::None;
        v.address = cfgRead8(s, cfgSvb(b, CFG_SVB_ADDRESS));
        v.hold    = cfgRead8(s, cfgSvb(b, CFG_SVB_HOLD)) != 0;
        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            ServoChannelSettings& c = v.channels[ch];
            c.use        = (ServoUse)readEnum(s, cfgSv(b, ch, CFG_SV_USE), SERVO_USE_COUNT, 0);
            c.evThrow    = cfgReadEvent(s, cfgSv(b, ch, CFG_SV_EV_THROW));
            c.evClose    = cfgReadEvent(s, cfgSv(b, ch, CFG_SV_EV_CLOSE));
            c.timeMs     = cfgRead16(s, cfgSv(b, ch, CFG_SV_TIME_MS));
            c.closedUs   = cfgRead16(s, cfgSv(b, ch, CFG_SV_CLOSED_US));
            c.thrownUs   = cfgRead16(s, cfgSv(b, ch, CFG_SV_THROWN_US));
            c.evThrown   = cfgReadEvent(s, cfgSv(b, ch, CFG_SV_EV_THROWN));
            c.evClosed   = cfgReadEvent(s, cfgSv(b, ch, CFG_SV_EV_CLOSED));
            c.brightness = cfgRead8(s, cfgSv(b, ch, CFG_SV_BRIGHTNESS));
        }
    }
}

// Every expansion field and the expansion magic. The area must be zeroed.
static void writeExpansion(AOLCB::ConfigStorage& s, uint64_t nodeId) {
    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b) {
        cfgWrite8(s, cfgXio(b, CFG_XIO_TYPE), (uint8_t)IoBoardType::None);
        cfgWrite8(s, cfgXio(b, CFG_XIO_ADDRESS), DEFAULT_XIO_ADDRESS + b);
        for (uint8_t l = 0; l < XIO_LINES; ++l) {
            const uint16_t i = b * XIO_LINES + l;
            writePinDefaults(s, cfgXioLine(b, l, 0), nodeId, EV_XIO_ACTIVE + i,
                             EV_XIO_INACTIVE + i, EV_XIO_ON + i, EV_XIO_OFF + i);
        }
    }

    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b) {
        cfgWrite8(s, cfgSvb(b, CFG_SVB_TYPE), (uint8_t)ServoBoardType::None);
        cfgWrite8(s, cfgSvb(b, CFG_SVB_ADDRESS), DEFAULT_SVB_ADDRESS + b);
        cfgWrite8(s, cfgSvb(b, CFG_SVB_HOLD), 0);
        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            const uint16_t i = b * SERVO_CHANNELS + ch;
            cfgWrite8(s, cfgSv(b, ch, CFG_SV_USE), (uint8_t)ServoUse::Unused);
            cfgWriteEvent(s, cfgSv(b, ch, CFG_SV_EV_THROW),  eventFor(nodeId, EV_SERVO_THROW + i));
            cfgWriteEvent(s, cfgSv(b, ch, CFG_SV_EV_CLOSE),  eventFor(nodeId, EV_SERVO_CLOSE + i));
            cfgWrite16(s, cfgSv(b, ch, CFG_SV_TIME_MS), DEFAULT_SERVO_TIME_MS);
            cfgWrite16(s, cfgSv(b, ch, CFG_SV_CLOSED_US), DEFAULT_CLOSED_US);
            cfgWrite16(s, cfgSv(b, ch, CFG_SV_THROWN_US), DEFAULT_THROWN_US);
            cfgWriteEvent(s, cfgSv(b, ch, CFG_SV_EV_THROWN), eventFor(nodeId, EV_SERVO_THROWN + i));
            cfgWriteEvent(s, cfgSv(b, ch, CFG_SV_EV_CLOSED), eventFor(nodeId, EV_SERVO_CLOSED + i));
            cfgWrite8(s, cfgSv(b, ch, CFG_SV_BRIGHTNESS), DEFAULT_BRIGHTNESS);
        }
    }

    // Detection while a block is off. These groups belong to a channel, but
    // they live at the end of the space, inside the expansion area, so they are
    // written here rather than with the rest of each channel's settings.
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        cfgWrite8(s, cfgPulse(ch, CFG_PD_MODE), (uint8_t)PulseDetect::On);
        cfgWrite8(s, cfgPulse(ch, CFG_PD_LENGTH), DEFAULT_DETECT_LEN_100US);
        cfgWrite16(s, cfgPulse(ch, CFG_PD_INTERVAL), DEFAULT_DETECT_INTERVAL_MS);
    }

    cfgWrite32(s, CFG_EXP_MAGIC_OFFSET, CFG_EXP_MAGIC);
}

bool addExpansionDefaults(AOLCB::ConfigStorage& s, uint64_t nodeId) {
    if (cfgRead32(s, CFG_EXP_MAGIC_OFFSET) == CFG_EXP_MAGIC) {
        return false;
    }
    uint8_t zeros[64] = {};
    for (uint32_t o = CFG_EXP_MAGIC_OFFSET; o < s.size(); o += sizeof(zeros)) {
        s.write(o, zeros, sizeof(zeros));
    }
    writeExpansion(s, nodeId);
    return true;
}

bool loadSettings(AOLCB::ConfigStorage& s, Settings& out) {
    if (cfgRead32(s, CFG_MAGIC_OFFSET) != CFG_LAYOUT_MAGIC) {
        return false;
    }

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        ChannelSettings& c = out.channels[ch];
        c.role        = (ChannelRole)readEnum(s, cfgChannel(ch, CFG_CH_ROLE), 3, 0);
        c.evFault     = cfgReadEvent(s, cfgChannel(ch, CFG_CH_EV_FAULT));
        c.powerOn     = (PowerOnMode)readEnum(s, cfgChannel(ch, CFG_CH_BLK_POWER_ON), 3, 0);
        c.dccReversed = cfgRead8(s, cfgChannel(ch, CFG_CH_BLK_DCC_REVERSED)) != 0;
        c.occupiedMa  = cfgRead16(s, cfgChannel(ch, CFG_CH_BLK_OCCUPIED_MA));
        c.clearMa     = cfgRead16(s, cfgChannel(ch, CFG_CH_BLK_CLEAR_MA));
        c.evOn        = cfgReadEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_ON));
        c.evOff       = cfgReadEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_OFF));
        c.evDcc       = cfgReadEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_DCC));
        c.evOccupied  = cfgReadEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_OCCUPIED));
        c.evClear     = cfgReadEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_CLEAR));

        // Detection while the block is off, from the groups at the end of the
        // space. Zero means the default in all three, so a board whose spare
        // bytes were never written still pulses.
        c.detectWhileOff = readEnum(s, cfgPulse(ch, CFG_PD_MODE), 2, 0) ==
                           (uint8_t)PulseDetect::On;
        c.detectPulseUs = (uint16_t)(orDefault(cfgRead8(s, cfgPulse(ch, CFG_PD_LENGTH)),
                                               DEFAULT_DETECT_LEN_100US,
                                               MIN_DETECT_LEN_100US,
                                               MAX_DETECT_LEN_100US) * 100u);
        c.detectIntervalMs = orDefault(cfgRead16(s, cfgPulse(ch, CFG_PD_INTERVAL)),
                                       DEFAULT_DETECT_INTERVAL_MS,
                                       MIN_DETECT_INTERVAL_MS, MAX_DETECT_INTERVAL_MS);
        c.motor       = (MotorType)readEnum(s, cfgChannel(ch, CFG_CH_TO_MOTOR), 2, 0);
        c.pulseMs     = cfgRead16(s, cfgChannel(ch, CFG_CH_TO_PULSE_MS));
        c.duty        = cfgRead8(s, cfgChannel(ch, CFG_CH_TO_DUTY));
        c.reverse     = cfgRead8(s, cfgChannel(ch, CFG_CH_TO_REVERSE)) != 0;
        c.turnoutPowerOn = (TurnoutPowerOn)readEnum(s, cfgChannel(ch, CFG_CH_TO_POWER_ON), 3, 0);
        c.evThrow     = cfgReadEvent(s, cfgChannel(ch, CFG_CH_TO_EV_THROW));
        c.evClose     = cfgReadEvent(s, cfgChannel(ch, CFG_CH_TO_EV_CLOSE));
        c.evThrown    = cfgReadEvent(s, cfgChannel(ch, CFG_CH_TO_EV_THROWN));
        c.evClosed    = cfgReadEvent(s, cfgChannel(ch, CFG_CH_TO_EV_CLOSED));
    }

    for (uint8_t p = 0; p < EXP_GPIO_COUNT; ++p) {
        readPin(s, cfgPin(p, 0), out.pins[p]);
    }

    WifiSettings& w = out.wifi;
    w.enabled = cfgRead8(s, cfgWifi(CFG_WIFI_ENABLE)) != 0;
    cfgReadString(s, cfgWifi(CFG_WIFI_SSID), w.ssid, sizeof(w.ssid));
    cfgReadString(s, cfgWifi(CFG_WIFI_PASSWORD), w.password, sizeof(w.password));
    cfgReadString(s, cfgWifi(CFG_WIFI_HOSTNAME), w.hostname, sizeof(w.hostname));
    w.hubMode = (HubMode)readEnum(s, cfgWifi(CFG_WIFI_HUB_MODE), 2, 0);
    cfgReadString(s, cfgWifi(CFG_WIFI_HUB_HOST), w.hubHost, sizeof(w.hubHost));
    w.hubPort = cfgRead16(s, cfgWifi(CFG_WIFI_HUB_PORT));
    if (w.hubPort == 0) {
        w.hubPort = DEFAULT_HUB_PORT;
    }

    out.usbGridConnect = (UsbGridConnect)readEnum(s, cfgUsb(CFG_USB_GRIDCONNECT), 3, 0);

    loadExpansion(s, out);
    return true;
}

void writeDefaults(AOLCB::ConfigStorage& s, uint64_t nodeId, bool keepUserInfo) {
    // Zero everything first, so spare bytes and string tails are clean.
    uint8_t zeros[64] = {};
    const uint32_t start = keepUserInfo ? AOLCB::CONFIG_APP_OFFSET : 0;
    for (uint32_t o = start; o < s.size(); o += sizeof(zeros)) {
        s.write(o, zeros, sizeof(zeros));
    }
    if (!keepUserInfo || cfgRead8(s, 0) != ACDI_USER_VERSION) {
        s.write(0, zeros, sizeof(zeros));
        s.write(sizeof(zeros), zeros, sizeof(zeros));
        cfgWrite8(s, 0, ACDI_USER_VERSION);
    }

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        cfgWrite8(s, cfgChannel(ch, CFG_CH_ROLE), (uint8_t)ChannelRole::Block);
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_EV_FAULT), eventFor(nodeId, EV_CHANNEL_FAULT + ch));

        cfgWrite8(s, cfgChannel(ch, CFG_CH_BLK_POWER_ON), (uint8_t)PowerOnMode::Off);
        cfgWrite8(s, cfgChannel(ch, CFG_CH_BLK_DCC_REVERSED), 0);
        cfgWrite16(s, cfgChannel(ch, CFG_CH_BLK_OCCUPIED_MA), DEFAULT_OCCUPIED_MA);
        cfgWrite16(s, cfgChannel(ch, CFG_CH_BLK_CLEAR_MA), DEFAULT_CLEAR_MA);
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_ON),       eventFor(nodeId, EV_CHANNEL_ON + ch));
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_OFF),      eventFor(nodeId, EV_CHANNEL_OFF + ch));
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_DCC),      eventFor(nodeId, EV_CHANNEL_DCC + ch));
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_OCCUPIED), eventFor(nodeId, EV_BLOCK_OCCUPIED + ch));
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_BLK_EV_CLEAR),    eventFor(nodeId, EV_BLOCK_CLEAR + ch));

        cfgWrite8(s, cfgChannel(ch, CFG_CH_TO_MOTOR), (uint8_t)MotorType::Stall);
        cfgWrite16(s, cfgChannel(ch, CFG_CH_TO_PULSE_MS), DEFAULT_PULSE_MS);
        cfgWrite8(s, cfgChannel(ch, CFG_CH_TO_DUTY), DEFAULT_TURNOUT_DUTY);
        cfgWrite8(s, cfgChannel(ch, CFG_CH_TO_REVERSE), 0);
        cfgWrite8(s, cfgChannel(ch, CFG_CH_TO_POWER_ON), (uint8_t)TurnoutPowerOn::Closed);
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_TO_EV_THROW),  eventFor(nodeId, EV_TURNOUT_THROW + ch));
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_TO_EV_CLOSE),  eventFor(nodeId, EV_TURNOUT_CLOSE + ch));
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_TO_EV_THROWN), eventFor(nodeId, EV_TURNOUT_THROWN + ch));
        cfgWriteEvent(s, cfgChannel(ch, CFG_CH_TO_EV_CLOSED), eventFor(nodeId, EV_TURNOUT_CLOSED + ch));
    }

    for (uint8_t p = 0; p < EXP_GPIO_COUNT; ++p) {
        writePinDefaults(s, cfgPin(p, 0), nodeId, EV_PIN_ACTIVE + p, EV_PIN_INACTIVE + p,
                         EV_PIN_ON + p, EV_PIN_OFF + p);
    }

    // Hostname from the low two bytes of the node ID, so two boards on one
    // network do not collide out of the box.
    char hostname[CFG_WIFI_HOSTNAME_SIZE];
    snprintf(hostname, sizeof(hostname), "femtolcc-%04x", (unsigned)(nodeId & 0xFFFF));
    cfgWrite8(s, cfgWifi(CFG_WIFI_ENABLE), 0);
    cfgWriteString(s, cfgWifi(CFG_WIFI_HOSTNAME), hostname, CFG_WIFI_HOSTNAME_SIZE);
    cfgWrite8(s, cfgWifi(CFG_WIFI_HUB_MODE), (uint8_t)HubMode::Server);
    cfgWrite16(s, cfgWifi(CFG_WIFI_HUB_PORT), DEFAULT_HUB_PORT);

    cfgWrite8(s, cfgUsb(CFG_USB_GRIDCONNECT), (uint8_t)UsbGridConnect::Auto);

    writeExpansion(s, nodeId);
    cfgWrite32(s, CFG_MAGIC_OFFSET, CFG_LAYOUT_MAGIC);
}

// --- console summary ---------------------------------------------------------

static void printEvent(Print& out, const char* label, uint64_t id) {
    out.printf("      %-9s ", label);
    for (int i = 7; i >= 0; --i) {
        out.printf(i ? "%02X." : "%02X", (unsigned)((id >> (8 * i)) & 0xFF));
    }
    out.println();
}

void printSettings(const Settings& s, Print& out) {
    static const char* const powerOn[] = { "off", "DC", "DCC" };
    static const char* const toPowerOn[] = { "closed", "thrown", "left alone" };
    static const char* const pinMode[] = { "unused", "input", "input, pull-up", "output" };

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        const ChannelSettings& c = s.channels[ch];
        switch (c.role) {
        case ChannelRole::Block:
            out.printf("  %c  block: power-on %s, DCC polarity %s, occupied >= %u mA, clear < %u mA\n",
                       'A' + ch, powerOn[(uint8_t)c.powerOn],
                       c.dccReversed ? "reversed" : "normal", c.occupiedMa, c.clearMa);
            if (c.detectWhileOff) {
                out.printf("      detect while off: on, %u us every %u ms\n",
                           c.detectPulseUs, c.detectIntervalMs);
            } else {
                out.println(F("      detect while off: off"));
            }
            printEvent(out, "on", c.evOn);
            printEvent(out, "off", c.evOff);
            printEvent(out, "dcc", c.evDcc);
            printEvent(out, "occupied", c.evOccupied);
            printEvent(out, "clear", c.evClear);
            printEvent(out, "fault", c.evFault);
            break;
        case ChannelRole::Turnout:
            if (c.motor == MotorType::Stall) {
                out.printf("  %c  turnout: stall motor, duty %u", 'A' + ch, c.duty);
            } else {
                out.printf("  %c  turnout: pulse motor, %u ms at duty %u", 'A' + ch, c.pulseMs, c.duty);
            }
            out.printf(", %s, power-on %s\n", c.reverse ? "reversed" : "normal",
                       toPowerOn[(uint8_t)c.turnoutPowerOn]);
            printEvent(out, "throw", c.evThrow);
            printEvent(out, "close", c.evClose);
            printEvent(out, "thrown", c.evThrown);
            printEvent(out, "closed", c.evClosed);
            printEvent(out, "fault", c.evFault);
            break;
        default:
            out.printf("  %c  unused\n", 'A' + ch);
            break;
        }
    }

    for (uint8_t p = 0; p < EXP_GPIO_COUNT; ++p) {
        const PinSettings& pin = s.pins[p];
        if (pin.mode == PinMode::Unused) {
            continue;
        }
        out.printf("  P%u %s%s", p, pinMode[(uint8_t)pin.mode], pin.invert ? ", inverted" : "");
        if (pin.mode == PinMode::Output) {
            out.println();
            printEvent(out, "on", pin.evOn);
            printEvent(out, "off", pin.evOff);
        } else {
            out.printf(", debounce %u ms\n", pin.debounceMs);
            printEvent(out, "active", pin.evActive);
            printEvent(out, "inactive", pin.evInactive);
        }
    }

    const WifiSettings& w = s.wifi;
    out.printf("  WiFi %s, SSID '%s', password %s, hostname %s\n",
               w.enabled ? "enabled" : "disabled", w.ssid,
               w.password[0] ? "set" : "not set", w.hostname);
    if (w.hubMode == HubMode::Server) {
        out.printf("  hub: listening on port %u\n", w.hubPort);
    } else {
        out.printf("  hub: connecting to %s:%u\n", w.hubHost, w.hubPort);
    }

    static const char* const usb[] = { "automatic", "always on", "off" };
    out.printf("  USB GridConnect output: %s\n", usb[(uint8_t)s.usbGridConnect]);

    // Expansion boards without their events: sixty-four lines and thirty-two
    // channels of them would not fit the console's buffer. JMRI shows them.
    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b) {
        const IoBoardSettings& x = s.ioBoards[b];
        if (x.type == IoBoardType::None) {
            continue;
        }
        out.printf("  I/O board %u: %s at 0x%02X%s\n", b + 1, ioBoardName(x.type), x.address,
                   ioBoardAddressValid(x.type, x.address) ? "" : " - address not allowed");
        for (uint8_t l = 0; l < ioBoardLines(x.type); ++l) {
            const PinSettings& pin = x.lines[l];
            if (pin.mode == PinMode::Unused) {
                continue;
            }
            out.printf("    line %u %s%s", l + 1, pinMode[(uint8_t)pin.mode], pin.invert ? ", inverted" : "");
            if (pin.mode == PinMode::Output) {
                out.println();
            } else {
                out.printf(", debounce %u ms\n", pin.debounceMs);
            }
        }
    }

    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b) {
        const ServoBoardSettings& v = s.servoBoards[b];
        if (v.type == ServoBoardType::None) {
            continue;
        }
        out.printf("  servo board %u: PCA9685 at 0x%02X%s, servo pulses %s at rest\n", b + 1, v.address,
                   servoBoardAddressValid(v.address) ? "" : " - address not allowed",
                   v.hold ? "held" : "stopped");
        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            const ServoChannelSettings& c = v.channels[ch];
            if (useIsServo(c.use)) {
                out.printf("    channel %u servo: closed %u us, thrown %u us, travel %u ms, power-on %s\n",
                           ch + 1, c.closedUs, c.thrownUs, c.timeMs, toPowerOn[(uint8_t)usePowerOn(c.use)]);
            } else if (useIsLight(c.use)) {
                out.printf("    channel %u light%s: brightness %u, fade %u ms, power-on %s\n",
                           ch + 1, useInverted(c.use) ? ", inverted" : "", c.brightness, c.timeMs,
                           usePowerOn(c.use) == TurnoutPowerOn::Thrown ? "on" : "off");
            }
        }
    }
}

// --- the node's view of the space --------------------------------------------

static const uint32_t PASSWORD_START = CFG_WIFI + CFG_WIFI_PASSWORD;
static const uint32_t PASSWORD_END   = PASSWORD_START + CFG_WIFI_PASSWORD_SIZE;

size_t LccConfigView::read(uint32_t offset, uint8_t* data, size_t len) {
    const size_t n = backing_.read(offset, data, len);
    // Blank whatever part of the read falls inside the password.
    const uint32_t from = offset > PASSWORD_START ? offset : PASSWORD_START;
    const uint32_t to = (offset + n) < PASSWORD_END ? (offset + n) : PASSWORD_END;
    if (from < to) {
        memset(data + (from - offset), 0, to - from);
    }
    return n;
}

size_t LccConfigView::write(uint32_t offset, const uint8_t* data, size_t len) {
    const uint32_t end = offset + len;
    const bool coversStart = offset <= PASSWORD_START && end > PASSWORD_START;
    if (!coversStart || data[PASSWORD_START - offset] != 0) {
        return backing_.write(offset, data, len);
    }

    // The tool is writing back the blank it read. Keep the stored password and
    // write only what lies either side of it.
    size_t n = 0;
    if (offset < PASSWORD_START) {
        n += backing_.write(offset, data, PASSWORD_START - offset);
    }
    if (end > PASSWORD_END) {
        n += backing_.write(PASSWORD_END, data + (PASSWORD_END - offset), end - PASSWORD_END);
    }
    // Report the password bytes as written, so the tool sees success.
    const uint32_t skippedEnd = end < PASSWORD_END ? end : PASSWORD_END;
    return n + (skippedEnd - PASSWORD_START);
}
