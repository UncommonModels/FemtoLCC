// The settings, the hardware and the events. See FemtoController.hxx.
//
//   Uncommon Models — https://uncommonmodels.com

#include "FemtoController.hxx"

#include <string.h>

#include "executor/Notifiable.hxx"
#include "openlcb/EventHandler.hxx"
#include "openlcb/TcpDefs.hxx"
#include "openlcb/WriteHelper.hxx"

#include "Arduino.h"
#include "Wire.h"

using openlcb::Defs;
using openlcb::EventRegistry;
using openlcb::EventRegistryEntry;
using openlcb::EventReport;
using openlcb::EventState;
using openlcb::WriteHelper;

/// How often the hardware is looked at. Occupancy samples one channel per pass,
/// so every block is read about every 20 ms, and the expander's port A is read
/// no faster than IoPins' own 5 ms floor.
static const unsigned POLL_MS = 5;

/// I2C: 100 kHz, and a short timeout so a missing chip cannot stall the
/// executor. The board has its own pull-ups at R2/R3.
static const uint32_t I2C_HZ = 100000;
static const uint16_t I2C_TIMEOUT_MS = 10;

// ---------------------------------------------------------------------------
// Event IDs
// ---------------------------------------------------------------------------

/// The node ID in the top six bytes, a suffix in the low two — the same scheme
/// the AOLCB firmware uses, so a layout's event IDs mean the same thing
/// whichever firmware is on the board.
static inline uint64_t event_for(uint64_t nodeId, uint16_t suffix)
{
    return (nodeId << 16) | suffix;
}

// Default suffixes, matching site/content/docs/configuration.md.
static const uint16_t EV_BLOCK_ON     = 0x0100;   // + channel
static const uint16_t EV_BLOCK_OFF    = 0x0110;
static const uint16_t EV_BLOCK_DCC    = 0x0120;
static const uint16_t EV_FAULT        = 0x0200;
static const uint16_t EV_OCCUPIED     = 0x0300;
static const uint16_t EV_CLEAR        = 0x0310;
static const uint16_t EV_THROW        = 0x0400;
static const uint16_t EV_CLOSE        = 0x0410;
static const uint16_t EV_THROWN       = 0x0420;
static const uint16_t EV_CLOSED       = 0x0430;
static const uint16_t EV_PIN_ACTIVE   = 0x0500;   // + pin
static const uint16_t EV_PIN_INACTIVE = 0x0510;
static const uint16_t EV_PIN_ON       = 0x0600;
static const uint16_t EV_PIN_OFF      = 0x0610;

/// The MTI that answers a consumer identify in a given state.
static Defs::MTI consumer_identified(EventState state)
{
    switch (state)
    {
        case EventState::VALID:
            return Defs::MTI_CONSUMER_IDENTIFIED_VALID;
        case EventState::INVALID:
            return Defs::MTI_CONSUMER_IDENTIFIED_INVALID;
        default:
            return Defs::MTI_CONSUMER_IDENTIFIED_UNKNOWN;
    }
}

/// The MTI that answers a producer identify in a given state.
static Defs::MTI producer_identified(EventState state)
{
    switch (state)
    {
        case EventState::VALID:
            return Defs::MTI_PRODUCER_IDENTIFIED_VALID;
        case EventState::INVALID:
            return Defs::MTI_PRODUCER_IDENTIFIED_INVALID;
        default:
            return Defs::MTI_PRODUCER_IDENTIFIED_UNKNOWN;
    }
}

/// True for the actions the node consumes; the rest it produces.
static bool is_consumed(FemtoAction action)
{
    return action <= FemtoAction::PinOff;
}

// ---------------------------------------------------------------------------

FemtoController::FemtoController(
    openlcb::SimpleCanStack *stack, const openlcb::ConfigDef &cfg)
    : StateFlowBase(stack->service())
    , stack_(stack)
    , cfg_(cfg)
    , expander_(MCP23018_ADDR)
    , channels_(expander_)
    , occupancy_(channels_)
    , turnouts_(channels_)
    , pins_(expander_)
    , applied_(false)
    , emitting_(false)
    , timer_(this)
{
    memset(channel_, 0, sizeof(channel_));
    memset(pin_, 0, sizeof(pin_));
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i)
    {
        channel_[i].role = ChannelRole::Unused;
        channel_[i].mode = PowerOnMode::Off;
        faultLatched_[i] = false;
    }
    for (uint8_t i = 0; i < EXP_GPIO_COUNT; ++i)
    {
        pin_[i].mode = PinMode::Unused;
    }
}

void FemtoController::hw_init()
{
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);

    if (!expander_.begin())
    {
        // Without the expander the four dcc_en lines and three of the four dir
        // lines cannot be driven. The node still runs and still speaks LCC, so
        // it can be configured and the fault diagnosed.
        printf("MCP23018 at 0x%02X is not answering; outputs B-D and DCC mode "
               "will not work.\n", MCP23018_ADDR);
    }

    channels_.begin();

    // Measure each channel's zero-current reading with everything off, before
    // any track power. Occupancy subtracts it from every later reading.
    occupancy_.calibrate();

    // Start the poll. It runs on the executor, so nothing else touches the
    // expander or the bus.
    start_flow(STATE(poll));
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

ConfigUpdateListener::UpdateAction FemtoController::apply_configuration(
    int fd, bool initial_load, BarrierNotifiable *done)
{
    AutoNotify n(done);

    // Whether any event ID, or any output's or pin's use, actually changed. If
    // so the node must re-announce what it produces and consumes, which is what
    // returning REINIT_NEEDED below makes the stack do.
    bool eventsChanged = false;

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i)
    {
        const auto ch = cfg_.seg().channels().entry(i);
        ChannelState &s = channel_[i];

        const ChannelRole roleBefore = s.role;
        const uint64_t evBefore[] = { s.evFault, s.evOn, s.evOff, s.evDcc,
            s.evOccupied, s.evClear, s.evThrow, s.evClose, s.evThrown,
            s.evClosed };

        const ChannelRole role = (ChannelRole)ch.role().read(fd);
        const bool roleChanged = (role != s.role) || initial_load;
        s.role = role;

        s.evFault = ch.event_fault().read(fd);

        const auto blk = ch.block();
        s.powerOn = (PowerOnMode)blk.power_on().read(fd);
        s.dccReversed = blk.dcc_reversed().read(fd) != 0;
        s.evOn = blk.event_on().read(fd);
        s.evOff = blk.event_off().read(fd);
        s.evDcc = blk.event_dcc().read(fd);
        s.evOccupied = blk.event_occupied().read(fd);
        s.evClear = blk.event_clear().read(fd);
        occupancy_.setThresholds(
            i, blk.occupied_ma().read(fd), blk.clear_ma().read(fd));
        occupancy_.setEnabled(i, role == ChannelRole::Block);

        const auto to = ch.turnout();
        s.turnoutPowerOn = (TurnoutPowerOn)to.power_on().read(fd);
        s.evThrow = to.event_throw().read(fd);
        s.evClose = to.event_close().read(fd);
        s.evThrown = to.event_thrown().read(fd);
        s.evClosed = to.event_closed().read(fd);

        if (role == ChannelRole::Turnout)
        {
            ChannelSettings cs;
            memset(&cs, 0, sizeof(cs));
            cs.role = role;
            cs.motor = (MotorType)to.motor().read(fd);
            cs.pulseMs = to.pulse_ms().read(fd);
            cs.duty = to.duty().read(fd);
            cs.reverse = to.reverse().read(fd) != 0;
            turnouts_.attach(i, cs);
        }
        else
        {
            turnouts_.detach(i);
        }

        const uint64_t evAfter[] = { s.evFault, s.evOn, s.evOff, s.evDcc,
            s.evOccupied, s.evClear, s.evThrow, s.evClose, s.evThrown,
            s.evClosed };
        if (roleBefore != s.role ||
            memcmp(evBefore, evAfter, sizeof(evBefore)) != 0)
        {
            eventsChanged = true;
        }

        apply_channel(i, roleChanged, initial_load);
    }

    PinSettings ps[EXP_GPIO_COUNT];
    for (uint8_t i = 0; i < EXP_GPIO_COUNT; ++i)
    {
        const auto p = cfg_.seg().pins().entry(i);
        PinState &s = pin_[i];

        const PinMode modeBefore = s.mode;
        const uint64_t evBefore[] =
            { s.evActive, s.evInactive, s.evOn, s.evOff };

        s.mode = (PinMode)p.mode().read(fd);
        s.evActive = p.event_active().read(fd);
        s.evInactive = p.event_inactive().read(fd);
        s.evOn = p.event_on().read(fd);
        s.evOff = p.event_off().read(fd);

        ps[i].mode = s.mode;
        ps[i].invert = p.invert().read(fd) != 0;
        ps[i].debounceMs = p.debounce_ms().read(fd);

        const uint64_t evAfter[] =
            { s.evActive, s.evInactive, s.evOn, s.evOff };
        if (modeBefore != s.mode ||
            memcmp(evBefore, evAfter, sizeof(evBefore)) != 0)
        {
            eventsChanged = true;
        }
    }
    pins_.configure(ps);
    channels_.flush();

    register_events();
    applied_ = true;

    // Everything above takes effect at once, with no reboot. But if an event ID
    // or an output's use changed, the registry alone is not enough: the node has
    // to tell the bus what it now produces and consumes, or a tool that already
    // queried it keeps the old answer. REINIT_NEEDED makes the stack re-run node
    // initialisation, which re-announces the lot.
    //
    // Not at the initial load: the node has not announced anything yet, and
    // asking for a re-init there would only make it do it twice.
    //
    // The WiFi credentials are read by main.cxx before the stack starts, so a
    // change to those still needs a restart; Esp32WiFiManager reports its own
    // need to reboot separately.
    return (eventsChanged && !initial_load) ? REINIT_NEEDED : UPDATED;
}

void FemtoController::apply_channel(uint8_t ch, bool roleChanged, bool atBoot)
{
    ChannelState &s = channel_[ch];

    if (!roleChanged)
    {
        // The use did not change, so the output keeps running with its new
        // settings rather than being interrupted.
        return;
    }

    faultLatched_[ch] = false;

    switch (s.role)
    {
        case ChannelRole::Block:
            switch (s.powerOn)
            {
                case PowerOnMode::DC:
                    channels_.set(ch, ChannelMode::DC, false, 255);
                    break;
                case PowerOnMode::DCC:
                    channels_.set(ch, ChannelMode::DCC, s.dccReversed, 0);
                    break;
                default:
                    channels_.set(ch, ChannelMode::Off, false, 0);
                    break;
            }
            s.mode = s.powerOn;
            break;

        case ChannelRole::Turnout:
            switch (s.turnoutPowerOn)
            {
                case TurnoutPowerOn::Closed:
                    turnouts_.set(ch, false);
                    break;
                case TurnoutPowerOn::Thrown:
                    turnouts_.set(ch, true);
                    break;
                default:
                    // Leave alone: no signal until the first command.
                    break;
            }
            s.mode = PowerOnMode::Off;
            break;

        default:
            channels_.set(ch, ChannelMode::Off, false, 0);
            s.mode = PowerOnMode::Off;
            break;
    }
    (void)atBoot;
}

void FemtoController::factory_reset(int fd)
{
    const uint64_t nodeId = stack_->node()->node_id();

    cfg_.userinfo().name().write(fd, openlcb::SNIP_STATIC_DATA.model_name);
    cfg_.userinfo().description().write(fd, "");

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i)
    {
        const auto ch = cfg_.seg().channels().entry(i);
        ch.description().write(fd, "");
        CDI_FACTORY_RESET(ch.role);
        ch.event_fault().write(fd, event_for(nodeId, EV_FAULT + i));

        const auto blk = ch.block();
        CDI_FACTORY_RESET(blk.power_on);
        CDI_FACTORY_RESET(blk.dcc_reversed);
        CDI_FACTORY_RESET(blk.occupied_ma);
        CDI_FACTORY_RESET(blk.clear_ma);
        blk.event_on().write(fd, event_for(nodeId, EV_BLOCK_ON + i));
        blk.event_off().write(fd, event_for(nodeId, EV_BLOCK_OFF + i));
        blk.event_dcc().write(fd, event_for(nodeId, EV_BLOCK_DCC + i));
        blk.event_occupied().write(fd, event_for(nodeId, EV_OCCUPIED + i));
        blk.event_clear().write(fd, event_for(nodeId, EV_CLEAR + i));

        const auto to = ch.turnout();
        CDI_FACTORY_RESET(to.motor);
        CDI_FACTORY_RESET(to.pulse_ms);
        CDI_FACTORY_RESET(to.duty);
        CDI_FACTORY_RESET(to.reverse);
        CDI_FACTORY_RESET(to.power_on);
        to.event_throw().write(fd, event_for(nodeId, EV_THROW + i));
        to.event_close().write(fd, event_for(nodeId, EV_CLOSE + i));
        to.event_thrown().write(fd, event_for(nodeId, EV_THROWN + i));
        to.event_closed().write(fd, event_for(nodeId, EV_CLOSED + i));
    }

    for (uint8_t i = 0; i < EXP_GPIO_COUNT; ++i)
    {
        const auto p = cfg_.seg().pins().entry(i);
        p.description().write(fd, "");
        CDI_FACTORY_RESET(p.mode);
        CDI_FACTORY_RESET(p.invert);
        CDI_FACTORY_RESET(p.debounce_ms);
        p.event_active().write(fd, event_for(nodeId, EV_PIN_ACTIVE + i));
        p.event_inactive().write(fd, event_for(nodeId, EV_PIN_INACTIVE + i));
        p.event_on().write(fd, event_for(nodeId, EV_PIN_ON + i));
        p.event_off().write(fd, event_for(nodeId, EV_PIN_OFF + i));
    }

    const auto creds = cfg_.seg().wifi_credentials();
    CDI_FACTORY_RESET(creds.enable);
    creds.ssid().write(fd, "");
    creds.password().write(fd, "");
    creds.hostname().write(fd, "");

    // OpenMRN's own WiFi group has to be reset here too.
    //
    // Esp32WiFiManager has a perfectly good factory_reset() of its own, but it
    // only runs on listeners that are registered when the settings file is
    // created, and the manager cannot be constructed that early: it takes the
    // network name and password as constructor arguments, so main.cxx has to
    // read them out of the very file being created. On a fresh board its group
    // would therefore be left as zeros — hub port 0, no mDNS service name —
    // which is not a working configuration.
    //
    // These are the same defaults Esp32WiFiManager::factory_reset writes.
    const auto wifi = cfg_.seg().wifi();
    CDI_FACTORY_RESET(wifi.sleep);
    CDI_FACTORY_RESET(wifi.connection_mode);

    CDI_FACTORY_RESET(wifi.hub().port);
    wifi.hub().service_name().write(
        fd, openlcb::TcpDefs::MDNS_SERVICE_NAME_GRIDCONNECT_CAN_TCP);

    CDI_FACTORY_RESET(wifi.uplink().search_mode);
    CDI_FACTORY_RESET(wifi.uplink().reconnect);
    wifi.uplink().manual_address().ip_address().write(fd, "");
    CDI_FACTORY_RESET(wifi.uplink().manual_address().port);
    wifi.uplink().auto_address().service_name().write(
        fd, openlcb::TcpDefs::MDNS_SERVICE_NAME_GRIDCONNECT_CAN_TCP);
    wifi.uplink().auto_address().host_name().write(fd, "");
    wifi.uplink().last_address().ip_address().write(fd, "");
    CDI_FACTORY_RESET(wifi.uplink().last_address().port);
}

// ---------------------------------------------------------------------------
// Event registration
// ---------------------------------------------------------------------------

void FemtoController::consume(
    uint64_t eventId, FemtoAction action, uint8_t index)
{
    if (eventId == 0)
    {
        return;     // an event of all zeros does nothing
    }
    EventRegistry::instance()->register_handler(
        EventRegistryEntry(this, eventId, femto_user_arg(action, index)), 0);
}

void FemtoController::produce(
    uint64_t eventId, FemtoAction action, uint8_t index)
{
    consume(eventId, action, index);
}

void FemtoController::register_events()
{
    // Drop the lot and make them again, so that an event ID edited in a
    // configuration tool takes effect without a reboot.
    EventRegistry::instance()->unregister_handler(this);

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i)
    {
        const ChannelState &s = channel_[i];
        if (s.role == ChannelRole::Unused)
        {
            continue;
        }

        // The fault event belongs to the output whatever its use.
        produce(s.evFault, FemtoAction::Fault, i);

        if (s.role == ChannelRole::Block)
        {
            consume(s.evOn, FemtoAction::BlockOn, i);
            consume(s.evOff, FemtoAction::BlockOff, i);
            consume(s.evDcc, FemtoAction::BlockDcc, i);
            produce(s.evOccupied, FemtoAction::Occupied, i);
            produce(s.evClear, FemtoAction::Clear, i);
        }
        else
        {
            consume(s.evThrow, FemtoAction::Throw, i);
            consume(s.evClose, FemtoAction::Close, i);
            produce(s.evThrown, FemtoAction::Thrown, i);
            produce(s.evClosed, FemtoAction::Closed, i);
        }
    }

    for (uint8_t i = 0; i < EXP_GPIO_COUNT; ++i)
    {
        const PinState &s = pin_[i];
        switch (s.mode)
        {
            case PinMode::Input:
            case PinMode::InputPullup:
                produce(s.evActive, FemtoAction::PinActive, i);
                produce(s.evInactive, FemtoAction::PinInactive, i);
                break;
            case PinMode::Output:
                consume(s.evOn, FemtoAction::PinOn, i);
                consume(s.evOff, FemtoAction::PinOff, i);
                break;
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Consuming events
// ---------------------------------------------------------------------------

void FemtoController::handle_event_report(const EventRegistryEntry &entry,
    EventReport *event, BarrierNotifiable *done)
{
    AutoNotify n(done);

    const FemtoAction action = femto_action_of(entry.user_arg);
    const uint8_t index = femto_index_of(entry.user_arg);

    if (!applied_ || !is_consumed(action))
    {
        return;
    }

    emitting_ = true;
    switch (action)
    {
        case FemtoAction::BlockOn:
            channels_.set(index, ChannelMode::DC, false, 255);
            channel_[index].mode = PowerOnMode::DC;
            break;

        case FemtoAction::BlockOff:
            channels_.set(index, ChannelMode::Off, false, 0);
            channel_[index].mode = PowerOnMode::Off;
            break;

        case FemtoAction::BlockDcc:
            channels_.set(
                index, ChannelMode::DCC, channel_[index].dccReversed, 0);
            channel_[index].mode = PowerOnMode::DCC;
            break;

        case FemtoAction::Throw:
            turnouts_.set(index, true);
            send_event(channel_[index].evThrown);
            break;

        case FemtoAction::Close:
            turnouts_.set(index, false);
            send_event(channel_[index].evClosed);
            break;

        case FemtoAction::PinOn:
            pins_.setOutput(index, true);
            break;

        case FemtoAction::PinOff:
            pins_.setOutput(index, false);
            break;

        default:
            break;
    }
    channels_.flush();
    emitting_ = false;
}

// ---------------------------------------------------------------------------
// Identify
// ---------------------------------------------------------------------------

EventState FemtoController::state_of(FemtoAction action, uint8_t index)
{
    if (!applied_)
    {
        return EventState::UNKNOWN;
    }

    switch (action)
    {
        // A block power event is valid when the block is in that mode.
        case FemtoAction::BlockOn:
            return channel_[index].mode == PowerOnMode::DC ? EventState::VALID
                                                           : EventState::INVALID;
        case FemtoAction::BlockOff:
            return channel_[index].mode == PowerOnMode::Off ? EventState::VALID
                                                            : EventState::INVALID;
        case FemtoAction::BlockDcc:
            return channel_[index].mode == PowerOnMode::DCC ? EventState::VALID
                                                            : EventState::INVALID;

        case FemtoAction::Occupied:
            switch (occupancy_.state(index))
            {
                case BlockState::Occupied: return EventState::VALID;
                case BlockState::Clear:    return EventState::INVALID;
                default:                   return EventState::UNKNOWN;
            }
        case FemtoAction::Clear:
            switch (occupancy_.state(index))
            {
                case BlockState::Clear:    return EventState::VALID;
                case BlockState::Occupied: return EventState::INVALID;
                default:                   return EventState::UNKNOWN;
            }

        // A turnout has no feedback: the state is what was last commanded.
        case FemtoAction::Throw:
        case FemtoAction::Thrown:
            switch (turnouts_.position(index))
            {
                case TurnoutPosition::Thrown: return EventState::VALID;
                case TurnoutPosition::Closed: return EventState::INVALID;
                default:                      return EventState::UNKNOWN;
            }
        case FemtoAction::Close:
        case FemtoAction::Closed:
            switch (turnouts_.position(index))
            {
                case TurnoutPosition::Closed: return EventState::VALID;
                case TurnoutPosition::Thrown: return EventState::INVALID;
                default:                      return EventState::UNKNOWN;
            }

        case FemtoAction::PinActive:
            return pins_.active(index) ? EventState::VALID : EventState::INVALID;
        case FemtoAction::PinInactive:
            return pins_.active(index) ? EventState::INVALID : EventState::VALID;

        case FemtoAction::PinOn:
            return pins_.output(index) ? EventState::VALID : EventState::INVALID;
        case FemtoAction::PinOff:
            return pins_.output(index) ? EventState::INVALID : EventState::VALID;

        // A fault is an event, not a state: it says something happened.
        case FemtoAction::Fault:
        default:
            return EventState::UNKNOWN;
    }
}

void FemtoController::handle_identify_global(const EventRegistryEntry &entry,
    EventReport *event, BarrierNotifiable *done)
{
    if (event->dst_node && event->dst_node != stack_->node())
    {
        return done->notify();
    }

    const FemtoAction action = femto_action_of(entry.user_arg);
    const uint8_t index = femto_index_of(entry.user_arg);
    const EventState state = state_of(action, index);

    event->event_write_helper<1>()->WriteAsync(stack_->node(),
        is_consumed(action) ? consumer_identified(state)
                            : producer_identified(state),
        WriteHelper::global(), openlcb::eventid_to_buffer(entry.event), done);
}

void FemtoController::handle_identify_consumer(const EventRegistryEntry &entry,
    EventReport *event, BarrierNotifiable *done)
{
    const FemtoAction action = femto_action_of(entry.user_arg);
    if (!is_consumed(action))
    {
        return done->notify();
    }
    event->event_write_helper<1>()->WriteAsync(stack_->node(),
        consumer_identified(state_of(action, femto_index_of(entry.user_arg))),
        WriteHelper::global(), openlcb::eventid_to_buffer(entry.event), done);
}

void FemtoController::handle_identify_producer(const EventRegistryEntry &entry,
    EventReport *event, BarrierNotifiable *done)
{
    const FemtoAction action = femto_action_of(entry.user_arg);
    if (is_consumed(action))
    {
        return done->notify();
    }
    event->event_write_helper<1>()->WriteAsync(stack_->node(),
        producer_identified(state_of(action, femto_index_of(entry.user_arg))),
        WriteHelper::global(), openlcb::eventid_to_buffer(entry.event), done);
}

// ---------------------------------------------------------------------------
// Producing events
// ---------------------------------------------------------------------------

void FemtoController::send_event(uint64_t eventId)
{
    if (eventId == 0)
    {
        return;
    }
    stack_->send_event(eventId);
}

// ---------------------------------------------------------------------------
// The poll
// ---------------------------------------------------------------------------

StateFlowBase::Action FemtoController::poll()
{
    if (applied_)
    {
        poll_occupancy();
        poll_faults();
        poll_pins();
        turnouts_.update();
        channels_.flush();
    }
    return sleep_and_call(&timer_, MSEC_TO_NSEC(POLL_MS), STATE(poll));
}

void FemtoController::poll_occupancy()
{
    occupancy_.poll();

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i)
    {
        if (channel_[i].role != ChannelRole::Block)
        {
            continue;
        }
        BlockState now;
        if (!occupancy_.takeTransition(i, now))
        {
            continue;
        }
        if (now == BlockState::Occupied)
        {
            send_event(channel_[i].evOccupied);
        }
        else if (now == BlockState::Clear)
        {
            send_event(channel_[i].evClear);
        }
        // Unknown means the block is not energised, so nothing is reported.
    }
}

void FemtoController::poll_faults()
{
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i)
    {
        if (channel_[i].role == ChannelRole::Unused)
        {
            continue;
        }

        const bool faulted = channels_.faulted(i);
        if (faulted && !faultLatched_[i])
        {
            // Switch the output off before reporting, so the driver is not left
            // trying to drive a short.
            faultLatched_[i] = true;
            channels_.set(i, ChannelMode::Off, false, 0);
            channel_[i].mode = PowerOnMode::Off;
            turnouts_.release(i);
            send_event(channel_[i].evFault);
        }
        else if (!faulted)
        {
            faultLatched_[i] = false;
        }
    }
}

void FemtoController::poll_pins()
{
    pins_.poll();

    for (uint8_t i = 0; i < EXP_GPIO_COUNT; ++i)
    {
        if (!pins_.isInput(i))
        {
            continue;
        }
        bool active;
        if (!pins_.takeTransition(i, active))
        {
            continue;
        }
        send_event(active ? pin_[i].evActive : pin_[i].evInactive);
    }
}
