// The settings, the hardware and the events. See FemtoController.hxx.
//
//   Uncommon Models — https://uncommonmodels.com

#include "FemtoController.hxx"

#include <string.h>

#include "executor/Executable.hxx"
#include "executor/Notifiable.hxx"
#include "openlcb/EventHandler.hxx"
#include "openlcb/TcpDefs.hxx"
#include "openlcb/WriteHelper.hxx"

#include "Arduino.h"
#include "I2cBus.h"
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

// ---------------------------------------------------------------------------
// Event IDs
// ---------------------------------------------------------------------------

/// The node ID in the top six bytes, a suffix in the low two. The suffixes
/// below are part of the board's published interface: a layout wired to the
/// default event IDs depends on them, so they are not to be renumbered.
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

// Expansion boards. The index is 16 * board + line (or channel), so board 1
// line 1 is 0x00 and board 2 channel 3 is 0x12.
static const uint16_t EV_XIO_ACTIVE   = 0x0700;
static const uint16_t EV_XIO_INACTIVE = 0x0800;
static const uint16_t EV_XIO_ON       = 0x0900;
static const uint16_t EV_XIO_OFF      = 0x0A00;
static const uint16_t EV_SV_THROW     = 0x0B00;
static const uint16_t EV_SV_CLOSE     = 0x0C00;
static const uint16_t EV_SV_THROWN    = 0x0D00;
static const uint16_t EV_SV_CLOSED    = 0x0E00;

// Default I2C addresses. Each board gets the next one up, so four boards
// straight out of a factory reset do not collide.
static const uint8_t DEFAULT_XIO_ADDRESS = 0x21;
static const uint8_t DEFAULT_SVB_ADDRESS = 0x40;

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

// Detection while a block is switched off: the defaults and the limits a
// setting is clamped to.
static const uint8_t  DEFAULT_DETECT_LEN_100US   = 20;      // 2 ms
static const uint8_t  MIN_DETECT_LEN_100US       = 10;
static const uint8_t  MAX_DETECT_LEN_100US       = 50;
static const uint16_t DEFAULT_DETECT_INTERVAL_MS = 300;
static const uint16_t MIN_DETECT_INTERVAL_MS     = 100;
static const uint16_t MAX_DETECT_INTERVAL_MS     = 10000;

/// Zero means "use the default"; anything else is clamped into range. This is
/// what lets a board whose spare bytes were never written still pulse.
static uint16_t or_default(
    uint32_t v, uint16_t fallback, uint16_t lo, uint16_t hi)
{
    if (v == 0)
    {
        return fallback;
    }
    return v < lo ? lo : (v > hi ? hi : (uint16_t)v);
}

/// True for the actions the node consumes; the rest it produces.
static bool is_consumed(FemtoAction action)
{
    return action <= FemtoAction::LastConsumed;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The hardware thread
// ---------------------------------------------------------------------------

/// The hardware poll runs at the same priority as the task the stack's executor
/// is on.
///
/// ESP-IDF starts app_main at ESP_TASK_MAIN_PRIO, which is 1, and
/// loop_executor() keeps that task for the stack. The only FreeRTOS priority
/// below 1 is 0, the idle task, so the hardware thread cannot be put below the
/// stack at all. Note that 0 must not be used to try: OpenMRN's
/// os_thread_create() treats a priority of 0 as "pick one" and substitutes
/// configMAX_PRIORITIES / 2, which would put this thread far *above* the stack.
/// Equal priority is the useful choice instead:
/// FreeRTOS time-slices two ready tasks of the same priority on each tick, so
/// the stack takes over from a pulse within one tick rather than waiting the
/// whole pulse out.
static const int HW_THREAD_PRIORITY = 1;

/// Enough for the I2C driver, the SPIFFS write ModuleStore does, and printf.
static const size_t HW_THREAD_STACK = 4096;

FemtoHwThread::FemtoHwThread()
    : hwExecutor_("femto_hw", HW_THREAD_PRIORITY, HW_THREAD_STACK)
    , hwService_(&hwExecutor_)
{
}

FemtoController::FemtoController(
    openlcb::SimpleCanStack *stack, const openlcb::ConfigDef &cfg)
    : StateFlowBase(&hwService_)
    , stack_(stack)
    , cfg_(cfg)
    , expander_(MCP23018_ADDR)
    , channels_(expander_)
    , occupancy_(channels_)
    , turnouts_(channels_)
    , pins_(expander_)
    , dcc_(channels_)
    , liveControl_(channels_, occupancy_, *this, dcc_)
    , moduleStore_(&hwLock_)
    , applied_(false)
    , emitting_(false)
    , timer_(this)
{
    memset(channel_, 0, sizeof(channel_));
    memset(pin_, 0, sizeof(pin_));
    memset(xio_, 0, sizeof(xio_));
    memset(ioSettings_, 0, sizeof(ioSettings_));
    memset(svSettings_, 0, sizeof(svSettings_));
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
    // The I2cBus driver owns the bus: it starts it at 100 kHz with the
    // short timeout, and the expansion board drivers switch it to 400 kHz and
    // back through i2cSetFast().
    i2cBegin();

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
// Live control
// ---------------------------------------------------------------------------

ChannelRole FemtoController::role(uint8_t ch) const
{
    return ch < NUM_CHANNELS ? channel_[ch].role : ChannelRole::Unused;
}

void FemtoController::channel_changed(uint8_t ch)
{
    if (ch >= NUM_CHANNELS)
    {
        return;
    }
    // Live control already holds this; the lock is recursive so that it can.
    OSMutexLock l(&hwLock_);
    // The block's three power events are consumed, and the state this node
    // reports for them is read back out of channel_[ch].mode, so that is what
    // has to follow the hardware.
    switch (channels_.mode(ch))
    {
        case ChannelMode::DC:
            channel_[ch].mode = PowerOnMode::DC;
            break;
        case ChannelMode::DCC:
            channel_[ch].mode = PowerOnMode::DCC;
            break;
        default:
            channel_[ch].mode = PowerOnMode::Off;
            break;
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

ConfigUpdateListener::UpdateAction FemtoController::apply_configuration(
    int fd, bool initial_load, BarrierNotifiable *done)
{
    AutoNotify n(done);

    // Runs on the stack's executor and writes to every driver, so it waits for
    // the hardware thread to finish whatever it is in the middle of.
    OSMutexLock l(&hwLock_);

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

        // Detection while the block is switched off: pulse it briefly and read
        // the current during the pulse. The driver spreads the four channels
        // across the interval so only one is ever pulsing.
        occupancy_.setPulseDetect(i,
            blk.detect_while_off().read(fd) == 0,    // PulseDetect::On is 0
            (uint16_t)(or_default(blk.pulse_len_100us().read(fd),
                           DEFAULT_DETECT_LEN_100US, MIN_DETECT_LEN_100US,
                           MAX_DETECT_LEN_100US) * 100u),
            or_default(blk.pulse_interval_ms().read(fd),
                DEFAULT_DETECT_INTERVAL_MS, MIN_DETECT_INTERVAL_MS,
                MAX_DETECT_INTERVAL_MS));

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

    // I/O expansion boards on the Qwiic connector. The driver probes each one
    // as it is configured, and keeps retrying a board that does not answer.
    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b)
    {
        const auto board = cfg_.seg().io_boards().entry(b);
        IoBoardSettings &bs = ioSettings_[b];

        const IoBoardType typeBefore = bs.type;
        const uint8_t addressBefore = bs.address;

        bs.type = (IoBoardType)board.type().read(fd);
        bs.address = (uint8_t)board.address().read(fd);

        for (uint8_t l = 0; l < XIO_LINES; ++l)
        {
            const auto line = board.lines().entry(l);
            XioLineState &ls = xio_[b][l];

            const PinMode modeBefore = bs.lines[l].mode;
            const uint64_t evBefore[] =
                { ls.evActive, ls.evInactive, ls.evOn, ls.evOff };

            bs.lines[l].mode = (PinMode)line.mode().read(fd);
            bs.lines[l].invert = line.invert().read(fd) != 0;
            bs.lines[l].debounceMs = line.debounce_ms().read(fd);

            ls.evActive = line.event_active().read(fd);
            ls.evInactive = line.event_inactive().read(fd);
            ls.evOn = line.event_on().read(fd);
            ls.evOff = line.event_off().read(fd);

            const uint64_t evAfter[] =
                { ls.evActive, ls.evInactive, ls.evOn, ls.evOff };
            if (modeBefore != bs.lines[l].mode ||
                memcmp(evBefore, evAfter, sizeof(evBefore)) != 0)
            {
                eventsChanged = true;
            }
        }

        if (typeBefore != bs.type || addressBefore != bs.address)
        {
            eventsChanged = true;
        }
    }
    ioBoards_.configure(ioSettings_);

    // Servo and light boards.
    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b)
    {
        const auto board = cfg_.seg().servo_boards().entry(b);
        ServoBoardSettings &bs = svSettings_[b];

        const ServoBoardType typeBefore = bs.type;
        const uint8_t addressBefore = bs.address;

        bs.type = (ServoBoardType)board.type().read(fd);
        bs.address = (uint8_t)board.address().read(fd);
        // 0 stops the pulses once a servo is in position; 1 keeps them up.
        bs.hold = board.hold().read(fd) != 0;

        for (uint8_t c = 0; c < SERVO_CHANNELS; ++c)
        {
            const auto chan = board.channels().entry(c);
            ServoChannelSettings &cs = bs.channels[c];

            const ServoUse useBefore = cs.use;
            const uint64_t evBefore[] =
                { cs.evThrow, cs.evClose, cs.evThrown, cs.evClosed };

            cs.use = (ServoUse)chan.use().read(fd);
            cs.timeMs = chan.time_ms().read(fd);
            cs.closedUs = chan.closed_us().read(fd);
            cs.thrownUs = chan.thrown_us().read(fd);
            cs.brightness = (uint8_t)chan.brightness().read(fd);
            cs.evThrow = chan.event_throw().read(fd);
            cs.evClose = chan.event_close().read(fd);
            cs.evThrown = chan.event_thrown().read(fd);
            cs.evClosed = chan.event_closed().read(fd);

            const uint64_t evAfter[] =
                { cs.evThrow, cs.evClose, cs.evThrown, cs.evClosed };
            if (useBefore != cs.use ||
                memcmp(evBefore, evAfter, sizeof(evBefore)) != 0)
            {
                eventsChanged = true;
            }
        }

        if (typeBefore != bs.type || addressBefore != bs.address)
        {
            eventsChanged = true;
        }
    }
    servoBoards_.configure(svSettings_);

    register_events();

    // The refresh loop drives every slot at 33 Hz. It is built here rather than
    // in the constructor because it needs the node, and it is never rebuilt:
    // the slots outlive every configuration change.
    if (!refreshLoop_)
    {
        std::vector<openlcb::Polling *> members;
        members.reserve(SLOT_COUNT);
        for (unsigned i = 0; i < SLOT_COUNT; ++i)
        {
            members.push_back(&slots_[i]);
        }
        refreshLoop_.reset(new openlcb::RefreshLoop(stack_->node(), members));
    }

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
        CDI_FACTORY_RESET(blk.detect_while_off);
        CDI_FACTORY_RESET(blk.pulse_len_100us);
        CDI_FACTORY_RESET(blk.pulse_interval_ms);
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

    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b)
    {
        const auto board = cfg_.seg().io_boards().entry(b);
        CDI_FACTORY_RESET(board.type);
        board.address().write(fd, (uint8_t)(DEFAULT_XIO_ADDRESS + b));
        for (uint8_t l = 0; l < XIO_LINES; ++l)
        {
            const auto line = board.lines().entry(l);
            const uint8_t i = (uint8_t)(b * XIO_LINES + l);
            line.description().write(fd, "");
            CDI_FACTORY_RESET(line.mode);
            CDI_FACTORY_RESET(line.invert);
            CDI_FACTORY_RESET(line.debounce_ms);
            line.event_active().write(fd, event_for(nodeId, EV_XIO_ACTIVE + i));
            line.event_inactive().write(
                fd, event_for(nodeId, EV_XIO_INACTIVE + i));
            line.event_on().write(fd, event_for(nodeId, EV_XIO_ON + i));
            line.event_off().write(fd, event_for(nodeId, EV_XIO_OFF + i));
        }
    }

    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b)
    {
        const auto board = cfg_.seg().servo_boards().entry(b);
        CDI_FACTORY_RESET(board.type);
        board.address().write(fd, (uint8_t)(DEFAULT_SVB_ADDRESS + b));
        CDI_FACTORY_RESET(board.hold);
        for (uint8_t c = 0; c < SERVO_CHANNELS; ++c)
        {
            const auto chan = board.channels().entry(c);
            const uint8_t i = (uint8_t)(b * SERVO_CHANNELS + c);
            chan.description().write(fd, "");
            CDI_FACTORY_RESET(chan.use);
            CDI_FACTORY_RESET(chan.time_ms);
            CDI_FACTORY_RESET(chan.closed_us);
            CDI_FACTORY_RESET(chan.thrown_us);
            CDI_FACTORY_RESET(chan.brightness);
            chan.event_throw().write(fd, event_for(nodeId, EV_SV_THROW + i));
            chan.event_close().write(fd, event_for(nodeId, EV_SV_CLOSE + i));
            chan.event_thrown().write(fd, event_for(nodeId, EV_SV_THROWN + i));
            chan.event_closed().write(fd, event_for(nodeId, EV_SV_CLOSED + i));
        }
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

    // The bit handlers register in their constructors and unregister in their
    // destructors, and their event IDs are fixed at construction. So they are
    // destroyed and remade too — and the old ones must go first, or their
    // destructors would unregister entries the new ones had just added. Each
    // slot is pointed at nothing until its producer exists again.
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
    {
        slots_[i].set_producer(nullptr);
    }
    bitProducers_.clear();
    bitConsumers_.clear();
    bits_.clear();

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
            // Occupancy is three-valued: a block that is not energised and
            // not being pulsed proves nothing either way. That survives the
            // move to the bit classes because EventState is three-valued too —
            // get_current_state() returns UNKNOWN, invert_event_state() leaves
            // UNKNOWN alone, and the MTI is computed as
            // MTI_PRODUCER_IDENTIFIED_VALID + state, so both the occupied and
            // the clear event answer "unknown" rather than one of them
            // claiming the block is clear.
            add_producer_bit(BitKind::Occupancy, i, SLOT_OCCUPANCY + i,
                s.evOccupied, s.evClear);
        }
        else
        {
            // Two bits rather than one: the events a turnout is driven by and
            // the ones it reports are different IDs, and one
            // BitEventInterface carries one pair.
            add_consumer_bit(BitKind::TurnoutCmd, i, s.evThrow, s.evClose);
            add_producer_bit(BitKind::TurnoutPos, i, SLOT_TURNOUT + i,
                s.evThrown, s.evClosed);
        }
    }

    // The I/O pins go through OpenMRN's bit classes rather than this class's
    // own handler: each one is a single bit with one pair of events, which is
    // exactly what BitEventInterface models, and letting BitEventProducer and
    // BitEventConsumer answer the identify queries is the point of the
    // exercise. See FemtoBits.hxx for which hardware qualifies and which does
    // not.
    for (uint8_t i = 0; i < EXP_GPIO_COUNT; ++i)
    {
        const PinState &s = pin_[i];
        switch (s.mode)
        {
            case PinMode::Input:
            case PinMode::InputPullup:
                add_producer_bit(BitKind::PinIn, i, SLOT_PIN_IN + i,
                    s.evActive, s.evInactive);
                break;
            case PinMode::Output:
                add_consumer_bit(BitKind::PinOut, i, s.evOn, s.evOff);
                break;
            default:
                break;
        }
    }

    // Expansion board lines. The index packs the board and the line:
    // 16 * board + line.
    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b)
    {
        const IoBoardSettings &bs = ioSettings_[b];
        const uint8_t lines = ioBoardLines(bs.type);
        for (uint8_t l = 0; l < lines; ++l)
        {
            const uint8_t idx = (uint8_t)(b * XIO_LINES + l);
            const XioLineState &ls = xio_[b][l];
            switch (bs.lines[l].mode)
            {
                case PinMode::Input:
                case PinMode::InputPullup:
                    add_producer_bit(BitKind::XioIn, idx, SLOT_XIO_IN + idx,
                        ls.evActive, ls.evInactive);
                    break;
                case PinMode::Output:
                    add_consumer_bit(
                        BitKind::XioOut, idx, ls.evOn, ls.evOff);
                    break;
                default:
                    break;
            }
        }
    }

    // Servo and light channels, indexed 16 * board + channel.
    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b)
    {
        const ServoBoardSettings &bs = svSettings_[b];
        if (bs.type == ServoBoardType::None)
        {
            continue;
        }
        for (uint8_t c = 0; c < SERVO_CHANNELS; ++c)
        {
            const ServoChannelSettings &cs = bs.channels[c];
            if (cs.use == ServoUse::Unused)
            {
                continue;
            }
            const uint8_t idx = (uint8_t)(b * SERVO_CHANNELS + c);
            add_consumer_bit(BitKind::ServoCmd, idx, cs.evThrow, cs.evClose);
            // Only a servo reports arriving; a light is where it was put.
            if (useIsServo(cs.use))
            {
                add_producer_bit(BitKind::ServoPos, idx,
                    SLOT_SERVO_POS + idx, cs.evThrown, cs.evClosed);
            }
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

    drive(action, index);
}

void FemtoController::drive(FemtoAction action, uint8_t index)
{
    // Consumed events arrive on the stack's executor and drive the hardware,
    // which belongs to the hardware thread.
    OSMutexLock l(&hwLock_);

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
            // The position bit reports it; the refresh loop sends whichever of
            // thrown/closed the turnout is now in.
            slots_[SLOT_TURNOUT + index].trigger();
            break;

        case FemtoAction::Close:
            turnouts_.set(index, false);
            slots_[SLOT_TURNOUT + index].trigger();
            break;

        case FemtoAction::PinOn:
            pins_.setOutput(index, true);
            break;

        case FemtoAction::PinOff:
            pins_.setOutput(index, false);
            break;

        case FemtoAction::XioOn:
            ioBoards_.setOutput(
                index / XIO_LINES, index % XIO_LINES, true);
            break;

        case FemtoAction::XioOff:
            ioBoards_.setOutput(
                index / XIO_LINES, index % XIO_LINES, false);
            break;

        case FemtoAction::SvThrow:
            servoBoards_.set(
                index / SERVO_CHANNELS, index % SERVO_CHANNELS, true);
            break;

        case FemtoAction::SvClose:
            servoBoards_.set(
                index / SERVO_CHANNELS, index % SERVO_CHANNELS, false);
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

    // Reads driver state from the stack's executor while the hardware thread
    // may be updating it.
    OSMutexLock l(&hwLock_);

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

        // A board that is not answering knows nothing about its lines.
        case FemtoAction::XioActive:
        case FemtoAction::XioInactive:
        {
            const uint8_t b = index / XIO_LINES;
            const uint8_t l = index % XIO_LINES;
            if (!ioBoards_.present(b))
            {
                return EventState::UNKNOWN;
            }
            const bool want = (action == FemtoAction::XioActive);
            return ioBoards_.bank(b).active(l) == want ? EventState::VALID
                                                       : EventState::INVALID;
        }

        case FemtoAction::XioOn:
        case FemtoAction::XioOff:
        {
            const uint8_t b = index / XIO_LINES;
            const uint8_t l = index % XIO_LINES;
            if (!ioBoards_.present(b))
            {
                return EventState::UNKNOWN;
            }
            const bool want = (action == FemtoAction::XioOn);
            return ioBoards_.bank(b).output(l) == want ? EventState::VALID
                                                       : EventState::INVALID;
        }

        case FemtoAction::SvThrow:
        case FemtoAction::SvClose:
        case FemtoAction::SvThrown:
        case FemtoAction::SvClosed:
        {
            const uint8_t b = index / SERVO_CHANNELS;
            const uint8_t c = index % SERVO_CHANNELS;
            const bool wantThrown = (action == FemtoAction::SvThrow ||
                action == FemtoAction::SvThrown);

            // A light has no travel, so it is simply what it was last told.
            if (useIsLight(svSettings_[b].channels[c].use))
            {
                bool on;
                if (!servoBoards_.commanded(b, c, on))
                {
                    return EventState::UNKNOWN;
                }
                return on == wantThrown ? EventState::VALID
                                        : EventState::INVALID;
            }
            switch (servoBoards_.position(b, c))
            {
                case TurnoutPosition::Thrown:
                    return wantThrown ? EventState::VALID
                                      : EventState::INVALID;
                case TurnoutPosition::Closed:
                    return wantThrown ? EventState::INVALID
                                      : EventState::VALID;
                default:
                    return EventState::UNKNOWN;
            }
        }

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
    // Nearly every call comes from the hardware poll, which is on its own
    // thread, and an LCC message may only be built and sent on the stack's
    // executor. The send is therefore posted there instead of made here.
    // CallbackExecutable deletes itself once it has run.
    openlcb::SimpleCanStack *stack = stack_;
    stack->executor()->add(new CallbackExecutable(
        [stack, eventId]() { stack->send_event(eventId); }));
}

// ---------------------------------------------------------------------------
// The poll
// ---------------------------------------------------------------------------

StateFlowBase::Action FemtoController::poll()
{
    if (applied_)
    {
        // Everything below reaches the drivers, so it is done under the lock
        // that the stack's executor also takes for live control, a consumed
        // event, an identify reply or a configuration load.
        OSMutexLock l(&hwLock_);

        poll_occupancy();
        poll_faults();
        poll_pins();
        poll_io_boards();
        poll_servo_boards();
        turnouts_.update();
        // Keeps the DCC refresh cycle turning, so a decoder that stops hearing
        // its address does not time out.
        dcc_.update();
        // Switches off anything a dispatcher left running if it has gone quiet.
        liveControl_.poll();

        // Saves the module description once it has stopped arriving. That write
        // can take tens of milliseconds, which is one of the two reasons this
        // poll is not on the stack's executor.
        //
        // The interlock: writing flash disables the flash cache while each
        // sector is erased and written, and the DCC timer interrupt is not
        // resident in RAM: it reaches static helpers and memcpy in
        // DCCSource.cpp, which is not annotated for IRAM. An interrupt
        // during that window is a crash. So while the source is running the
        // description is held in RAM and written when it stops — which
        // ::end() makes happen promptly. A dispatcher's write is still
        // accepted and read back at once; only the flash write waits.
        if (!dcc_.isRunning())
        {
            moduleStore_.poll();
        }
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
        // Only a definite reading is reported. Unknown means the block is not
        // energised, so nothing is sent: the state stands until it can be
        // measured again.
        if (now == BlockState::Occupied || now == BlockState::Clear)
        {
            slots_[SLOT_OCCUPANCY + i].trigger();
        }
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

void FemtoController::poll_io_boards()
{
    ioBoards_.poll();

    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b)
    {
        // A board appearing or going away changes what its lines can be said
        // to be, but re-announcing every one of them is far more traffic than
        // it is worth, so the flag is drained and noted rather than acted on.
        if (ioBoards_.takeChanged(b))
        {
            printf("I/O board %u at 0x%02X %s\n", (unsigned)(b + 1),
                ioBoards_.address(b),
                ioBoards_.present(b) ? "found" : "not answering");
        }

        for (uint8_t l = 0; l < XIO_LINES; ++l)
        {
            bool active;
            if (!ioBoards_.takeTransition(b, l, active))
            {
                continue;
            }
            // The refresh loop sends whichever of the pair matches the state
            // the line is in now; this thread only marks it as changed.
            (void)active;
            slots_[SLOT_XIO_IN + (unsigned)(b * XIO_LINES + l)].trigger();
        }
    }
}

void FemtoController::poll_servo_boards()
{
    servoBoards_.poll();

    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b)
    {
        if (servoBoards_.takeChanged(b))
        {
            printf("servo board %u at 0x%02X %s\n", (unsigned)(b + 1),
                servoBoards_.address(b),
                servoBoards_.present(b) ? "found" : "not answering");
        }

        for (uint8_t c = 0; c < SERVO_CHANNELS; ++c)
        {
            bool thrown;
            if (!servoBoards_.takeArrival(b, c, thrown))
            {
                continue;
            }
            (void)thrown;
            slots_[SLOT_SERVO_POS + (unsigned)(b * SERVO_CHANNELS + c)]
                .trigger();
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
        // The hardware thread never sends: it marks the bit as changed and the
        // refresh loop, on the stack's executor, sends whichever of the pair
        // matches the state the pin is now in.
        (void)active;
        slots_[SLOT_PIN_IN + i].trigger();
    }
}

// ---------------------------------------------------------------------------
// The bit API, for FemtoBits
// ---------------------------------------------------------------------------

openlcb::Node *FemtoController::node()
{
    return stack_->node();
}

/// The action whose "true" meaning matches a bit being on. get_current_state()
/// asks state_of() about this one, so VALID means the bit is on.
static FemtoAction action_for(BitKind kind, bool on)
{
    switch (kind)
    {
        case BitKind::Occupancy:
            return on ? FemtoAction::Occupied : FemtoAction::Clear;
        case BitKind::TurnoutCmd:
            return on ? FemtoAction::Throw : FemtoAction::Close;
        case BitKind::TurnoutPos:
            return on ? FemtoAction::Thrown : FemtoAction::Closed;
        case BitKind::PinIn:
            return on ? FemtoAction::PinActive : FemtoAction::PinInactive;
        case BitKind::PinOut:
            return on ? FemtoAction::PinOn : FemtoAction::PinOff;
        case BitKind::XioIn:
            return on ? FemtoAction::XioActive : FemtoAction::XioInactive;
        case BitKind::XioOut:
            return on ? FemtoAction::XioOn : FemtoAction::XioOff;
        case BitKind::ServoCmd:
            return on ? FemtoAction::SvThrow : FemtoAction::SvClose;
        case BitKind::ServoPos:
        default:
            return on ? FemtoAction::SvThrown : FemtoAction::SvClosed;
    }
}

openlcb::EventState FemtoController::bit_state(BitKind kind, uint8_t index)
{
    // state_of() takes the hardware lock itself.
    return state_of(action_for(kind, true), index);
}

void FemtoController::bit_set(BitKind kind, uint8_t index, bool value)
{
    switch (kind)
    {
        case BitKind::PinOut:
        case BitKind::TurnoutCmd:
        case BitKind::XioOut:
        case BitKind::ServoCmd:
            drive(action_for(kind, value), index);
            break;

        // The rest are inputs: the bus does not get to set them. OpenMRN calls
        // set_state() only on a consumer, so this should not be reached, but a
        // producer-only bit answering it quietly is better than one that drives
        // something.
        default:
            break;
    }
}

void FemtoController::add_producer_bit(
    BitKind kind, uint8_t index, unsigned slot, uint64_t on, uint64_t off)
{
    if (on == 0 || off == 0 || slot >= SLOT_COUNT)
    {
        return;     // an event of all zeros does nothing
    }
    bits_.emplace_back(new FemtoBit(this, kind, index, on, off));
    bitProducers_.emplace_back(
        new openlcb::BitEventProducer(bits_.back().get()));
    slots_[slot].set_producer(bitProducers_.back().get());
}

void FemtoController::add_consumer_bit(
    BitKind kind, uint8_t index, uint64_t on, uint64_t off)
{
    if (on == 0 || off == 0)
    {
        return;
    }
    bits_.emplace_back(new FemtoBit(this, kind, index, on, off));
    bitConsumers_.emplace_back(
        new openlcb::BitEventConsumer(bits_.back().get()));
}
