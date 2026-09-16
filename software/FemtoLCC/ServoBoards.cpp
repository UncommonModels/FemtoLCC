#include "ServoBoards.h"
#include <Arduino.h>
#include "I2cBus.h"
#include "Notice.h"

// One servo frame. Moves step this often.
static const uint32_t FRAME_MS = 20;
static const uint32_t FRAME_US = 20000;

// A missing board is probed, and a present one checked, this often.
static const uint32_t CHECK_MS = 3000;

// Pulses carry on this long after a servo arrives, for the servo itself to
// catch up with the last step, before they stop. After a jump - from an
// unknown position, or on a board just found - the servo swings at its own
// speed, which can take most of a second.
static const uint16_t SETTLE_MS = 400;
static const uint16_t JUMP_SETTLE_MS = 1000;

// Pulse widths outside this could drive a servo against its end stops.
static const uint16_t MIN_US = 500;
static const uint16_t MAX_US = 2500;

// PWM values: 0 is full off, FULL full on, anything between the count at
// which the output goes low in a 4096-count frame.
static const uint16_t FULL = 4096;
static const uint16_t NOT_WRITTEN = 0xFFFF;

// PCA9685 registers.
static const uint8_t REG_MODE1    = 0x00;
static const uint8_t REG_MODE2    = 0x01;
static const uint8_t REG_LED0     = 0x06;   // ON_L, ON_H, OFF_L, OFF_H, then LED1...
static const uint8_t REG_PRESCALE = 0xFE;

static const uint8_t MODE1_RESTART = 0x80;
static const uint8_t MODE1_AI      = 0x20;  // register address steps on by itself
static const uint8_t MODE1_SLEEP   = 0x10;
static const uint8_t MODE2_OUTDRV  = 0x04;  // totem-pole outputs
static const uint16_t LED_FULL     = 0x1000;

// 25 MHz internal oscillator / (4096 counts * 50 Hz) - 1.
static const uint8_t PRESCALE_50HZ = 121;

static uint16_t clampUs(uint16_t us) {
    return us < MIN_US ? MIN_US : us > MAX_US ? MAX_US : us;
}

// The four LEDn registers: on at count 0, off at `pwm`; or the full-on or
// full-off bit.
static void encode(uint16_t pwm, uint8_t* reg) {
    const uint16_t on = pwm >= FULL ? LED_FULL : 0;
    const uint16_t off = pwm == 0 ? LED_FULL : pwm >= FULL ? 0 : pwm;
    reg[0] = (uint8_t)on;
    reg[1] = (uint8_t)(on >> 8);
    reg[2] = (uint8_t)off;
    reg[3] = (uint8_t)(off >> 8);
}

ServoBoards::ServoBoards() : nextCheck_(0) {
    for (Board& b : boards_) {
        b.address = 0;
        b.problem = nullptr;
        b.fitted = false;
        b.present = false;
        b.reported = false;
        b.changed = false;
        b.hold = false;
        b.lastCheck = 0;
        b.lastFrame = 0;
        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            b.channel[ch] = Channel{};      // unused, nothing known
            b.written[ch] = NOT_WRITTEN;
        }
    }
}

// --- settings ------------------------------------------------------------------

void ServoBoards::configure(const ServoBoardSettings boards[NUM_SERVO_BOARDS]) {
    for (uint8_t i = 0; i < NUM_SERVO_BOARDS; ++i) {
        const ServoBoardSettings& s = boards[i];
        Board& b = boards_[i];
        const bool wanted = s.type == ServoBoardType::PCA9685;

        const char* problem = nullptr;
        if (wanted && !servoBoardAddressValid(s.address)) {
            problem = "address not allowed";
        }
        for (uint8_t o = 0; o < i && wanted && !problem; ++o) {
            if (boards_[o].fitted && boards_[o].address == s.address) {
                problem = "same address as another board";
            }
        }
        if (problem && problem != b.problem) {
            notice("servo board %u at 0x%02X not used: %s\n", i + 1, s.address, problem);
        }

        const bool fitted = wanted && !problem;
        if (s.address != b.address || fitted != b.fitted) {
            b.present = false;
            b.reported = false;
            b.changed = true;
        }
        b.address = s.address;
        b.problem = problem;
        b.fitted = fitted;
        b.hold = s.hold;

        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            const ServoChannelSettings& cs = s.channels[ch];
            Channel& c = b.channel[ch];
            const ServoUse use = fitted ? cs.use : ServoUse::Unused;
            const bool kindChanged = useIsServo(use) != useIsServo(c.use) ||
                                     useIsLight(use) != useIsLight(c.use);
            c.use = use;
            c.timeMs = cs.timeMs;
            if (useIsLight(use)) {
                c.closed = 0;
                c.thrown = (uint16_t)(cs.brightness * 16);
            } else {
                c.closed = clampUs(cs.closedUs);
                c.thrown = clampUs(cs.thrownUs);
            }

            if (kindChanged) {
                // A new use starts from its power-on state.
                c.raw = c.moving = c.arrived = c.known = c.targetKnown = c.pulsing = false;
                if (use != ServoUse::Unused && usePowerOn(use) != TurnoutPowerOn::Leave) {
                    c.target = usePowerOn(use) == TurnoutPowerOn::Thrown;
                    c.targetKnown = true;
                    moveTo(b, c, c.target ? c.thrown : c.closed, false);
                }
            } else if (c.targetKnown && !c.raw) {
                // The same use, perhaps with new positions or brightness.
                const uint16_t to = c.target ? c.thrown : c.closed;
                if (to != (c.moving ? c.to : c.value)) {
                    moveTo(b, c, to, c.moving && c.announce);
                }
            }
            if (useIsServo(use) && b.hold && c.known) {
                c.pulsing = true;
            }
        }
    }

    for (uint8_t i = 0; i < NUM_SERVO_BOARDS; ++i) {
        if (boards_[i].fitted) {
            check(i);
        }
    }
}

// --- motion --------------------------------------------------------------------

ServoBoards::Channel* ServoBoards::find(uint8_t board, uint8_t channel) {
    if (board >= NUM_SERVO_BOARDS || channel >= SERVO_CHANNELS || !boards_[board].fitted) {
        return nullptr;
    }
    return &boards_[board].channel[channel];
}

// Start a move from where the channel is. It takes the travel time for the
// full distance between the two positions, and proportionally less for less.
// With nowhere known to start from, or no travel time, it jumps.
void ServoBoards::moveTo(Board& b, Channel& c, uint16_t to, bool announce) {
    c.pulsing = true;
    c.announce = announce && useIsServo(c.use);
    c.arrived = false;
    const uint16_t span = c.thrown > c.closed ? c.thrown - c.closed : c.closed - c.thrown;
    const uint16_t distance = to > c.value ? to - c.value : c.value - to;
    if (!c.known || c.timeMs == 0 || span == 0 || distance == 0) {
        const bool jump = !c.known || (c.timeMs == 0 && distance != 0);
        c.value = to;
        c.known = true;
        arrive(b, c, jump ? JUMP_SETTLE_MS : SETTLE_MS);
        return;
    }
    c.from = c.value;
    c.to = to;
    c.start = millis();
    c.duration = (uint32_t)c.timeMs * (distance < span ? distance : span) / span;
    c.moving = true;
}

// Arrival counts only if the board was there to make the move.
void ServoBoards::arrive(Board& b, Channel& c, uint16_t settleMs) {
    c.moving = false;
    c.restAt = millis();
    c.settleMs = settleMs;
    c.arrived = c.announce && b.present;
}

void ServoBoards::step(Board& b, uint32_t now) {
    for (Channel& c : b.channel) {
        if (c.moving) {
            const uint32_t elapsed = now - c.start;
            if (elapsed >= c.duration) {
                c.value = c.to;
                arrive(b, c, SETTLE_MS);
            } else {
                c.value = (uint16_t)(c.from + ((int32_t)c.to - (int32_t)c.from) *
                                              (int32_t)elapsed / (int32_t)c.duration);
            }
        } else if (c.pulsing && !b.hold && !c.raw && useIsServo(c.use) &&
                   now - c.restAt >= c.settleMs) {
            // In position: stop the pulses, so the servo does not buzz.
            c.pulsing = false;
        }
    }
}

uint16_t ServoBoards::pwm(const Channel& c) const {
    if (!c.pulsing) {
        return 0;
    }
    if (c.raw || useIsServo(c.use)) {
        return (uint16_t)(((uint32_t)c.value * FULL + FRAME_US / 2) / FRAME_US);
    }
    // A light: the square of the level, so equal steps look like equal steps.
    // value is brightness * 16, at most 4080, and 4080^2 / 4064 is just FULL.
    const uint32_t level = c.value;
    uint32_t n = level * level / 4064;
    if (level && !n) {
        n = 1;
    }
    if (n > FULL) {
        n = FULL;
    }
    return useInverted(c.use) ? (uint16_t)(FULL - n) : (uint16_t)n;
}

// Send every channel whose PWM has changed, in one transfer spanning them.
bool ServoBoards::flush(Board& b) {
    uint16_t want[SERVO_CHANNELS];
    int lo = -1;
    int hi = -1;
    for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
        want[ch] = pwm(b.channel[ch]);
        if (want[ch] != b.written[ch]) {
            if (lo < 0) {
                lo = ch;
            }
            hi = ch;
        }
    }
    if (lo < 0) {
        return true;
    }
    uint8_t regs[4 * SERVO_CHANNELS];
    for (int ch = lo; ch <= hi; ++ch) {
        encode(want[ch], regs + 4 * (ch - lo));
    }
    if (!i2cWrite(b.address, REG_LED0 + 4 * lo, regs, 4 * (hi - lo + 1))) {
        return false;
    }
    for (int ch = lo; ch <= hi; ++ch) {
        b.written[ch] = want[ch];
    }
    return true;
}

// --- boards --------------------------------------------------------------------

// The prescaler can only be set while asleep. All Call is turned off, or every
// PCA9685 would also answer at 0x70. Then every channel is sent afresh. The
// servos on a board just found or just reset may be anywhere, so each one
// with a position is pulsed long enough for a full swing.
bool ServoBoards::init(Board& b) {
    const uint8_t sleep = MODE1_SLEEP | MODE1_AI;
    const uint8_t prescale = PRESCALE_50HZ;
    const uint8_t mode2 = MODE2_OUTDRV;
    const uint8_t wake = MODE1_AI;
    const uint8_t restart = MODE1_RESTART | MODE1_AI;
    if (!i2cWrite(b.address, REG_MODE1, &sleep, 1) ||
        !i2cWrite(b.address, REG_PRESCALE, &prescale, 1) ||
        !i2cWrite(b.address, REG_MODE2, &mode2, 1) ||
        !i2cWrite(b.address, REG_MODE1, &wake, 1)) {
        return false;
    }
    delayMicroseconds(500);     // the oscillator's start-up time, once per board found
    if (!i2cWrite(b.address, REG_MODE1, &restart, 1)) {
        return false;
    }
    const uint32_t now = millis();
    for (Channel& c : b.channel) {
        if (useIsServo(c.use) && c.known && !c.moving && !c.raw) {
            c.pulsing = true;
            c.restAt = now;
            c.settleMs = JUMP_SETTLE_MS;
        }
    }
    for (uint16_t& w : b.written) {
        w = NOT_WRITTEN;
    }
    return flush(b);
}

// A present board comes back from a power cut asleep, with auto-increment
// off: set it up again if so. A missing one is probed.
void ServoBoards::check(uint8_t i) {
    Board& b = boards_[i];
    b.lastCheck = millis();
    if (b.present) {
        uint8_t mode1;
        if (!i2cRead(b.address, REG_MODE1, &mode1, 1)) {
            lost(i);
        } else if ((mode1 & (MODE1_SLEEP | MODE1_AI)) != MODE1_AI && !init(b)) {
            lost(i);
        }
    } else if (i2cProbe(b.address) && init(b)) {
        found(i);
    } else {
        lost(i);
    }
}

void ServoBoards::found(uint8_t i) {
    Board& b = boards_[i];
    b.present = true;
    b.reported = false;
    b.changed = true;
    notice("servo board %u (PCA9685 at 0x%02X) answering\n", i + 1, b.address);
}

void ServoBoards::lost(uint8_t i) {
    Board& b = boards_[i];
    if (b.present) {
        b.changed = true;
    }
    b.present = false;
    b.lastCheck = millis();
    if (!b.reported) {
        b.reported = true;
        notice("servo board %u (PCA9685 at 0x%02X) not answering\n", i + 1, b.address);
    }
}

void ServoBoards::poll() {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_SERVO_BOARDS; ++i) {
        Board& b = boards_[i];
        if (!b.fitted) {
            continue;
        }
        // Time moves on while a board is missing, so it picks up where it
        // should be when it comes back.
        if (now - b.lastFrame >= FRAME_MS) {
            b.lastFrame = now;
            step(b, now);
        }
        if (b.present && !flush(b)) {
            lost(i);
        }
    }

    for (uint8_t n = 0; n < NUM_SERVO_BOARDS; ++n) {
        const uint8_t i = (nextCheck_ + n) % NUM_SERVO_BOARDS;
        if (boards_[i].fitted && now - boards_[i].lastCheck >= CHECK_MS) {
            check(i);
            nextCheck_ = (i + 1) % NUM_SERVO_BOARDS;
            break;
        }
    }
}

// --- commands and state ----------------------------------------------------------

bool ServoBoards::set(uint8_t board, uint8_t channel, bool thrown) {
    Channel* c = find(board, channel);
    if (!c || c->use == ServoUse::Unused) {
        return false;
    }
    if (c->raw) {
        // A light's level means nothing after a pulse width; start it afresh.
        c->raw = false;
        c->known = c->known && useIsServo(c->use);
    }
    c->target = thrown;
    c->targetKnown = true;
    moveTo(boards_[board], *c, thrown ? c->thrown : c->closed, true);
    return true;
}

bool ServoBoards::setPulse(uint8_t board, uint8_t channel, uint16_t us) {
    Channel* c = find(board, channel);
    if (!c) {
        return false;
    }
    c->moving = false;
    c->arrived = false;
    c->targetKnown = false;
    if (us == 0) {
        c->pulsing = false;
        c->known = c->known && useIsServo(c->use) && !c->raw;
        c->raw = false;
    } else {
        // Held until the next command, whatever the board's setting, so the
        // servo can be watched while the width is tried.
        c->raw = true;
        c->value = clampUs(us);
        c->known = true;
        c->pulsing = true;
    }
    return true;
}

bool ServoBoards::takeChanged(uint8_t board) {
    if (board >= NUM_SERVO_BOARDS || !boards_[board].changed) {
        return false;
    }
    boards_[board].changed = false;
    return true;
}

bool ServoBoards::takeArrival(uint8_t board, uint8_t channel, bool& thrown) {
    Channel* c = find(board, channel);
    if (!c || !c->arrived) {
        return false;
    }
    c->arrived = false;
    thrown = c->target;
    return true;
}

bool ServoBoards::fitted(uint8_t board) const {
    return board < NUM_SERVO_BOARDS && boards_[board].fitted;
}

bool ServoBoards::present(uint8_t board) const {
    return board < NUM_SERVO_BOARDS && boards_[board].present;
}

uint8_t ServoBoards::address(uint8_t board) const {
    return board < NUM_SERVO_BOARDS ? boards_[board].address : 0;
}

bool ServoBoards::commanded(uint8_t board, uint8_t channel, bool& thrown) const {
    if (!fitted(board) || channel >= SERVO_CHANNELS) {
        return false;
    }
    const Channel& c = boards_[board].channel[channel];
    thrown = c.target;
    return c.targetKnown;
}

TurnoutPosition ServoBoards::position(uint8_t board, uint8_t channel) const {
    if (!present(board) || channel >= SERVO_CHANNELS) {
        return TurnoutPosition::Unknown;
    }
    const Channel& c = boards_[board].channel[channel];
    if (!c.targetKnown || c.moving || c.raw) {
        return TurnoutPosition::Unknown;
    }
    return c.target ? TurnoutPosition::Thrown : TurnoutPosition::Closed;
}

void ServoBoards::printStatus(Print& out, bool channels) const {
    for (uint8_t i = 0; i < NUM_SERVO_BOARDS; ++i) {
        const Board& b = boards_[i];
        if (!b.fitted && !b.problem) {
            continue;
        }
        out.printf(" S%u PCA9685  0x%02X  %s\n", i + 1, b.address,
                   b.problem ? b.problem : b.present ? "answering" : "NOT ANSWERING");
        if (!channels || !b.fitted) {
            continue;
        }
        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            const Channel& c = b.channel[ch];
            if (c.raw) {
                out.printf(" S%u.%-2u pulse  %u us\n", i + 1, ch + 1, c.value);
            } else if (useIsServo(c.use)) {
                const char* where = !c.targetKnown ? "-"
                                  : c.moving       ? (c.target ? "throwing" : "closing")
                                  : c.target       ? "thrown" : "closed";
                out.printf(" S%u.%-2u servo  %-8s %4u us%s\n", i + 1, ch + 1, where,
                           c.known ? c.value : 0, c.pulsing ? "" : ", pulses off");
            } else if (useIsLight(c.use)) {
                out.printf(" S%u.%-2u light  %-8s %3u\n", i + 1, ch + 1,
                           c.moving ? "fading" : c.target ? "on" : "off", c.value / 16);
            }
        }
    }
}
