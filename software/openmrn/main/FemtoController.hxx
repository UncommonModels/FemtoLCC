// Ties the configuration to the hardware and to the node's events.
//
// Its job: read the settings, set each output up as a block, a
// turnout or nothing, configure the I/O pins, register every configured event,
// act on the ones that are consumed, and send the ones that are produced.
//
// It is one class rather than a collection of ConfiguredConsumer and
// ConfiguredProducer objects because the board does not fit those shapes:
//
//   - A block is a three-state thing — off, DC, DCC — driven by three separate
//     consumed events. ConfiguredConsumer and BitEventConsumer both model a
//     single on/off bit.
//   - Occupancy is not a Gpio. It comes from oversampled current-sense readings
//     with hysteresis and asymmetric on/off delays, which Occupancy already
//     implements; it only needs its transitions turned into events.
//   - The I/O pins live behind an I2C port expander, so they are not Gpio
//     objects either, and IoPins already debounces them.
//   - Whether an output's events mean anything at all depends on its use, which
//     is itself a setting.
//
// OpenMRN's GPIO-shaped helpers are used where they fit, which on this board is
// nowhere; what is used instead is the machinery underneath them — the event
// registry, ConfigUpdateListener and the executor — which is the part that
// matters for standards conformance.
//
// Threading: the hardware poll runs on its own thread, not on the stack's
// executor.
//
// It used to share the executor, which was simpler and needed no locking. It
// cannot, because the poll blocks. Pulsed occupancy detection energises a block
// and busy-waits for the length of the pulse — about 2 ms — sampling the
// current, and ModuleStore's commit writes a SPIFFS file, which can take tens of
// milliseconds. On the stack's executor both stall CAN handling, because that
// same executor is what drains the TWAI driver through ::select().
//
// OpenMRN expects blocking work to be moved off: Esp32HardwareI2C.hxx tells
// callers to give blocking I/O its own Executor, and HubDevice.hxx says outright
// that such work "must be run on its own executor".
//
// So the poll is a StateFlow on hwExecutor_, and everything that touches the
// drivers — the poll, consumed events, the identify replies, a configuration
// load, and the 0xE0 and 0xE1 memory spaces — takes hwLock_ first. That lock is
// recursive, because live control calls back into channel_changed() while it
// already holds it. An LCC message may only be sent on the stack's executor, so
// send_event() posts the send there rather than making it from this thread.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_FEMTOCONTROLLER_HXX_
#define _FEMTOLCC_FEMTOCONTROLLER_HXX_

#include <stdint.h>

#include <memory>
#include <vector>

#include "executor/Executor.hxx"
#include "executor/Service.hxx"
#include "executor/StateFlow.hxx"
#include "openlcb/EventHandlerTemplates.hxx"
#include "openlcb/SimpleStack.hxx"
#include "os/OS.hxx"
#include "utils/ConfigUpdateListener.hxx"

#include "config.hxx"

#include "Channels.h"
#include "Config.h"
#include "DCCSource.h"
#include "Expander.h"
#include "FemtoBits.hxx"
#include "IoBoards.h"
#include "IoPins.h"
#include "LiveControl.hxx"
#include "ModuleStore.hxx"
#include "Occupancy.h"
#include "ServoBoards.h"
#include "Turnouts.h"
#include "board.h"

/// What a consumed event does, or what a produced event reports. Stored in the
/// event registry entry's user argument along with the index it applies to, so
/// one handler can serve every event the node has.
enum class FemtoAction : uint8_t
{
    // Consumed: acted on when the event arrives. These must stay first and
    // contiguous; is_consumed() is a comparison against LastConsumed.
    BlockOn = 0,    ///< power the block with DC
    BlockOff,       ///< switch the block off
    BlockDcc,       ///< pass the DCC track signal through
    Throw,          ///< drive the turnout to thrown
    Close,          ///< drive the turnout to closed
    PinOn,          ///< pull the I/O pin to ground
    PinOff,         ///< let the I/O pin go
    XioOn,          ///< drive an expansion board line
    XioOff,         ///< let an expansion board line go
    SvThrow,        ///< throw a servo, or switch a light on
    SvClose,        ///< close a servo, or switch a light off
    LastConsumed = SvClose,

    // Produced: sent when the hardware changes.
    Occupied,       ///< the block became occupied
    Clear,          ///< the block became clear
    Thrown,         ///< the turnout was commanded thrown
    Closed,         ///< the turnout was commanded closed
    PinActive,      ///< the input became active
    PinInactive,    ///< the input became inactive
    Fault,          ///< the driver reported a fault
    XioActive,      ///< an expansion board input became active
    XioInactive,    ///< an expansion board input became inactive
    SvThrown,       ///< a servo turnout reached thrown
    SvClosed,       ///< a servo turnout reached closed
};

/// Packs an action and the output or pin it applies to into the registry
/// entry's user argument.
static inline uint32_t femto_user_arg(FemtoAction action, uint8_t index)
{
    return ((uint32_t)action << 8) | index;
}

static inline FemtoAction femto_action_of(uint32_t user_arg)
{
    return (FemtoAction)((user_arg >> 8) & 0xFF);
}

static inline uint8_t femto_index_of(uint32_t user_arg)
{
    return (uint8_t)(user_arg & 0xFF);
}

/// Holds the thread the hardware poll runs on.
///
/// This is a base class rather than a pair of plain members because
/// StateFlowBase's constructor needs the Service, and base classes are built
/// before members. It is listed first in FemtoController's bases for that
/// reason.
class FemtoHwThread
{
protected:
    FemtoHwThread();

    /// The thread the hardware poll runs on.
    Executor<1> hwExecutor_;

    /// A Service bound to that thread, which is what StateFlowBase takes.
    Service hwService_;
};

/// The node's hardware, its settings and its events.
class FemtoController : private FemtoHwThread,
                        public openlcb::SimpleEventHandler,
                        public DefaultConfigUpdateListener,
                        public StateFlowBase
{
public:
    /// @param stack is the OpenLCB stack; the controller uses its node and its
    /// executor.
    /// @param cfg is the configuration layout, from config.hxx.
    FemtoController(openlcb::SimpleCanStack *stack,
        const openlcb::ConfigDef &cfg);

    /// Brings the I2C bus, the expander and the drivers up. Call once, before
    /// the stack starts, so that the first apply_configuration() has working
    /// hardware to talk to.
    void hw_init();

    // --- ConfigUpdateListener ---------------------------------------------

    /// Re-reads every setting and brings the hardware and the event
    /// registrations in line with it. At the initial load each output takes its
    /// power-on state; later, only an output whose use changed does.
    UpdateAction apply_configuration(
        int fd, bool initial_load, BarrierNotifiable *done) override;

    /// Puts every setting back to its default: the node name, each output
    /// unused, each pin unused, WiFi off, and an event ID for every event
    /// derived from the node ID (the suffix table is in FemtoController.cxx).
    void factory_reset(int fd) override;

    // --- EventHandler ------------------------------------------------------

    /// Acts on a consumed event.
    void handle_event_report(const openlcb::EventRegistryEntry &entry,
        openlcb::EventReport *event, BarrierNotifiable *done) override;

    /// Answers "what do you produce and consume?".
    void handle_identify_global(const openlcb::EventRegistryEntry &entry,
        openlcb::EventReport *event, BarrierNotifiable *done) override;

    /// Answers "do you consume this event, and what is its state?".
    void handle_identify_consumer(const openlcb::EventRegistryEntry &entry,
        openlcb::EventReport *event, BarrierNotifiable *done) override;

    /// Answers "do you produce this event, and what is its state?".
    void handle_identify_producer(const openlcb::EventRegistryEntry &entry,
        openlcb::EventReport *event, BarrierNotifiable *done) override;

    // --- for live control (memory space 0xE0) ------------------------------

    /// What output `ch` is set up as. Live control may only drive a block.
    ChannelRole role(uint8_t ch) const;

    /// Something other than an event drove an output directly — live control,
    /// or its lease running out. Re-reads what the hardware is actually doing
    /// so that the state this node reports for the block's power events stays
    /// truthful.
    void channel_changed(uint8_t ch);

    /// The 0xE0 memory space, for registering with the memory config server.
    /// Owned here because it needs the drivers, and because its lease has to be
    /// enforced from the poll below — both of which live on the executor.
    LiveControl *live_control()
    {
        return &liveControl_;
    }

    /// The 0xE1 memory space. begin() must be called once the file system is
    /// mounted, before the stack starts.
    ModuleStore *module_store()
    {
        return &moduleStore_;
    }

    /// The DCC source, for live control (memory space 0xE0).
    DCCSource *dcc()
    {
        return &dcc_;
    }

    /// The lock that guards every access to the drivers. Public because the two
    /// memory spaces are served on the stack's executor and so must take it
    /// before they touch the hardware this thread is driving. It is recursive:
    /// LiveControl holds it and calls back into channel_changed().
    OSMutex *hw_lock()
    {
        return &hwLock_;
    }

    // --- for FemtoBits -----------------------------------------------------

    /// The state of one hardware bit, as an identify reply wants it: VALID when
    /// the bit is on. This is state_of() underneath, so the bit handlers and
    /// the hand-rolled ones cannot drift apart. Takes the hardware lock.
    openlcb::EventState bit_state(BitKind kind, uint8_t index);

    /// Drives one hardware bit. This is the same switch a consumed event goes
    /// through, for the same reason. Takes the hardware lock.
    void bit_set(BitKind kind, uint8_t index, bool value);

    /// The node the bits send from.
    openlcb::Node *node();

private:
    /// The settings of one output, as last applied.
    struct ChannelState
    {
        ChannelRole role;
        PowerOnMode powerOn;
        bool dccReversed;
        TurnoutPowerOn turnoutPowerOn;

        /// What the block is doing now, so that a state query can be answered
        /// and a change can be reported.
        PowerOnMode mode;

        uint64_t evFault;
        uint64_t evOn, evOff, evDcc, evOccupied, evClear;
        uint64_t evThrow, evClose, evThrown, evClosed;
    };

    /// The settings of one I/O pin, as last applied.
    struct PinState
    {
        PinMode mode;
        uint64_t evActive, evInactive, evOn, evOff;
    };

    /// The events of one line on an I/O expansion board. The line's mode,
    /// polarity and debounce live in ioSettings_, because that is what the
    /// IoBoards driver takes; only the event IDs are kept separately.
    struct XioLineState
    {
        uint64_t evActive, evInactive, evOn, evOff;
    };

    // --- the periodic poll, on the executor -------------------------------

    /// Samples occupancy, driver faults and the I/O pins, ends turnout pulses
    /// and sends whatever events those produce, then sleeps.
    Action poll();

    void poll_occupancy();
    void poll_faults();
    void poll_pins();
    void poll_io_boards();
    void poll_servo_boards();

    // --- applying settings -------------------------------------------------

    void apply_channel(uint8_t ch, bool roleChanged, bool atBoot);
    void apply_pins();

    /// Drops every event registration and makes them again from the settings.
    /// Called on every configuration change, so that a tool that edits an event
    /// ID takes effect at once.
    void register_events();

    /// Acts on one consumed action. Shared by the hand-rolled event handler
    /// and by the bit consumers, so there is one description of what an
    /// incoming command does to the hardware.
    void drive(FemtoAction action, uint8_t index);

    void consume(uint64_t eventId, FemtoAction action, uint8_t index);
    void produce(uint64_t eventId, FemtoAction action, uint8_t index);

    /// Sends an event report onto the bus.
    void send_event(uint64_t eventId);

    /// The state a produced or consumed event is in, for an identify reply.
    openlcb::EventState state_of(FemtoAction action, uint8_t index);

    openlcb::SimpleCanStack *stack_;
    const openlcb::ConfigDef cfg_;

    /// Guards every driver below against the stack's executor. Recursive; see
    /// the threading note at the top of this file.
    OSMutex hwLock_ {true};

    Expander expander_;
    Channels channels_;
    Occupancy occupancy_;
    Turnouts turnouts_;
    IoPins pins_;

    /// The boards on the Qwiic connector. Both are always constructed; a board
    /// that is not configured is simply never probed.
    IoBoards ioBoards_;
    ServoBoards servoBoards_;

    /// The DCC waveform on channel A. Declared before liveControl_ because
    /// that takes a reference to it, and members are built in declaration
    /// order.
    DCCSource dcc_;

    LiveControl liveControl_;
    ModuleStore moduleStore_;

    ChannelState channel_[NUM_CHANNELS];
    PinState pin_[EXP_GPIO_COUNT];

    /// Expansion board settings, as last applied, in the shape the drivers
    /// take. The servo settings carry their own event IDs; the I/O board lines
    /// do not, so those are in xio_.
    XioLineState xio_[NUM_XIO_BOARDS][XIO_LINES];
    IoBoardSettings ioSettings_[NUM_XIO_BOARDS];
    ServoBoardSettings svSettings_[NUM_SERVO_BOARDS];

    // --- the bit layer (see FemtoBits.hxx) ---------------------------------

    /// One slot per bit the board could ever produce, allocated once and
    /// registered with the refresh loop for the life of the node, because
    /// RefreshLoop cannot be subtracted from. A configuration change swaps the
    /// producer a slot points at.
    FemtoBitSlot slots_[SLOT_COUNT];

    /// The adapters and the handlers over them, rebuilt whenever the settings
    /// change. The handlers register in their constructors and unregister in
    /// their destructors, so the old ones must be destroyed before the new ones
    /// are made.
    std::vector<std::unique_ptr<FemtoBit>> bits_;
    std::vector<std::unique_ptr<openlcb::BitEventProducer>> bitProducers_;
    std::vector<std::unique_ptr<openlcb::BitEventConsumer>> bitConsumers_;

    /// Drives every slot at 33 Hz. Built on the first configuration load, once
    /// the node exists.
    std::unique_ptr<openlcb::RefreshLoop> refreshLoop_;

    /// Makes a producer bit and points its slot at it.
    void add_producer_bit(
        BitKind kind, uint8_t index, unsigned slot, uint64_t on, uint64_t off);

    /// Makes a consumer bit.
    void add_consumer_bit(
        BitKind kind, uint8_t index, uint64_t on, uint64_t off);

    /// Latched so that a driver holding nFAULT low sends one event, not a
    /// stream of them.
    bool faultLatched_[NUM_CHANNELS];

    /// True once apply_configuration() has run, so the poll does not touch
    /// hardware that has not been set up.
    bool applied_;

    /// Set while acting on an event, so that a change the node made itself is
    /// still reported but cannot recurse.
    bool emitting_;

    StateFlowTimer timer_;
};

#endif // _FEMTOLCC_FEMTOCONTROLLER_HXX_
