// I/O expansion boards on the Qwiic connector J3.
//
// Up to four boards, each an MCP23017 (16 lines), a PCF8574 or PCF8574A, or a
// TCA9534 or PCA9554 (8 lines each). Every line works like P0-P7 - unused,
// input, input with pull-up, or output, with the same polarity and debounce
// settings - and the logic is PinBank's, shared with IoPins.
//
//   MCP23017   push-pull outputs, 100k pull-ups
//   PCF8574    quasi-bidirectional: an output sinks, and anything else is held
//              high by a weak current source, so every input has a pull-up
//   TCA9534    push-pull outputs, no pull-ups: input with pull-up acts as input
//
// Boards are optional and come and go: plugged in after power-on, or knocked
// loose. One that does not answer is reported once and tried again every
// few seconds, and when it answers it is set up and its outputs take the
// state they were last commanded. A board that is answering is set up again
// just as often, so one unplugged and plugged back between two checks does
// not stay reset.
//
// Nothing here waits. Inputs are read one board per call, round-robin, each
// board no more often than every POLL_MS, and only boards with inputs.

#pragma once

#include <stdint.h>
#include "Config.h"
#include "IoPins.h"

class Print;

class IoBoards {
public:
    IoBoards();

    // Take new settings, and probe every configured board now.
    void configure(const IoBoardSettings boards[NUM_XIO_BOARDS]);

    // Retries, checks, input reads and pending output writes. Call every loop.
    void poll();

    // True once each time a board is found or lost, so the caller can update
    // the states of its events.
    bool takeChanged(uint8_t board);

    // True exactly once per debounced input change.
    bool takeTransition(uint8_t board, uint8_t line, bool& active);

    bool fitted(uint8_t board) const;       // configured, at an address it may use
    bool present(uint8_t board) const;      // and answering
    uint8_t address(uint8_t board) const;
    const PinBank& bank(uint8_t board) const;

    // Switch an output. It is written on the next poll(), so lines switched
    // together go out in one write. False if the line is not an output.
    bool setOutput(uint8_t board, uint8_t line, bool on);

    // One line per configured board, and with `lines` each line in use.
    void printStatus(Print& out, bool lines) const;

private:
    struct Board {
        IoBoardType type;
        uint8_t address;
        const char* problem;    // why a configured board is not used
        bool fitted;
        bool present;
        bool reported;          // "not answering" printed since it was last seen
        bool changed;
        bool dirty;             // latch waiting to be written
        uint32_t lastCheck;
        uint32_t lastPoll;
        PinBank bank;
    };

    void check(uint8_t board, bool seed);
    bool setup(Board& b, bool seed);
    bool writeLatch(Board& b);
    bool readLevels(Board& b, uint16_t& levels);
    void found(uint8_t board);
    void lost(uint8_t board);

    Board boards_[NUM_XIO_BOARDS];
    uint8_t nextCheck_;
    uint8_t nextPoll_;
};
