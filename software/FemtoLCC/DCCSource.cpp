#include "DCCSource.h"
#include "Channels.h"
#include <Arduino.h>
#include <string.h>

// NMRA S-9.1 bit timings, in microseconds per half-bit.
static const uint32_t DCC_ONE_HALF_US  = 58;
static const uint32_t DCC_ZERO_HALF_US = 100;

static const uint8_t PREAMBLE_BITS = 16;   // spec floor is 14
static const uint8_t PACKET_SLOTS = 4;

// One packet expanded to a bit stream the ISR can walk without doing any
// encoding work itself.
struct DccBits {
    uint8_t bits[(DCC_MAX_BITS + 7) / 8];
    uint8_t count;
    uint8_t repeats;
};

static volatile DccBits g_slots[PACKET_SLOTS];
static volatile uint8_t g_head = 0;      // written by the main loop
static volatile uint8_t g_tail = 0;      // consumed by the ISR

static volatile DccBits g_current;
static volatile uint8_t g_bitIndex = 0;
static volatile bool g_secondHalf = false;
static volatile bool g_level = false;
static volatile uint8_t g_repeatsLeft = 0;
static volatile bool g_haveCurrent = false;

static hw_timer_t* g_timer = nullptr;
static int g_dirPin = -1;

static inline bool bitAt(const volatile DccBits& p, uint8_t index) {
    return (p.bits[index >> 3] >> (7 - (index & 7))) & 1;
}

static inline void setBit(DccBits& p, uint8_t index, bool value) {
    if (value) {
        p.bits[index >> 3] |= (uint8_t)(0x80 >> (index & 7));
    } else {
        p.bits[index >> 3] &= (uint8_t)~(0x80 >> (index & 7));
    }
}

// The idle packet keeps the waveform alive when nothing else is queued. DCC is
// a continuously-clocked signal: stop sending and decoders lose the carrier.
static void buildIdle(DccBits& out) {
    memset(out.bits, 0, sizeof(out.bits));
    uint8_t n = 0;
    for (uint8_t i = 0; i < PREAMBLE_BITS; ++i) setBit(out, n++, true);
    const uint8_t bytes[3] = { 0xFF, 0x00, 0xFF };
    for (uint8_t b = 0; b < 3; ++b) {
        setBit(out, n++, false);                       // byte separator
        for (int8_t k = 7; k >= 0; --k) setBit(out, n++, (bytes[b] >> k) & 1);
    }
    setBit(out, n++, true);                            // end bit
    out.count = n;
    out.repeats = 1;
}

static void IRAM_ATTR onDccTimer() {
    // Every interrupt flips the bridge polarity; two flips make one DCC bit.
    g_level = !g_level;
    digitalWrite(g_dirPin, g_level ? HIGH : LOW);

    if (g_secondHalf) {
        g_secondHalf = false;
        if (++g_bitIndex >= g_current.count) {
            g_bitIndex = 0;
            if (g_repeatsLeft > 1) {
                --g_repeatsLeft;
            } else if (g_head != g_tail) {
                memcpy((void*)&g_current, (const void*)&g_slots[g_tail], sizeof(DccBits));
                g_tail = (uint8_t)((g_tail + 1) % PACKET_SLOTS);
                g_repeatsLeft = g_current.repeats;
            } else {
                DccBits idle;
                buildIdle(idle);
                memcpy((void*)&g_current, &idle, sizeof(DccBits));
                g_repeatsLeft = 1;
            }
        }
    } else {
        g_secondHalf = true;
    }

    const uint32_t half = bitAt(g_current, g_bitIndex) ? DCC_ONE_HALF_US
                                                       : DCC_ZERO_HALF_US;
    timerWrite(g_timer, 0);
    timerAlarm(g_timer, half, false, 0);
}

DCCSource::DCCSource(Channels& channels)
    : channels_(channels), running_(false), refreshCursor_(0),
      refreshPhase_(0), lastRefresh_(0) {
    forgetAll();
}

bool DCCSource::begin() {
    if (running_) {
        return true;
    }

    // Put channel A into DC mode with the PWM rail hard on, so the multiplexer
    // passes a constant high to whichever half `dir` selects.
    channels_.set(0, ChannelMode::DC, false, 255);
    channels_.flush();

    g_dirPin = PIN_DIR[0];
    pinMode(g_dirPin, OUTPUT);
    digitalWrite(g_dirPin, LOW);

    DccBits idle;
    buildIdle(idle);
    memcpy((void*)&g_current, &idle, sizeof(DccBits));
    g_bitIndex = 0;
    g_secondHalf = false;
    g_level = false;
    g_repeatsLeft = 1;
    g_head = g_tail = 0;

    g_timer = timerBegin(1000000);          // 1 MHz, so ticks are microseconds
    if (g_timer == nullptr) {
        return false;
    }
    timerAttachInterrupt(g_timer, &onDccTimer);
    timerAlarm(g_timer, DCC_ONE_HALF_US, false, 0);

    running_ = true;
    return true;
}

void DCCSource::end() {
    if (!running_) {
        return;
    }
    if (g_timer != nullptr) {
        timerDetachInterrupt(g_timer);
        timerEnd(g_timer);
        g_timer = nullptr;
    }
    running_ = false;
    channels_.set(0, ChannelMode::Off, false, 0);
    channels_.flush();
}

// --- packet construction ---------------------------------------------------

uint8_t DCCSource::writeAddress(uint16_t address, uint8_t* out) {
    if (address >= 128) {
        // 14-bit long address: 11AAAAAA AAAAAAAA
        out[0] = (uint8_t)(0xC0 | ((address >> 8) & 0x3F));
        out[1] = (uint8_t)(address & 0xFF);
        return 2;
    }
    out[0] = (uint8_t)(address & 0x7F);
    return 1;
}

void DCCSource::buildSpeedPacket(const Loco& loco, uint8_t* out, uint8_t& len) const {
    uint8_t n = writeAddress(loco.address, out);
    out[n++] = 0x3F;                     // 128-step speed instruction
    if (loco.speed < 0) {
        out[n++] = loco.forward ? 0x81 : 0x01;        // emergency stop
    } else if (loco.speed == 0) {
        out[n++] = loco.forward ? 0x80 : 0x00;        // stop
    } else {
        const uint8_t step = (uint8_t)(loco.speed + 1);   // 1..126 -> 2..127
        out[n++] = (uint8_t)((loco.forward ? 0x80 : 0x00) | (step & 0x7F));
    }
    len = n;
}

void DCCSource::buildFunctionPacket(const Loco& loco, uint8_t group,
                                    uint8_t* out, uint8_t& len) const {
    uint8_t n = writeAddress(loco.address, out);
    const uint32_t f = loco.functions;
    switch (group) {
    case 0:   // F0-F4: 100DDDDD, D0 is F0
        out[n++] = (uint8_t)(0x80
                 | ((f & (1UL << 0)) ? 0x10 : 0)
                 | ((f >> 1) & 0x0F));
        break;
    case 1:   // F5-F8: 1011DDDD
        out[n++] = (uint8_t)(0xB0 | ((f >> 5) & 0x0F));
        break;
    default:  // F9-F12: 1010DDDD
        out[n++] = (uint8_t)(0xA0 | ((f >> 9) & 0x0F));
        break;
    }
    len = n;
}

void DCCSource::queuePacket(const uint8_t* bytes, uint8_t count, uint8_t repeats) {
    if (!running_) {
        return;
    }
    const uint8_t next = (uint8_t)((g_head + 1) % PACKET_SLOTS);
    if (next == g_tail) {
        return;    // queue full; the refresh cycle will come round again
    }

    DccBits p;
    memset(p.bits, 0, sizeof(p.bits));
    uint8_t n = 0;
    for (uint8_t i = 0; i < PREAMBLE_BITS; ++i) setBit(p, n++, true);

    uint8_t checksum = 0;
    for (uint8_t b = 0; b < count; ++b) {
        checksum ^= bytes[b];
        setBit(p, n++, false);
        for (int8_t k = 7; k >= 0; --k) setBit(p, n++, (bytes[b] >> k) & 1);
    }
    setBit(p, n++, false);
    for (int8_t k = 7; k >= 0; --k) setBit(p, n++, (checksum >> k) & 1);
    setBit(p, n++, true);

    p.count = n;
    p.repeats = repeats;

    memcpy((void*)&g_slots[g_head], &p, sizeof(DccBits));
    g_head = next;
}

// --- loco table ------------------------------------------------------------

DCCSource::Loco* DCCSource::findLoco(uint16_t address, bool create) {
    for (uint8_t i = 0; i < DCC_MAX_LOCOS; ++i) {
        if (locos_[i].used && locos_[i].address == address) {
            return &locos_[i];
        }
    }
    if (!create) {
        return nullptr;
    }
    for (uint8_t i = 0; i < DCC_MAX_LOCOS; ++i) {
        if (!locos_[i].used) {
            locos_[i].used = true;
            locos_[i].address = address;
            locos_[i].speed = 0;
            locos_[i].forward = true;
            locos_[i].functions = 0;
            return &locos_[i];
        }
    }
    return nullptr;
}

void DCCSource::forgetAll() {
    for (uint8_t i = 0; i < DCC_MAX_LOCOS; ++i) {
        locos_[i].used = false;
        locos_[i].address = 0;
        locos_[i].speed = 0;
        locos_[i].forward = true;
        locos_[i].functions = 0;
    }
}

void DCCSource::setSpeed(uint16_t address, int16_t speed, bool forward) {
    Loco* loco = findLoco(address, true);
    if (loco == nullptr) {
        return;
    }
    if (speed > 126) speed = 126;
    loco->speed = speed;
    loco->forward = forward;

    uint8_t bytes[4];
    uint8_t len = 0;
    buildSpeedPacket(*loco, bytes, len);
    queuePacket(bytes, len, 2);    // a new command goes out twice
}

void DCCSource::setFunction(uint16_t address, uint8_t function, bool on) {
    Loco* loco = findLoco(address, true);
    if (loco == nullptr || function > 28) {
        return;
    }
    if (on) {
        loco->functions |= (1UL << function);
    } else {
        loco->functions &= ~(1UL << function);
    }

    const uint8_t group = function <= 4 ? 0 : (function <= 8 ? 1 : 2);
    uint8_t bytes[4];
    uint8_t len = 0;
    buildFunctionPacket(*loco, group, bytes, len);
    queuePacket(bytes, len, 2);
}

void DCCSource::emergencyStopAll() {
    for (uint8_t i = 0; i < DCC_MAX_LOCOS; ++i) {
        if (locos_[i].used) {
            locos_[i].speed = -1;
            uint8_t bytes[4];
            uint8_t len = 0;
            buildSpeedPacket(locos_[i], bytes, len);
            queuePacket(bytes, len, 3);
        }
    }
}

uint8_t DCCSource::locoCount() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < DCC_MAX_LOCOS; ++i) {
        if (locos_[i].used) ++n;
    }
    return n;
}

bool DCCSource::locoAt(uint8_t index, uint16_t& address, int16_t& speed,
                       bool& forward) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < DCC_MAX_LOCOS; ++i) {
        if (!locos_[i].used) continue;
        if (n++ == index) {
            address = locos_[i].address;
            speed = locos_[i].speed;
            forward = locos_[i].forward;
            return true;
        }
    }
    return false;
}

void DCCSource::update() {
    if (!running_) {
        return;
    }
    // Decoders forget: a loco that stops hearing its address will eventually
    // time out, so speed and functions are re-sent on a rotation.
    if (millis() - lastRefresh_ < 30) {
        return;
    }
    lastRefresh_ = millis();

    for (uint8_t tries = 0; tries < DCC_MAX_LOCOS; ++tries) {
        refreshCursor_ = (uint8_t)((refreshCursor_ + 1) % DCC_MAX_LOCOS);
        if (!locos_[refreshCursor_].used) {
            continue;
        }
        uint8_t bytes[4];
        uint8_t len = 0;
        if (refreshPhase_ == 0) {
            buildSpeedPacket(locos_[refreshCursor_], bytes, len);
        } else {
            buildFunctionPacket(locos_[refreshCursor_], (uint8_t)(refreshPhase_ - 1),
                                bytes, len);
        }
        queuePacket(bytes, len, 1);
        refreshPhase_ = (uint8_t)((refreshPhase_ + 1) % 4);
        return;
    }
}
