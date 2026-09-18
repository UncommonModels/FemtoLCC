// Live control: memory space 0xE0.
//
// The configuration (space 253) says what each output is *for*. This space
// drives the outputs moment to moment from a dispatcher such as olcbweb: a
// block's DC speed and polarity, or DCC pass-through. Nothing here is saved; a
// reboot starts again from the configuration.
//
// The byte layout, which a dispatcher such as olcbweb is written against. It is
// this board's published interface: changing it breaks every dispatcher.
//
//   0   version, reads 1
//   1   lease in seconds, 0 for none. With a lease, when nothing writes to this
//       space for that long every output last set here is switched off, so a
//       dispatcher that crashes does not leave trains running.
//   4   outputs A-D, 4 bytes each: mode (0 off, 1 DC, 2 DCC), reverse, duty
//       (DC, 0-255), status (read only: bit 0 fault, 1 occupied, 2 set up as a
//       track block, 3 last set here). Only track blocks can be set. A write
//       applies each output whose first three bytes it covers.
//   20  DCC source on output A: write 1 to start it, 0 to stop; reads 1 if running
//   24  locomotive command, written as one 8-byte block: address (2), speed
//       (0-126, 0xFF emergency stop), direction (1 forward), functions F0-F28
//       as a bit mask (4). Reads back the last command.
//   32  the locomotives the source is refreshing, 8 x 4 bytes: address (2),
//       speed (0xFF emergency stop), flags (bit 0 forward); address 0 is empty
//
// The OpenLCB standards leave the spaces below 0xEF unassigned, so this is
// FemtoLCC's own, reached with the standard Memory Configuration read and
// write commands.
//
// DCC: this firmware has no DCC source yet, so bytes 20 onwards are present and
// correctly shaped but inert — byte 20 reads 0, the locomotive table reads as
// empty, and writes to either are accepted and ignored rather than rejected, so
// a dispatcher that drives blocks works unchanged and one that tries to drive
// locomotives fails visibly (nothing moves) rather than by datagram error. When
// a DCC source is added this is the one file that has to change.
//
// Threading: read() and write() are served on the stack's executor, while
// poll() is called from FemtoController's hardware thread. They therefore do
// not share a thread, and each takes FemtoController's recursive hardware lock
// before touching a driver. poll() is already called with that lock held.
// applyChannel() calls back into FemtoController::channel_changed(), which takes
// the same lock again — which is why it is recursive.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_LIVECONTROL_HXX_
#define _FEMTOLCC_LIVECONTROL_HXX_

#include <stdint.h>

#include "openlcb/MemoryConfig.hxx"

#include "Channels.h"
#include "DCCSource.h"
#include "Occupancy.h"
#include "board.h"

class FemtoController;

class LiveControl : public openlcb::MemorySpace
{
public:
    /// The memory space number a dispatcher addresses.
    static const uint8_t SPACE = 0xE0;

    LiveControl(Channels &channels, Occupancy &occupancy,
        FemtoController &controller, DCCSource &dcc);

    // --- openlcb::MemorySpace ---------------------------------------------

    address_t max_address() override
    {
        return SIZE - 1;
    }

    bool read_only() override
    {
        return false;
    }

    size_t read(address_t source, uint8_t *dst, size_t len, errorcode_t *error,
        Notifiable *again) override;

    size_t write(address_t destination, const uint8_t *data, size_t len,
        errorcode_t *error, Notifiable *again) override;

    // ----------------------------------------------------------------------

    /// Enforces the lease. Called from FemtoController's poll.
    void poll();

    /// Whether output `ch` was last set through this space.
    bool is_live(uint8_t ch) const
    {
        return ch < NUM_CHANNELS && set_[ch].live;
    }

private:
    static const uint32_t SIZE = 64;
    static const uint32_t CHANNELS_AT = 4;
    static const uint32_t SOURCE_AT = 20;
    static const uint32_t LOCO_AT = 24;
    static const uint32_t TABLE_AT = 32;

    /// What this space last set an output to, so that it can tell whether
    /// something else — an event, or the configuration being re-applied — has
    /// driven it since. The lease only switches off what is still as this space
    /// left it.
    struct Setting
    {
        bool live;
        ChannelMode mode;
        bool reverse;
        uint8_t duty;
    };

    /// True if a write of `len` bytes at `address` covers all `count` bytes
    /// starting at `from`.
    static bool covers(
        uint32_t address, size_t len, uint32_t from, uint32_t count);

    void apply_channel(uint8_t ch, const uint8_t *b);
    void expire();

    Channels &channels_;
    Occupancy &occupancy_;
    FemtoController &controller_;
    DCCSource &dcc_;

    /// Hands a locomotive command to the source, sending only what changed.
    void apply_loco(const uint8_t *b);

    uint8_t leaseS_;
    uint32_t lastWrite_;
    Setting set_[NUM_CHANNELS];
    uint8_t loco_[8];           // the last locomotive command, as written
    bool locoLive_;
};

#endif // _FEMTOLCC_LIVECONTROL_HXX_
