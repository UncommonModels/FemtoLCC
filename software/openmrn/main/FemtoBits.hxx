// The board's hardware, expressed as OpenLCB bits.
//
// Most of what this node produces and consumes is one bit with one pair of
// events: an I/O pin is on or off, a block is occupied or clear, a servo is
// thrown or closed. OpenMRN already models exactly that — BitEventInterface is
// "a thing with an on event, an off event, a current state and a setter", and
// BitEventProducer, BitEventConsumer and BitEventPC turn one into a conforming
// event handler, including the identify replies that are easy to get subtly
// wrong by hand.
//
// So the test applied here is simply: is it one bit? Where the answer is yes,
// the hardware goes through OpenMRN's classes rather than through
// FemtoController's own event handler:
//
//   an I/O pin, in or out          an expansion board line, in or out
//   a block's occupancy            a servo channel, or a light
//   a turnout, as *two* bits       (see below)
//
// A turnout is two bits rather than one because the events it is driven by and
// the events it reports are different IDs: "throw"/"close" are consumed and
// "thrown"/"closed" are produced. One BitEventInterface carries one pair, so it
// takes two — a consumed pair and a produced pair — over the same position.
//
// What is deliberately *not* here, and stays in FemtoController by hand:
//
//   - A block's power. It is three mutually exclusive states — off, DC, DCC —
//     over three separate consumed events. A bit is two states, and three
//     BitEventConsumers over one output would each answer identify without
//     knowing about the other two, so a tool would see two of them claim
//     "valid" at once. It is also where live control (memory space 0xE0) calls
//     channel_changed() to keep the reported state truthful, which has no
//     equivalent in the bit model.
//   - A driver fault. It is not a state at all: it says something happened, and
//     there is no "unfaulted" event to pair it with.
//
// Threading: get_current_state() and set_state() are called on the stack's
// executor, while the hardware thread owns the drivers, so every accessor takes
// FemtoController's recursive hardware lock. The hardware thread never sends an
// event; it marks a shell pending and the refresh loop, which also runs on the
// stack's executor, does the sending.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_FEMTOBITS_HXX_
#define _FEMTOLCC_FEMTOBITS_HXX_

#include <stdint.h>

#include "openlcb/EventHandlerTemplates.hxx"
#include "openlcb/RefreshLoop.hxx"

#include "Config.h"
#include "board.h"

class FemtoController;

/// What one bit is attached to. The index means whichever of the board's things
/// the kind counts: an output, a pin, a packed board-and-line, and so on.
enum class BitKind : uint8_t
{
    Occupancy,      ///< produced: a block is occupied (on) or clear (off)
    TurnoutCmd,     ///< consumed: throw (on) or close (off)
    TurnoutPos,     ///< produced: thrown (on) or closed (off)
    PinIn,          ///< produced: an I/O pin's debounced input
    PinOut,         ///< consumed: an I/O pin driven as an output
    XioIn,          ///< produced: an expansion board line, 16 * board + line
    XioOut,         ///< consumed: the same, as an output
    ServoCmd,       ///< consumed: throw / light on, 16 * board + channel
    ServoPos,       ///< produced: a servo reached its position
};

/// One hardware bit, as OpenMRN models it.
///
/// Every accessor goes back through FemtoController, which owns the drivers and
/// the lock; this class is only the adapter that gives OpenMRN the shape it
/// wants.
class FemtoBit : public openlcb::BitEventInterface
{
public:
    /// @param controller owns the hardware this bit reads and writes.
    /// @param kind is what the bit is attached to.
    /// @param index is which one, in that kind's own numbering.
    /// @param on is the event meaning the bit became true.
    /// @param off is the event meaning it became false.
    FemtoBit(FemtoController *controller, BitKind kind, uint8_t index,
        uint64_t on, uint64_t off);

    openlcb::EventState get_current_state() override;
    void set_state(bool new_value) override;
    openlcb::Node *node() override;

private:
    FemtoController *controller_;
    BitKind kind_;
    uint8_t index_;
};

/// A fixed slot in the refresh loop, holding a producer that can be swapped.
///
/// RefreshLoop can be added to but never subtracted from, and stopping one
/// needs a wait on the executor that cannot be done from inside it. So the
/// slots are allocated once, for every bit the board could ever produce, and
/// stay registered for the life of the node; a configuration change swaps the
/// producer each one points at, or clears it. A slot with no producer costs one
/// virtual call and an immediate notify, thirty-three times a second.
class FemtoBitSlot : public openlcb::Polling
{
public:
    FemtoBitSlot()
        : producer_(nullptr)
        , pending_(false)
    {
    }

    /// Points the slot at a producer, or at nothing. Called on the stack's
    /// executor while the configuration is being applied.
    void set_producer(openlcb::BitEventProducer *producer)
    {
        producer_ = producer;
        pending_ = false;
    }

    /// Marks the bit as having changed, so the next refresh sends its event.
    /// Called from the hardware thread, which must not send anything itself.
    void trigger()
    {
        pending_ = true;
    }

    void poll_33hz(
        openlcb::WriteHelper *helper, Notifiable *done) override
    {
        if (producer_ == nullptr || !pending_)
        {
            done->notify();
            return;
        }
        pending_ = false;
        // Sends whichever of the two events matches the state the bit is in
        // now, which is what the hardware transition just made true.
        producer_->SendEventReport(helper, done);
    }

private:
    openlcb::BitEventProducer *producer_;
    volatile bool pending_;
};

// The slot numbering. Every bit the board can produce has one, whether or not
// it is configured, so that a slot's identity never moves when the settings
// change.
static const unsigned SLOT_OCCUPANCY = 0;
static const unsigned SLOT_TURNOUT   = SLOT_OCCUPANCY + NUM_CHANNELS;
static const unsigned SLOT_PIN_IN    = SLOT_TURNOUT + NUM_CHANNELS;
static const unsigned SLOT_XIO_IN    = SLOT_PIN_IN + EXP_GPIO_COUNT;
static const unsigned SLOT_SERVO_POS =
    SLOT_XIO_IN + NUM_XIO_BOARDS * XIO_LINES;
static const unsigned SLOT_COUNT =
    SLOT_SERVO_POS + NUM_SERVO_BOARDS * SERVO_CHANNELS;

#endif // _FEMTOLCC_FEMTOBITS_HXX_
