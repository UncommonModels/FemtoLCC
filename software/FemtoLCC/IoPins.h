// General-purpose I/O lines: the eight on expander port A, /P0../P7, and the
// lines on I/O expansion boards (IoBoards.h).
//
// Each line is unused, an input (with or without a pull-up), or an output.
// Inputs are debounced here and report transitions so the caller can produce
// LCC events; outputs are switched by consumed events.
//
// Levels are expressed as active/on rather than high/low. The expander's
// outputs are open drain, so the natural wiring is to ground: an input is
// active when something pulls it low, and an output is on when it pulls low.
// Expansion lines follow the same rule, so a line means the same wherever it
// is. The per-line invert setting swaps both.
//
// PinBank is that logic for up to 16 lines, with no I/O of its own: its owner
// reads the chip and hands the levels over, and sets the chip up the way it
// asks. IoPins is the owner for port A; IoBoards owns one bank per board.

#pragma once

#include <stdint.h>
#include "board.h"
#include "Expander.h"
#include "Config.h"

class PinBank {
public:
    static const uint8_t MAX_LINES = 16;

    PinBank();

    // Take settings for the first `count` lines; any beyond are unused.
    // Outputs that stay outputs keep their state; new ones start off.
    void load(const PinSettings* pins, uint8_t count);

    // How the chip should be set up. Bit n is line n. Direction: 1 is an
    // input, and unused lines are inputs. Pull-ups: on for unused lines too,
    // so they do not float. Latch: 1 is high, and every line that is not an
    // output reads 1, which is what a quasi-bidirectional port needs.
    uint16_t inputMask() const;
    uint16_t pullupMask() const;
    uint16_t latch() const;

    // Levels read from the chip, a 1 bit for high. start() takes them as they
    // are without reporting a transition; valid false leaves every input
    // inactive. sample() debounces.
    void start(uint16_t levels, bool valid);
    void sample(uint16_t levels, uint32_t now);

    // True exactly once per debounced input change.
    bool takeTransition(uint8_t line, bool& active);

    uint8_t count() const { return count_; }
    PinMode mode(uint8_t line) const;
    bool isInput(uint8_t line) const;
    bool anyInput() const { return inputs_ != 0; }
    bool active(uint8_t line) const;        // debounced input state
    bool output(uint8_t line) const;        // output state

    // Switch an output. True if the latch changed and must be written.
    bool set(uint8_t line, bool on);

private:
    uint8_t count_;
    PinMode mode_[MAX_LINES];
    uint16_t debounceMs_[MAX_LINES];
    uint32_t since_[MAX_LINES];

    // One bit per line.
    uint16_t inputs_;
    uint16_t outputs_;
    uint16_t invert_;
    uint16_t on_;
    uint16_t active_;
    uint16_t pending_;
    uint16_t transition_;
};

class IoPins : public PinBank {
public:
    explicit IoPins(Expander& expander);

    // Set every pin's direction, pull-up and polarity. Inputs take their
    // current level without reporting a transition.
    void configure(const PinSettings pins[EXP_GPIO_COUNT]);

    // Read the port every few milliseconds and debounce. Call every loop.
    void poll();

    void setOutput(uint8_t pin, bool on);

private:
    Expander& expander_;
    uint32_t lastPoll_;
};
