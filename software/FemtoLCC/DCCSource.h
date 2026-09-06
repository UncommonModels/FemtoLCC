// DCC waveform generation on channel A.
//
// Channel A is the only channel that can do this, and the board's wiring is why:
// its direction line is on a real GPIO (`/dir_A`, GPIO4), while B, C and D have
// theirs behind the MCP23018. An I2C write per half-bit is nowhere near the 58 us
// DCC needs, so A is the only candidate.
//
// The trick is the existing multiplexer truth table. With dcc_en low and the PWM
// line held high:
//
//   dir = 0  ->  IN1 = 1, IN2 = 0  ->  OUT1 +, OUT2 -
//   dir = 1  ->  IN1 = 0, IN2 = 1  ->  OUT1 -, OUT2 +
//
// Toggling dir at DCC bit timings therefore swings the H-bridge between the two
// polarities, which is exactly a DCC signal. No extra hardware, just a timer.
//
// A '1' bit is 58 us per half; a '0' bit is 100 us per half.

#pragma once

#include <stdint.h>
#include "board.h"

class Channels;

// Longest packet we emit: 16 preamble + start + 6 bytes at 9 bits + end.
static const uint8_t DCC_MAX_BITS = 96;

// Locomotives whose speed and functions are refreshed on the bus.
static const uint8_t DCC_MAX_LOCOS = 8;

class DCCSource {
public:
    explicit DCCSource(Channels& channels);

    // Takes channel A over: mux to DC mode, PWM hard on, timer driving dir_A.
    // Returns false if the timer could not be claimed.
    bool begin();

    // Stops the waveform and returns channel A to normal control.
    void end();

    bool isRunning() const { return running_; }

    // speed is 0-126, or -1 for emergency stop. Address may be short (1-127) or
    // long (128-10239); the encoding is chosen automatically.
    void setSpeed(uint16_t address, int16_t speed, bool forward);
    void setFunction(uint16_t address, uint8_t function, bool on);
    void emergencyStopAll();
    void forgetAll();

    uint8_t locoCount() const;
    bool locoAt(uint8_t index, uint16_t& address, int16_t& speed, bool& forward) const;

    // Called from the main loop: keeps the refresh cycle turning.
    void update();

private:
    struct Loco {
        uint16_t address;
        int16_t speed;      // -1 estop, 0 stop, 1..126
        bool forward;
        uint32_t functions; // bit n = Fn
        bool used;
    };

    Loco* findLoco(uint16_t address, bool create);

    // Packet assembly. Bytes are the DCC payload without the checksum, which is
    // appended here.
    void queuePacket(const uint8_t* bytes, uint8_t count, uint8_t repeats);
    void buildSpeedPacket(const Loco& loco, uint8_t* out, uint8_t& len) const;
    void buildFunctionPacket(const Loco& loco, uint8_t group, uint8_t* out, uint8_t& len) const;
    static uint8_t writeAddress(uint16_t address, uint8_t* out);

    Channels& channels_;
    bool running_;

    Loco locos_[DCC_MAX_LOCOS];
    uint8_t refreshCursor_;
    uint8_t refreshPhase_;      // rotates speed / function groups
    uint32_t lastRefresh_;
};
