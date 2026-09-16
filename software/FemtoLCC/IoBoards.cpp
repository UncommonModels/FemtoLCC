#include "IoBoards.h"
#include <Arduino.h>
#include "I2cBus.h"
#include "Notice.h"

// Inputs on one board are read this often. With every board full of inputs
// that is still one short transfer per loop at most.
static const uint32_t POLL_MS = 10;

// A missing board is probed, and a present one set up again, this often.
static const uint32_t CHECK_MS = 3000;

// MCP23017 registers, IOCON.BANK = 0 (the reset default). A and B sit side by
// side, so one two-byte transfer covers both ports.
static const uint8_t MCP_IODIRA = 0x00;
static const uint8_t MCP_IPOLA  = 0x02;
static const uint8_t MCP_GPPUA  = 0x0C;
static const uint8_t MCP_GPIOA  = 0x12;
static const uint8_t MCP_OLATA  = 0x14;

// TCA9534 / PCA9554 registers.
static const uint8_t TCA_INPUT    = 0x00;
static const uint8_t TCA_OUTPUT   = 0x01;
static const uint8_t TCA_POLARITY = 0x02;
static const uint8_t TCA_CONFIG   = 0x03;   // 1 is an input

IoBoards::IoBoards() : nextCheck_(0), nextPoll_(0) {
    for (Board& b : boards_) {
        b.type = IoBoardType::None;
        b.address = 0;
        b.problem = nullptr;
        b.fitted = false;
        b.present = false;
        b.reported = false;
        b.changed = false;
        b.dirty = false;
        b.lastCheck = 0;
        b.lastPoll = 0;
    }
}

void IoBoards::configure(const IoBoardSettings boards[NUM_XIO_BOARDS]) {
    bool slowChip = false;
    for (uint8_t i = 0; i < NUM_XIO_BOARDS; ++i) {
        const IoBoardSettings& s = boards[i];
        Board& b = boards_[i];

        const char* problem = nullptr;
        if (s.type != IoBoardType::None && !ioBoardAddressValid(s.type, s.address)) {
            problem = s.address == MCP23018_ADDR ? "0x20 is the on-board expander"
                                                 : "address not allowed for this chip";
        }
        for (uint8_t o = 0; o < i && !problem && s.type != IoBoardType::None; ++o) {
            if (boards_[o].fitted && boards_[o].address == s.address) {
                problem = "same address as another board";
            }
        }
        if (problem && problem != b.problem) {
            notice("I/O board %u at 0x%02X not used: %s\n", i + 1, s.address, problem);
        }

        // A different chip or address is a different board.
        if (s.type != b.type || s.address != b.address) {
            b.present = false;
            b.reported = false;
            b.changed = true;
        }
        b.type = s.type;
        b.address = s.address;
        b.problem = problem;
        b.fitted = s.type != IoBoardType::None && !problem;
        if (!b.fitted) {
            b.present = false;
        }
        b.bank.load(s.lines, b.fitted ? ioBoardLines(s.type) : 0);
        b.dirty = false;
        slowChip = slowChip || (b.fitted && s.type == IoBoardType::PCF8574);
    }

    // Everything else on the bus - the MCP23018, MCP23017, TCA9534, PCA9685 -
    // runs at 400 kHz. The PCF8574 is a 100 kHz part.
    i2cSetFast(!slowChip);

    // New settings: inputs take their levels afresh, as P0-P7 do.
    for (uint8_t i = 0; i < NUM_XIO_BOARDS; ++i) {
        if (boards_[i].fitted) {
            check(i, true);
        }
    }
}

// A present board is set up again, its inputs keeping their debounce state
// unless `seed`. A missing one is probed and, if it answers, set up afresh.
void IoBoards::check(uint8_t i, bool seed) {
    Board& b = boards_[i];
    b.lastCheck = millis();
    if (b.present) {
        if (!setup(b, seed)) {
            lost(i);
        }
    } else if (i2cProbe(b.address) && setup(b, true)) {
        found(i);
    } else {
        lost(i);
    }
}

void IoBoards::found(uint8_t i) {
    Board& b = boards_[i];
    b.present = true;
    b.reported = false;
    b.changed = true;
    notice("I/O board %u (%s at 0x%02X) answering\n", i + 1, ioBoardName(b.type), b.address);
}

void IoBoards::lost(uint8_t i) {
    Board& b = boards_[i];
    if (b.present) {
        b.changed = true;
    }
    b.present = false;
    b.lastCheck = millis();
    if (!b.reported) {
        b.reported = true;
        notice("I/O board %u (%s at 0x%02X) not answering\n", i + 1, ioBoardName(b.type), b.address);
    }
}

// Latch before direction, as for P0-P7, so a line that becomes an output
// starts in the right state. With `seed`, read the inputs as they are.
bool IoBoards::setup(Board& b, bool seed) {
    const uint16_t latch = b.bank.latch();
    const uint16_t inputs = b.bank.inputMask();
    const uint16_t pullups = b.bank.pullupMask();
    uint16_t levels = 0xFFFF;
    bool ok = false;

    switch (b.type) {
    case IoBoardType::MCP23017: {
        const uint8_t olat[2] = { (uint8_t)latch, (uint8_t)(latch >> 8) };
        const uint8_t gppu[2] = { (uint8_t)pullups, (uint8_t)(pullups >> 8) };
        const uint8_t ipol[2] = { 0, 0 };
        const uint8_t iodir[2] = { (uint8_t)inputs, (uint8_t)(inputs >> 8) };
        ok = i2cWrite(b.address, MCP_OLATA, olat, 2) &&
             i2cWrite(b.address, MCP_GPPUA, gppu, 2) &&
             i2cWrite(b.address, MCP_IPOLA, ipol, 2) &&
             i2cWrite(b.address, MCP_IODIRA, iodir, 2);
        break;
    }
    case IoBoardType::PCF8574: {
        // No registers: the latch is the port. Its 1 bits are what make the
        // inputs inputs.
        const uint8_t port = (uint8_t)latch;
        ok = i2cWriteRaw(b.address, &port, 1);
        break;
    }
    case IoBoardType::TCA9534: {
        const uint8_t out = (uint8_t)latch;
        const uint8_t pol = 0;
        const uint8_t cfg = (uint8_t)inputs;
        ok = i2cWrite(b.address, TCA_OUTPUT, &out, 1) &&
             i2cWrite(b.address, TCA_POLARITY, &pol, 1) &&
             i2cWrite(b.address, TCA_CONFIG, &cfg, 1);
        break;
    }
    default:
        return false;
    }

    if (ok) {
        b.dirty = false;
    }
    if (seed) {
        ok = ok && readLevels(b, levels);
        b.bank.start(levels, ok);
        b.lastPoll = millis();
    }
    return ok;
}

bool IoBoards::writeLatch(Board& b) {
    const uint16_t latch = b.bank.latch();
    switch (b.type) {
    case IoBoardType::MCP23017: {
        const uint8_t olat[2] = { (uint8_t)latch, (uint8_t)(latch >> 8) };
        return i2cWrite(b.address, MCP_OLATA, olat, 2);
    }
    case IoBoardType::PCF8574: {
        const uint8_t port = (uint8_t)latch;
        return i2cWriteRaw(b.address, &port, 1);
    }
    case IoBoardType::TCA9534: {
        const uint8_t out = (uint8_t)latch;
        return i2cWrite(b.address, TCA_OUTPUT, &out, 1);
    }
    default:
        return false;
    }
}

bool IoBoards::readLevels(Board& b, uint16_t& levels) {
    uint8_t in[2] = { 0xFF, 0xFF };
    bool ok;
    switch (b.type) {
    case IoBoardType::MCP23017: ok = i2cRead(b.address, MCP_GPIOA, in, 2); break;
    case IoBoardType::PCF8574:  ok = i2cReadRaw(b.address, in, 1);         break;
    case IoBoardType::TCA9534:  ok = i2cRead(b.address, TCA_INPUT, in, 1); break;
    default:                    ok = false;                                break;
    }
    levels = (uint16_t)(in[0] | (in[1] << 8));
    return ok;
}

void IoBoards::poll() {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < NUM_XIO_BOARDS; ++i) {
        Board& b = boards_[i];
        if (b.present && b.dirty) {
            if (writeLatch(b)) {
                b.dirty = false;
            } else {
                lost(i);
            }
        }
    }

    // At most one check per call.
    for (uint8_t n = 0; n < NUM_XIO_BOARDS; ++n) {
        const uint8_t i = (nextCheck_ + n) % NUM_XIO_BOARDS;
        if (boards_[i].fitted && now - boards_[i].lastCheck >= CHECK_MS) {
            check(i, false);
            nextCheck_ = (i + 1) % NUM_XIO_BOARDS;
            break;
        }
    }

    // At most one input read per call.
    for (uint8_t n = 0; n < NUM_XIO_BOARDS; ++n) {
        const uint8_t i = (nextPoll_ + n) % NUM_XIO_BOARDS;
        Board& b = boards_[i];
        if (!b.present || !b.bank.anyInput() || now - b.lastPoll < POLL_MS) {
            continue;
        }
        b.lastPoll = now;
        nextPoll_ = (i + 1) % NUM_XIO_BOARDS;
        uint16_t levels;
        if (readLevels(b, levels)) {
            b.bank.sample(levels, now);
        } else {
            lost(i);
        }
        break;
    }
}

bool IoBoards::takeChanged(uint8_t board) {
    if (board >= NUM_XIO_BOARDS || !boards_[board].changed) {
        return false;
    }
    boards_[board].changed = false;
    return true;
}

bool IoBoards::takeTransition(uint8_t board, uint8_t line, bool& active) {
    return board < NUM_XIO_BOARDS && boards_[board].bank.takeTransition(line, active);
}

bool IoBoards::fitted(uint8_t board) const {
    return board < NUM_XIO_BOARDS && boards_[board].fitted;
}

bool IoBoards::present(uint8_t board) const {
    return board < NUM_XIO_BOARDS && boards_[board].present;
}

uint8_t IoBoards::address(uint8_t board) const {
    return board < NUM_XIO_BOARDS ? boards_[board].address : 0;
}

const PinBank& IoBoards::bank(uint8_t board) const {
    return boards_[board < NUM_XIO_BOARDS ? board : 0].bank;
}

bool IoBoards::setOutput(uint8_t board, uint8_t line, bool on) {
    if (!fitted(board)) {
        return false;
    }
    Board& b = boards_[board];
    if (b.bank.mode(line) != PinMode::Output) {
        return false;
    }
    if (b.bank.set(line, on)) {
        b.dirty = true;
    }
    return true;
}

void IoBoards::printStatus(Print& out, bool lines) const {
    for (uint8_t i = 0; i < NUM_XIO_BOARDS; ++i) {
        const Board& b = boards_[i];
        if (b.type == IoBoardType::None) {
            continue;
        }
        out.printf(" X%u %-8s 0x%02X  %s\n", i + 1, ioBoardName(b.type), b.address,
                   b.problem ? b.problem : b.present ? "answering" : "NOT ANSWERING");
        if (!lines || !b.fitted) {
            continue;
        }
        for (uint8_t l = 0; l < b.bank.count(); ++l) {
            if (b.bank.mode(l) == PinMode::Output) {
                out.printf(" X%u.%-2u output %s\n", i + 1, l + 1, b.bank.output(l) ? "on" : "off");
            } else if (b.bank.isInput(l)) {
                out.printf(" X%u.%-2u input  %s\n", i + 1, l + 1,
                           !b.present ? "-" : b.bank.active(l) ? "active" : "inactive");
            }
        }
    }
}
