// Ties the configuration to the hardware and to the node's events.
//
// This is the OpenMRN counterpart of ../FemtoLCC/Controller.{h,cpp}, and it
// does the same job: read the settings, set each output up as a block, a
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
// Threading: everything here runs on the OpenMRN executor. Event handlers are
// called there, apply_configuration() is called there, and the periodic poll is
// a StateFlow that sleeps on the executor rather than a task of its own. That
// is deliberate: the expander's shadow register and the I2C bus are then
// touched from exactly one thread and need no locking.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_FEMTOCONTROLLER_HXX_
#define _FEMTOLCC_FEMTOCONTROLLER_HXX_

#include <stdint.h>

#include "executor/StateFlow.hxx"
#include "openlcb/EventHandlerTemplates.hxx"
#include "openlcb/SimpleStack.hxx"
#include "utils/ConfigUpdateListener.hxx"

#include "config.hxx"

#include "Channels.h"
#include "Config.h"
#include "Expander.h"
#include "IoPins.h"
#include "Occupancy.h"
#include "Turnouts.h"
#include "board.h"

/// What a consumed event does, or what a produced event reports. Stored in the
/// event registry entry's user argument along with the index it applies to, so
/// one handler can serve every event the node has.
enum class FemtoAction : uint8_t
{
    // Consumed: acted on when the event arrives.
    BlockOn = 0,    ///< power the block with DC
    BlockOff,       ///< switch the block off
    BlockDcc,       ///< pass the DCC track signal through
    Throw,          ///< drive the turnout to thrown
    Close,          ///< drive the turnout to closed
    PinOn,          ///< pull the I/O pin to ground
    PinOff,         ///< let the I/O pin go

    // Produced: sent when the hardware changes.
    Occupied,       ///< the block became occupied
    Clear,          ///< the block became clear
    Thrown,         ///< the turnout was commanded thrown
    Closed,         ///< the turnout was commanded closed
    PinActive,      ///< the input became active
    PinInactive,    ///< the input became inactive
    Fault,          ///< the driver reported a fault
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

/// The node's hardware, its settings and its events.
class FemtoController : public openlcb::SimpleEventHandler,
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
    /// derived from the node ID the same way the AOLCB firmware derives them.
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

    // --- the periodic poll, on the executor -------------------------------

    /// Samples occupancy, driver faults and the I/O pins, ends turnout pulses
    /// and sends whatever events those produce, then sleeps.
    Action poll();

    void poll_occupancy();
    void poll_faults();
    void poll_pins();

    // --- applying settings -------------------------------------------------

    void apply_channel(uint8_t ch, bool roleChanged, bool atBoot);
    void apply_pins();

    /// Drops every event registration and makes them again from the settings.
    /// Called on every configuration change, so that a tool that edits an event
    /// ID takes effect at once.
    void register_events();

    void consume(uint64_t eventId, FemtoAction action, uint8_t index);
    void produce(uint64_t eventId, FemtoAction action, uint8_t index);

    /// Sends an event report onto the bus.
    void send_event(uint64_t eventId);

    /// The state a produced or consumed event is in, for an identify reply.
    openlcb::EventState state_of(FemtoAction action, uint8_t index);

    openlcb::SimpleCanStack *stack_;
    const openlcb::ConfigDef cfg_;

    Expander expander_;
    Channels channels_;
    Occupancy occupancy_;
    Turnouts turnouts_;
    IoPins pins_;

    ChannelState channel_[NUM_CHANNELS];
    PinState pin_[EXP_GPIO_COUNT];

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
