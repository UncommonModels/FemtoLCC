// Servo and light boards on the Qwiic connector J3: up to two PCA9685s, 16
// PWM channels each, running at the 50 Hz frame a servo expects.
//
// Each channel is unused, a servo turnout, or a light.
//
//   servo turnout  moves between its closed and thrown pulse widths over the
//                  travel time, a step every frame, so it moves slowly like a
//                  real turnout. A move reversed part-way takes the time for
//                  the distance left. Once in position its pulses stop, unless
//                  the board is set to hold, so an idle servo does not buzz.
//   light          fades between off and its brightness over the fade time.
//                  Brightness is perceptual: 128 looks about half as bright
//                  as 255. An inverted light is lit by a low output, for an
//                  LED wired from the supply into the pin.
//
// A servo gives no feedback, so its position is what was last commanded, and
// arrival is when the pulse width reaches the target. From an unknown position
// - power-on set to leave it, or a pulse from the console - the first move
// jumps straight there.
//
// Boards come and go like the I/O boards (IoBoards.h): a missing one is
// reported once and tried again every few seconds; when it answers it is set
// up and every channel sent where it should be.

#pragma once

#include <stdint.h>
#include "Config.h"
#include "Turnouts.h"   // TurnoutPosition

class Print;

class ServoBoards {
public:
    ServoBoards();

    // Take new settings, and probe every configured board now. A channel whose
    // use changed takes its power-on state; the others keep their position
    // and move to any new one.
    void configure(const ServoBoardSettings boards[NUM_SERVO_BOARDS]);

    // Retries, checks, motion and register writes. Call every loop.
    void poll();

    // True once each time a board is found or lost.
    bool takeChanged(uint8_t board);

    // True once when a servo reaches the position it was sent to, with
    // thrown saying which.
    bool takeArrival(uint8_t board, uint8_t channel, bool& thrown);

    bool fitted(uint8_t board) const;       // configured, at an address it may use
    bool present(uint8_t board) const;      // and answering
    uint8_t address(uint8_t board) const;

    // Throw or close a servo, or switch a light on or off. False if the
    // channel is neither.
    bool set(uint8_t board, uint8_t channel, bool thrown);

    // A raw pulse width in microseconds, 0 to stop the pulses, whatever the
    // channel is set up as: for finding a servo's positions.
    bool setPulse(uint8_t board, uint8_t channel, uint16_t us);

    // The last command, if there has been one.
    bool commanded(uint8_t board, uint8_t channel, bool& thrown) const;

    // Where a servo is. Unknown while moving, or while its board is missing.
    TurnoutPosition position(uint8_t board, uint8_t channel) const;

    // One line per configured board, and with `channels` each channel in use.
    void printStatus(Print& out, bool channels) const;

private:
    // Positions are in the channel's own units: microseconds for a servo, or
    // for a raw pulse; brightness times 16 for a light.
    struct Channel {
        ServoUse use;
        uint16_t closed, thrown;
        uint16_t timeMs;
        uint16_t value;         // where it is now
        uint16_t from, to;      // the move in progress
        uint32_t start, duration;
        uint32_t restAt;        // when it last stopped moving
        uint16_t settleMs;      // how long pulses carry on after that
        bool known;             // value is real
        bool target;            // thrown or on
        bool targetKnown;
        bool moving;
        bool announce;          // report this move's arrival
        bool arrived;
        bool pulsing;           // output running, not held full off
        bool raw;               // value is a pulse from the console
    };

    struct Board {
        uint8_t address;
        const char* problem;
        bool fitted;
        bool present;
        bool reported;
        bool changed;
        bool hold;
        uint32_t lastCheck;
        uint32_t lastFrame;
        Channel channel[SERVO_CHANNELS];
        uint16_t written[SERVO_CHANNELS];   // PWM last sent, or NOT_WRITTEN
    };

    Channel* find(uint8_t board, uint8_t channel);
    void moveTo(Board& b, Channel& c, uint16_t to, bool announce);
    void arrive(Board& b, Channel& c, uint16_t settleMs);
    void step(Board& b, uint32_t now);
    uint16_t pwm(const Channel& c) const;
    bool flush(Board& b);
    bool init(Board& b);
    void check(uint8_t board);
    void found(uint8_t board);
    void lost(uint8_t board);

    Board boards_[NUM_SERVO_BOARDS];
    uint8_t nextCheck_;
};
