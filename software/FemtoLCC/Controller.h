// Ties the configuration to the hardware and to the node's events.
//
// The configuration says what each output and pin is for, and which event IDs
// belong to it. This class applies that: it sets each output up as a block, a
// turnout or nothing, configures the expander pins and the expansion boards,
// registers the configured events with the node together with their current
// state, and maps every consumed event to the action it stands for. Going the
// other way it watches occupancy, driver faults, inputs and servo moves, and
// produces their events.
//
// A configuration tool can change any of this at run time; apply() is called
// again and the node re-announces its events.

#pragma once

#include <stdint.h>
#include <AOLCB.h>
#include "board.h"
#include "Config.h"
#include "Channels.h"
#include "Occupancy.h"
#include "Turnouts.h"
#include "IoPins.h"
#include "IoBoards.h"
#include "ServoBoards.h"

class Controller {
public:
    Controller(AOLCB::Node& node, Channels& channels, Occupancy& occupancy,
               Turnouts& turnouts, IoPins& pins, IoBoards& ioBoards, ServoBoards& servos);

    // Bring hardware and event registrations in line with the settings. At
    // boot every output takes its power-on state. Later, only an output whose
    // use changed does; the others keep running with their new settings, and
    // the node re-announces its events.
    void apply(const Settings& settings, bool atBoot);

    // An Event Report for one of the node's consumed events.
    void handleEvent(uint64_t eventId);

    // Occupancy, driver faults, inputs and turnout pulses. Call every loop.
    void poll();

    // Throw or close a turnout, as if its event had arrived.
    bool setTurnout(uint8_t channel, bool thrown);

    // Switch an output line on an I/O expansion board, as if its event had
    // arrived. False if the line is not an output.
    bool setLine(uint8_t board, uint8_t line, bool on);

    // Throw or close a servo turnout, or switch a light, as if its event had
    // arrived. False if the channel is neither.
    bool setServo(uint8_t board, uint8_t channel, bool thrown);

    // The console drove a channel, or a servo channel, directly; update the
    // states the node reports for it.
    void channelChanged(uint8_t channel);
    void servoChanged(uint8_t board, uint8_t channel);

    ChannelRole role(uint8_t channel) const;

private:
    enum class Action : uint8_t {
        BlockOn, BlockOff, BlockDcc, Throw, Close, PinOn, PinOff,
        LineOn, LineOff, ServoThrow, ServoClose
    };

    struct Binding {
        uint64_t eventId;
        Action action;
        uint8_t index;      // channel, pin, or 16 * board + line or channel
    };

    static const uint16_t MAX_BINDINGS  = NUM_CHANNELS * 3 + EXP_GPIO_COUNT * 2 +
                                          NUM_XIO_BOARDS * XIO_LINES * 2 +
                                          NUM_SERVO_BOARDS * SERVO_CHANNELS * 2;
    static const uint16_t MAX_PRODUCERS = NUM_CHANNELS * 3 + EXP_GPIO_COUNT * 2 +
                                          NUM_XIO_BOARDS * XIO_LINES * 2 +
                                          NUM_SERVO_BOARDS * SERVO_CHANNELS * 2;

    void applyChannel(uint8_t ch, bool roleChanged);
    void registerEvents();
    void consume(uint64_t eventId, Action action, uint8_t index, AOLCB::EventState state);
    void produce(uint64_t eventId, AOLCB::EventState state);
    void setConsumerState(uint64_t eventId, AOLCB::EventState state);
    void setProducerState(uint64_t eventId, AOLCB::EventState state);
    void emit(uint64_t eventId);

    void updateBlockStates(uint8_t ch);
    void updateTurnoutStates(uint8_t ch);
    void updatePinStates(uint8_t pin);
    void updateLineStates(uint8_t board, uint8_t line);
    void updateInputStates(uint8_t board);
    void updateServoStates(uint8_t board, uint8_t channel);
    AOLCB::EventState lineTruth(uint8_t board, uint8_t line, bool active) const;
    AOLCB::EventState commandTruth(uint8_t board, uint8_t channel, bool thrown) const;

    void pollOccupancy();
    void pollFaults();
    void pollPins();
    void pollIoBoards();
    void pollServos();

    AOLCB::Node& node_;
    Channels& channels_;
    Occupancy& occupancy_;
    Turnouts& turnouts_;
    IoPins& pins_;
    IoBoards& ioBoards_;
    ServoBoards& servos_;

    Settings settings_;         // as last applied
    bool applied_;

    Binding bindings_[MAX_BINDINGS];
    uint16_t bindingCount_;
    uint64_t producers_[MAX_PRODUCERS];
    uint16_t producerCount_;

    bool faultLatched_[NUM_CHANNELS];
    uint32_t lastOccupancyPoll_;
    bool emitting_;             // acting on one of our own events
};
