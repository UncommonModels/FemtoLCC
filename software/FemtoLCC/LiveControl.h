// Live control: memory space 0xE0.
//
// The configuration (space 0xFD) says what each output is for. This space
// drives the outputs moment to moment from a dispatcher such as olcbweb: a
// block's DC speed and polarity, DCC pass-through, and locomotives on the
// board's own DCC source. Nothing here is saved; a reboot starts again from
// the configuration.
//
// The OpenLCB standards leave the spaces below 0xEF unassigned, so this is
// FemtoLCC's own, reached with the standard Memory Configuration read and
// write commands. The layout, big-endian:
//
//   0   version, reads 1
//   1   lease in seconds, 0 for none. With a lease, when nothing writes to this
//       space for that long every output last set here is switched off and
//       every locomotive commanded here is stopped, so a dispatcher that
//       crashes does not leave trains running.
//   4   outputs A-D, 4 bytes each: mode (0 off, 1 DC, 2 DCC), reverse, duty
//       (DC, 0-255), status (read only: bit 0 fault, 1 occupied, 2 set up as a
//       track block, 3 last set here). Only track blocks can be set, and not
//       output A while it is the DCC source. A write applies each output whose
//       first three bytes it covers.
//   20  DCC source on output A: write 1 to start it, 0 to stop; reads 1 if running
//   24  locomotive command, written as one 8-byte block: address (2), speed
//       (0-126, 0xFF emergency stop), direction (1 forward), functions F0-F28
//       as a bit mask (4). Reads back the last command.
//   32  the locomotives the source is refreshing, 8 x 4 bytes: address (2),
//       speed (0xFF emergency stop), flags (bit 0 forward); address 0 is empty

#pragma once

#include <stdint.h>
#include <AOLCB.h>
#include "board.h"
#include "Channels.h"
#include "Occupancy.h"
#include "Controller.h"
#include "DCCSource.h"

class LiveControl : public AOLCB::MemorySpace {
public:
    static const uint8_t SPACE = 0xE0;

    LiveControl(Channels& channels, Occupancy& occupancy, Controller& controller, DCCSource& dcc);

    uint32_t size() const override { return SIZE; }
    size_t read(uint32_t address, uint8_t* out, size_t len) override;
    uint16_t write(uint32_t address, const uint8_t* data, size_t len) override;

    // Enforces the lease. Call every loop.
    void poll();

    // Whether output ch was last set through this space.
    bool isLive(uint8_t ch) const { return ch < NUM_CHANNELS && set_[ch].live; }

private:
    static const uint32_t SIZE = 64;
    static const uint32_t CHANNELS_AT = 4;
    static const uint32_t SOURCE_AT = 20;
    static const uint32_t LOCO_AT = 24;
    static const uint32_t TABLE_AT = 32;

    // What this space last set an output to, to tell whether something else
    // (an event, the console) has driven it since.
    struct Setting {
        bool live;
        ChannelMode mode;
        bool reverse;
        uint8_t duty;
    };

    static bool covers(uint32_t address, size_t len, uint32_t from, uint32_t count);
    void applyChannel(uint8_t ch, const uint8_t* b);
    void applyLoco(const uint8_t* b);
    void expire();

    Channels& channels_;
    Occupancy& occupancy_;
    Controller& controller_;
    DCCSource& dcc_;

    uint8_t leaseS_;
    uint32_t lastWrite_;
    Setting set_[NUM_CHANNELS];
    uint8_t loco_[8];           // the last locomotive command, as written
    bool locoLive_;
};
