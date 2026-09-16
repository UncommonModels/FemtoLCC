#include "Controller.h"
#include <Arduino.h>
#include "Notice.h"

using AOLCB::EventState;

// Occupancy samples one channel per pass, so each channel is seen every 40 ms.
static const uint32_t OCCUPANCY_POLL_MS = 10;

static EventState truth(bool valid) {
    return valid ? EventState::Valid : EventState::Invalid;
}

static EventState blockTruth(BlockState now, BlockState want) {
    return now == BlockState::Unknown ? EventState::Unknown : truth(now == want);
}

static EventState turnoutTruth(TurnoutPosition now, TurnoutPosition want) {
    return now == TurnoutPosition::Unknown ? EventState::Unknown : truth(now == want);
}

static const char* blockText(BlockState s) {
    return s == BlockState::Occupied ? "OCCUPIED"
         : s == BlockState::Clear    ? "clear"
                                     : "-";
}

Controller::Controller(AOLCB::Node& node, Channels& channels, Occupancy& occupancy,
                       Turnouts& turnouts, IoPins& pins, IoBoards& ioBoards, ServoBoards& servos)
    : node_(node), channels_(channels), occupancy_(occupancy), turnouts_(turnouts),
      pins_(pins), ioBoards_(ioBoards), servos_(servos), settings_{}, applied_(false),
      bindingCount_(0), producerCount_(0), lastOccupancyPoll_(0), emitting_(false) {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        faultLatched_[i] = false;
    }
}

ChannelRole Controller::role(uint8_t channel) const {
    return channel < NUM_CHANNELS ? settings_.channels[channel].role : ChannelRole::Unused;
}

// --- applying settings -------------------------------------------------------

void Controller::apply(const Settings& settings, bool atBoot) {
    bool roleChanged[NUM_CHANNELS];
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        roleChanged[ch] = atBoot || !applied_ ||
                          settings.channels[ch].role != settings_.channels[ch].role;
    }
    settings_ = settings;
    applied_ = true;

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        applyChannel(ch, roleChanged[ch]);
    }
    channels_.flush();
    pins_.configure(settings_.pins);
    ioBoards_.configure(settings_.ioBoards);
    servos_.configure(settings_.servoBoards);

    registerEvents();
    if (!atBoot) {
        node_.identifyEvents();
    }
}

void Controller::applyChannel(uint8_t ch, bool roleChanged) {
    const ChannelSettings& c = settings_.channels[ch];
    occupancy_.setThresholds(ch, c.occupiedMa, c.clearMa);
    occupancy_.setEnabled(ch, c.role == ChannelRole::Block);
    occupancy_.setPulseDetect(ch, c.detectWhileOff, c.detectPulseUs, c.detectIntervalMs);

    switch (c.role) {
    case ChannelRole::Block:
        turnouts_.detach(ch);
        if (roleChanged) {
            switch (c.powerOn) {
            case PowerOnMode::DC:  channels_.set(ch, ChannelMode::DC, false, 255);          break;
            case PowerOnMode::DCC: channels_.set(ch, ChannelMode::DCC, c.dccReversed, 0);   break;
            default:               channels_.set(ch, ChannelMode::Off, false, 0);          break;
            }
        } else if (channels_.mode(ch) == ChannelMode::DCC) {
            channels_.setReverse(ch, c.dccReversed);    // a polarity change applies now
        }
        break;

    case ChannelRole::Turnout:
        turnouts_.attach(ch, c);
        if (roleChanged) {
            if (c.turnoutPowerOn == TurnoutPowerOn::Leave) {
                channels_.set(ch, ChannelMode::Off, false, 0);
            } else {
                turnouts_.set(ch, c.turnoutPowerOn == TurnoutPowerOn::Thrown);
            }
        }
        break;

    default:
        turnouts_.detach(ch);
        channels_.set(ch, ChannelMode::Off, false, 0);
        break;
    }
}

// --- event registration ------------------------------------------------------

// An event ID of zero means "none": a user can blank a field in the
// configuration tool to stop the node sending or acting on it.

void Controller::consume(uint64_t eventId, Action action, uint8_t index, EventState state) {
    if (eventId == 0 || bindingCount_ >= MAX_BINDINGS) {
        return;
    }
    // One event may drive several things - an "all off" shared by four blocks,
    // say - but the node is told about it once.
    bool known = false;
    for (uint16_t i = 0; i < bindingCount_; ++i) {
        known = known || bindings_[i].eventId == eventId;
    }
    bindings_[bindingCount_++] = Binding{ eventId, action, index };
    if (!known) {
        node_.addConsumer(eventId, state);
    }
}

void Controller::produce(uint64_t eventId, EventState state) {
    if (eventId == 0 || producerCount_ >= MAX_PRODUCERS) {
        return;
    }
    for (uint16_t i = 0; i < producerCount_; ++i) {
        if (producers_[i] == eventId) {
            return;
        }
    }
    producers_[producerCount_++] = eventId;
    node_.addProducer(eventId, state);
}

void Controller::setConsumerState(uint64_t eventId, EventState state) {
    if (eventId != 0) {
        node_.setConsumerState(eventId, state);
    }
}

void Controller::setProducerState(uint64_t eventId, EventState state) {
    if (eventId != 0) {
        node_.setProducerState(eventId, state);
    }
}

// Send an event, and act on it here as well if this node consumes it too: the
// bus never echoes a node's own messages back, and a button on P0 wired to a
// turnout on output A is a natural thing to configure. One level only, so two
// settings that feed each other cannot loop.
void Controller::emit(uint64_t eventId) {
    if (eventId == 0) {
        return;
    }
    node_.produceEvent(eventId);
    if (emitting_) {
        return;
    }
    emitting_ = true;
    handleEvent(eventId);
    emitting_ = false;
}

void Controller::registerEvents() {
    node_.clearEvents();
    bindingCount_ = 0;
    producerCount_ = 0;

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        const ChannelSettings& c = settings_.channels[ch];
        if (c.role == ChannelRole::Unused) {
            continue;
        }
        produce(c.evFault, truth(channels_.faulted(ch)));

        if (c.role == ChannelRole::Block) {
            const ChannelMode mode = channels_.mode(ch);
            const BlockState block = occupancy_.state(ch);
            consume(c.evOn,  Action::BlockOn,  ch, truth(mode == ChannelMode::DC));
            consume(c.evOff, Action::BlockOff, ch, truth(mode == ChannelMode::Off));
            consume(c.evDcc, Action::BlockDcc, ch, truth(mode == ChannelMode::DCC));
            produce(c.evOccupied, blockTruth(block, BlockState::Occupied));
            produce(c.evClear,    blockTruth(block, BlockState::Clear));
        } else {
            const TurnoutPosition pos = turnouts_.position(ch);
            consume(c.evThrow, Action::Throw, ch, turnoutTruth(pos, TurnoutPosition::Thrown));
            consume(c.evClose, Action::Close, ch, turnoutTruth(pos, TurnoutPosition::Closed));
            produce(c.evThrown, turnoutTruth(pos, TurnoutPosition::Thrown));
            produce(c.evClosed, turnoutTruth(pos, TurnoutPosition::Closed));
        }
    }

    for (uint8_t p = 0; p < EXP_GPIO_COUNT; ++p) {
        const PinSettings& pin = settings_.pins[p];
        if (pin.mode == PinMode::Output) {
            consume(pin.evOn,  Action::PinOn,  p, truth(pins_.output(p)));
            consume(pin.evOff, Action::PinOff, p, truth(!pins_.output(p)));
        } else if (pins_.isInput(p)) {
            produce(pin.evActive,   truth(pins_.active(p)));
            produce(pin.evInactive, truth(!pins_.active(p)));
        }
    }

    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b) {
        if (!ioBoards_.fitted(b)) {
            continue;
        }
        const PinBank& bank = ioBoards_.bank(b);
        for (uint8_t l = 0; l < bank.count(); ++l) {
            const PinSettings& line = settings_.ioBoards[b].lines[l];
            const uint8_t index = b * XIO_LINES + l;
            if (bank.mode(l) == PinMode::Output) {
                consume(line.evOn,  Action::LineOn,  index, truth(bank.output(l)));
                consume(line.evOff, Action::LineOff, index, truth(!bank.output(l)));
            } else if (bank.isInput(l)) {
                produce(line.evActive,   lineTruth(b, l, true));
                produce(line.evInactive, lineTruth(b, l, false));
            }
        }
    }

    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b) {
        if (!servos_.fitted(b)) {
            continue;
        }
        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            const ServoChannelSettings& c = settings_.servoBoards[b].channels[ch];
            if (c.use == ServoUse::Unused) {
                continue;
            }
            const uint8_t index = b * SERVO_CHANNELS + ch;
            consume(c.evThrow, Action::ServoThrow, index, commandTruth(b, ch, true));
            consume(c.evClose, Action::ServoClose, index, commandTruth(b, ch, false));
            if (useIsServo(c.use)) {
                const TurnoutPosition pos = servos_.position(b, ch);
                produce(c.evThrown, turnoutTruth(pos, TurnoutPosition::Thrown));
                produce(c.evClosed, turnoutTruth(pos, TurnoutPosition::Closed));
            }
        }
    }
}

// --- event states --------------------------------------------------------------

void Controller::updateBlockStates(uint8_t ch) {
    const ChannelSettings& c = settings_.channels[ch];
    if (c.role != ChannelRole::Block) {
        return;
    }
    const ChannelMode mode = channels_.mode(ch);
    setConsumerState(c.evOn,  truth(mode == ChannelMode::DC));
    setConsumerState(c.evOff, truth(mode == ChannelMode::Off));
    setConsumerState(c.evDcc, truth(mode == ChannelMode::DCC));
}

void Controller::updateTurnoutStates(uint8_t ch) {
    const ChannelSettings& c = settings_.channels[ch];
    const TurnoutPosition pos = turnouts_.position(ch);
    setConsumerState(c.evThrow,  turnoutTruth(pos, TurnoutPosition::Thrown));
    setConsumerState(c.evClose,  turnoutTruth(pos, TurnoutPosition::Closed));
    setProducerState(c.evThrown, turnoutTruth(pos, TurnoutPosition::Thrown));
    setProducerState(c.evClosed, turnoutTruth(pos, TurnoutPosition::Closed));
}

void Controller::updatePinStates(uint8_t pin) {
    const PinSettings& p = settings_.pins[pin];
    setConsumerState(p.evOn,  truth(pins_.output(pin)));
    setConsumerState(p.evOff, truth(!pins_.output(pin)));
}

void Controller::updateLineStates(uint8_t board, uint8_t line) {
    const PinSettings& p = settings_.ioBoards[board].lines[line];
    const bool on = ioBoards_.bank(board).output(line);
    setConsumerState(p.evOn,  truth(on));
    setConsumerState(p.evOff, truth(!on));
}

void Controller::updateInputStates(uint8_t board) {
    const PinBank& bank = ioBoards_.bank(board);
    for (uint8_t l = 0; l < bank.count(); ++l) {
        if (bank.isInput(l)) {
            const PinSettings& p = settings_.ioBoards[board].lines[l];
            setProducerState(p.evActive,   lineTruth(board, l, true));
            setProducerState(p.evInactive, lineTruth(board, l, false));
        }
    }
}

void Controller::updateServoStates(uint8_t board, uint8_t channel) {
    const ServoChannelSettings& c = settings_.servoBoards[board].channels[channel];
    if (!servos_.fitted(board) || c.use == ServoUse::Unused) {
        return;
    }
    setConsumerState(c.evThrow, commandTruth(board, channel, true));
    setConsumerState(c.evClose, commandTruth(board, channel, false));
    if (useIsServo(c.use)) {
        const TurnoutPosition pos = servos_.position(board, channel);
        setProducerState(c.evThrown, turnoutTruth(pos, TurnoutPosition::Thrown));
        setProducerState(c.evClosed, turnoutTruth(pos, TurnoutPosition::Closed));
    }
}

// Nobody can vouch for the inputs of a board that is not answering.
EventState Controller::lineTruth(uint8_t board, uint8_t line, bool active) const {
    if (!ioBoards_.present(board)) {
        return EventState::Unknown;
    }
    return truth(ioBoards_.bank(board).active(line) == active);
}

EventState Controller::commandTruth(uint8_t board, uint8_t channel, bool thrown) const {
    bool now;
    return servos_.commanded(board, channel, now) ? truth(now == thrown) : EventState::Unknown;
}

void Controller::channelChanged(uint8_t channel) {
    if (channel < NUM_CHANNELS) {
        updateBlockStates(channel);
    }
}

void Controller::servoChanged(uint8_t board, uint8_t channel) {
    if (board < NUM_SERVO_BOARDS && channel < SERVO_CHANNELS) {
        updateServoStates(board, channel);
    }
}

// --- consumed events -----------------------------------------------------------

bool Controller::setTurnout(uint8_t channel, bool thrown) {
    if (!turnouts_.attached(channel)) {
        return false;
    }
    const ChannelSettings& c = settings_.channels[channel];
    turnouts_.set(channel, thrown);
    updateTurnoutStates(channel);
    // No position feedback, so report the move as done once it is commanded.
    emit(thrown ? c.evThrown : c.evClosed);
    return true;
}

bool Controller::setLine(uint8_t board, uint8_t line, bool on) {
    if (!ioBoards_.setOutput(board, line, on)) {
        return false;
    }
    updateLineStates(board, line);
    return true;
}

// A servo reports thrown or closed when it gets there, from pollServos().
bool Controller::setServo(uint8_t board, uint8_t channel, bool thrown) {
    if (!servos_.set(board, channel, thrown)) {
        return false;
    }
    updateServoStates(board, channel);
    return true;
}

void Controller::handleEvent(uint64_t eventId) {
    for (uint16_t i = 0; i < bindingCount_; ++i) {
        const Binding& b = bindings_[i];
        if (b.eventId != eventId) {
            continue;
        }
        switch (b.action) {
        case Action::BlockOn:
            channels_.set(b.index, ChannelMode::DC, false, 255);
            break;
        case Action::BlockOff:
            channels_.set(b.index, ChannelMode::Off, false, 0);
            break;
        case Action::BlockDcc:
            channels_.set(b.index, ChannelMode::DCC,
                          settings_.channels[b.index].dccReversed, 0);
            break;
        case Action::Throw:
        case Action::Close:
            if (setTurnout(b.index, b.action == Action::Throw)) {
                notice("LCC event -> turnout %c %s\n", 'A' + b.index,
                       b.action == Action::Throw ? "thrown" : "closed");
            }
            continue;
        case Action::PinOn:
        case Action::PinOff:
            pins_.setOutput(b.index, b.action == Action::PinOn);
            updatePinStates(b.index);
            notice("LCC event -> P%u %s\n", b.index, b.action == Action::PinOn ? "on" : "off");
            continue;
        case Action::LineOn:
        case Action::LineOff: {
            const uint8_t board = b.index / XIO_LINES;
            const uint8_t line = b.index % XIO_LINES;
            setLine(board, line, b.action == Action::LineOn);
            notice("LCC event -> X%u.%u %s\n", board + 1, line + 1,
                   b.action == Action::LineOn ? "on" : "off");
            continue;
        }
        case Action::ServoThrow:
        case Action::ServoClose: {
            const uint8_t board = b.index / SERVO_CHANNELS;
            const uint8_t ch = b.index % SERVO_CHANNELS;
            const bool thrown = b.action == Action::ServoThrow;
            if (setServo(board, ch, thrown)) {
                const bool light = useIsLight(settings_.servoBoards[board].channels[ch].use);
                notice("LCC event -> S%u.%u %s\n", board + 1, ch + 1,
                       light ? (thrown ? "on" : "off") : (thrown ? "throw" : "close"));
            }
            continue;
        }
        }
        channels_.flush();
        updateBlockStates(b.index);
        notice("LCC event -> channel %c\n", 'A' + b.index);
    }
}

// --- produced events -------------------------------------------------------------

void Controller::poll() {
    turnouts_.update();
    pollOccupancy();
    pollFaults();
    pollPins();
    pollIoBoards();
    pollServos();
}

// Sample one channel per pass and report any change to the bus.
void Controller::pollOccupancy() {
    if (millis() - lastOccupancyPoll_ < OCCUPANCY_POLL_MS) {
        return;
    }
    lastOccupancyPoll_ = millis();
    occupancy_.poll();

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        BlockState now;
        if (!occupancy_.takeTransition(i, now) || settings_.channels[i].role != ChannelRole::Block) {
            continue;
        }
        const ChannelSettings& c = settings_.channels[i];
        setProducerState(c.evOccupied, blockTruth(now, BlockState::Occupied));
        setProducerState(c.evClear,    blockTruth(now, BlockState::Clear));
        if (now == BlockState::Occupied) {
            emit(c.evOccupied);
        } else if (now == BlockState::Clear) {
            emit(c.evClear);
        }
        notice("block %c %s\n", 'A' + i, blockText(now));
    }
}

// A DRV8874 pulls nFAULT low on overcurrent, overtemperature or undervoltage.
// Shut the offending channel down rather than letting it retry into a short.
void Controller::pollFaults() {
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        const bool faulted = channels_.faulted(i);
        const ChannelSettings& c = settings_.channels[i];
        if (faulted && !faultLatched_[i]) {
            channels_.set(i, ChannelMode::Off, false, 0);
            channels_.flush();
            turnouts_.release(i);
            updateBlockStates(i);
            if (c.role != ChannelRole::Unused) {
                setProducerState(c.evFault, EventState::Valid);
                emit(c.evFault);                        // tell the layout
            }
            notice("channel %c FAULT - shut down\n", 'A' + i);
        } else if (!faulted && faultLatched_[i] && c.role != ChannelRole::Unused) {
            setProducerState(c.evFault, EventState::Invalid);
        }
        faultLatched_[i] = faulted;
    }
}

void Controller::pollPins() {
    pins_.poll();
    for (uint8_t p = 0; p < EXP_GPIO_COUNT; ++p) {
        bool active;
        if (!pins_.takeTransition(p, active)) {
            continue;
        }
        const PinSettings& pin = settings_.pins[p];
        setProducerState(pin.evActive,   truth(active));
        setProducerState(pin.evInactive, truth(!active));
        notice("P%u %s\n", p, active ? "active" : "inactive");
        emit(active ? pin.evActive : pin.evInactive);
    }
}

// Inputs on the expansion boards, as for P0-P7. A board found or lost makes
// its inputs' states known or unknown.
void Controller::pollIoBoards() {
    ioBoards_.poll();
    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b) {
        if (!ioBoards_.fitted(b)) {
            continue;
        }
        if (ioBoards_.takeChanged(b)) {
            updateInputStates(b);
        }
        for (uint8_t l = 0; l < ioBoards_.bank(b).count(); ++l) {
            bool active;
            if (!ioBoards_.takeTransition(b, l, active)) {
                continue;
            }
            const PinSettings& line = settings_.ioBoards[b].lines[l];
            setProducerState(line.evActive,   truth(active));
            setProducerState(line.evInactive, truth(!active));
            notice("X%u.%u %s\n", b + 1, l + 1, active ? "active" : "inactive");
            emit(active ? line.evActive : line.evInactive);
        }
    }
}

// Servo moves, and the thrown or closed event once each one ends.
void Controller::pollServos() {
    servos_.poll();
    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b) {
        if (!servos_.fitted(b)) {
            continue;
        }
        const bool changed = servos_.takeChanged(b);
        for (uint8_t ch = 0; ch < SERVO_CHANNELS; ++ch) {
            bool thrown;
            const bool arrived = servos_.takeArrival(b, ch, thrown);
            if (changed || arrived) {
                updateServoStates(b, ch);
            }
            if (!arrived) {
                continue;
            }
            const ServoChannelSettings& c = settings_.servoBoards[b].channels[ch];
            notice("S%u.%u %s\n", b + 1, ch + 1, thrown ? "thrown" : "closed");
            emit(thrown ? c.evThrown : c.evClosed);
        }
    }
}
