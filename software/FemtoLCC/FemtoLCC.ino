// FemtoLCC — ESP32-C6 LCC node with four block driver channels.
//
//   Uncommon Models — https://uncommonmodels.com
//
// What works today:
//   * all four block driver channels, DC and DCC pass-through modes
//   * per-channel current sensing and fault detection
//   * the MCP23018 expander
//   * CAN bring-up on the LCC bus through the MCP2562
//   * a serial console, so the board can drive track right now
//
// What does not work yet: the OpenLCB protocol itself. See software/README.md
// under "LCC status" before wiring this to a layout bus.

#include <Arduino.h>
#include <Wire.h>

#include "board.h"
#include "Expander.h"
#include "Channels.h"
#include "Occupancy.h"
#include "DCCSource.h"
#include "Throttle.h"
#include "LccTraction.h"
#include <AOLCB.h>

// A node ID must come from a range you own. The 02.01.57.xx.xx.xx block below is
// a placeholder - replace it before putting this on a shared bus, because two
// nodes answering to one ID will break alias allocation for both.
static const uint64_t NODE_ID = 0x020157000001ULL;

static Expander expander(MCP23018_ADDR);
static Channels channels(expander);
static AOLCB::ESP32CANInterface can(PIN_CAN_TX, PIN_CAN_RX, LCC_BITRATE);
static AOLCB::Node node(NODE_ID);
static Occupancy occupancy(channels);
static DCCSource dccSource(channels);
static Throttle throttle(dccSource, channels);
static LccTraction traction(dccSource);

// Event IDs are the node ID in the top six bytes and a suffix in the low two,
// which is the usual convention and guarantees they are unique to this board.
static inline uint64_t eventFor(uint16_t suffix) {
    return (NODE_ID << 16) | suffix;
}

// Two consumed events per channel - a throttle or panel sends these to switch a
// block - and one produced event per channel to report a driver fault.
static inline uint64_t evChannelOn(uint8_t ch)    { return eventFor(0x0100 + ch); }
static inline uint64_t evChannelOff(uint8_t ch)   { return eventFor(0x0110 + ch); }
static inline uint64_t evChannelDCC(uint8_t ch)   { return eventFor(0x0120 + ch); }
static inline uint64_t evChannelFault(uint8_t ch) { return eventFor(0x0200 + ch); }

// Occupancy is what a signalling panel or dispatcher actually subscribes to.
static inline uint64_t evBlockOccupied(uint8_t ch) { return eventFor(0x0300 + ch); }
static inline uint64_t evBlockClear(uint8_t ch)    { return eventFor(0x0310 + ch); }

static bool faultLatched[NUM_CHANNELS] = { false, false, false, false };

// An event this node consumes has been reported on the bus.
static void handleLccEvent(uint64_t eventId) {
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        if (eventId == evChannelOn(i)) {
            channels.set(i, ChannelMode::DC, false, 255);
        } else if (eventId == evChannelOff(i)) {
            channels.set(i, ChannelMode::Off, false, 0);
        } else if (eventId == evChannelDCC(i)) {
            channels.set(i, ChannelMode::DCC, false, 0);
        } else {
            continue;
        }
        channels.flush();
        Serial.printf("LCC event -> channel %c\n", 'A' + i);
        return;
    }
}

static const char* blockText(BlockState s) {
    return s == BlockState::Occupied ? "OCCUPIED"
         : s == BlockState::Clear    ? "clear"
                                     : "-";
}

static void printStatus() {
    Serial.println(F("ch  mode  dir  duty    mA  block     fault"));
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        const char* m = channels.mode(i) == ChannelMode::Off ? "off"
                      : channels.mode(i) == ChannelMode::DC  ? "dc "
                                                             : "dcc";
        Serial.printf(" %c  %s   %s  %4u  %5lu  %-8s  %s\n",
                      'A' + i, m,
                      channels.reversed(i) ? "rev" : "fwd",
                      channels.duty(i),
                      (unsigned long)occupancy.milliamps(i),
                      blockText(occupancy.state(i)),
                      channels.faulted(i) ? "FAULT" : "-");
    }
}

// Console grammar, one command per line:
//   a dc 128        channel A, DC drive, duty 128 forward
//   a dc -128       same, reverse
//   b dcc           channel B passes the track signal through
//   c dcc -         same, reversed polarity (reversing section)
//   d off           channel D idle
//   stop            every channel off
//   status          print the table above
static void handleCommand(char* line) {
    for (char* p = line; *p; ++p) {
        *p = tolower(*p);
    }

    if (strcmp(line, "stop") == 0) {
        channels.allOff();
        channels.flush();
        Serial.println(F("all channels off"));
        return;
    }
    if (strcmp(line, "status") == 0) {
        printStatus();
        return;
    }
    if (strncmp(line, "detect", 6) == 0) {
        char* on = strtok(line + 6, " \t");
        char* off = strtok(nullptr, " \t");
        if (on && off) {
            occupancy.setThresholds((uint32_t)atol(on), (uint32_t)atol(off));
            Serial.printf("thresholds: occupied >= %s mA, clear < %s mA\n", on, off);
        } else {
            Serial.println(F("usage: detect <occupied_mA> <clear_mA>"));
        }
        return;
    }
    if (strncmp(line, "source", 6) == 0) {
        char* arg = strtok(line + 6, " \t");
        if (arg && strcmp(arg, "on") == 0) {
            if (dccSource.begin()) {
                Serial.println(F("channel A is now a DCC source"));
            } else {
                Serial.println(F("could not start the DCC timer"));
            }
        } else if (arg && strcmp(arg, "off") == 0) {
            dccSource.end();
            Serial.println(F("channel A released, DCC source stopped"));
        } else {
            Serial.printf("DCC source %s, %u loco(s)\n",
                          dccSource.isRunning() ? "running" : "stopped",
                          dccSource.locoCount());
        }
        return;
    }
    if (strncmp(line, "traction", 8) == 0) {
        char* arg = strtok(line + 8, " \t");
        if (arg && strcmp(arg, "on") == 0) {
            traction.setEnabled(true);
        } else if (arg && strcmp(arg, "off") == 0) {
            traction.setEnabled(false);
        }
        Serial.printf("LCC traction translation %s, %u train node(s) seen\n",
                      traction.isEnabled() ? "on" : "off", traction.knownTrains());
        return;
    }
    if (strcmp(line, "locos") == 0) {
        uint16_t addr; int16_t sp; bool fwd;
        for (uint8_t i = 0; dccSource.locoAt(i, addr, sp, fwd); ++i) {
            Serial.printf("  %u  speed %d  %s\n", addr, sp, fwd ? "fwd" : "rev");
        }
        return;
    }
    if (strcmp(line, "calibrate") == 0) {
        channels.allOff();
        channels.flush();
        occupancy.calibrate();
        Serial.println(F("baseline recalibrated with all channels off"));
        return;
    }

    char ch = line[0];
    if (ch < 'a' || ch > 'd') {
        Serial.println(F("? try: a dc 128 | b dcc | c dcc - | d off | stop | status"));
        return;
    }
    const uint8_t index = ch - 'a';

    char* mode = strtok(line + 1, " \t");
    char* arg = strtok(nullptr, " \t");
    if (!mode) {
        Serial.println(F("? missing mode"));
        return;
    }

    if (strcmp(mode, "off") == 0) {
        channels.set(index, ChannelMode::Off, false, 0);
    } else if (strcmp(mode, "dc") == 0) {
        long duty = arg ? atol(arg) : 0;
        const bool reverse = duty < 0;
        if (duty < 0) duty = -duty;
        if (duty > 255) duty = 255;
        channels.set(index, ChannelMode::DC, reverse, (uint8_t)duty);
    } else if (strcmp(mode, "dcc") == 0) {
        const bool reverse = arg && arg[0] == '-';
        channels.set(index, ChannelMode::DCC, reverse, 0);
    } else {
        Serial.println(F("? mode must be off, dc or dcc"));
        return;
    }

    channels.flush();
    Serial.printf("channel %c set\n", ch);
}

// Sample one channel per pass and report any change to the bus.
static void pollOccupancy() {
    static uint32_t last = 0;
    if (millis() - last < 10) {
        return;
    }
    last = millis();
    occupancy.poll();

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        BlockState now;
        if (!occupancy.takeTransition(i, now)) {
            continue;
        }
        if (now == BlockState::Occupied) {
            node.produceEvent(evBlockOccupied(i));
        } else if (now == BlockState::Clear) {
            node.produceEvent(evBlockClear(i));
        }
        Serial.printf("block %c %s\n", 'A' + i, blockText(now));
    }
}

static void pollConsole() {
    static char buf[96];
    static size_t len = 0;

    while (Serial.available()) {
        const char c = Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            buf[len] = '\0';
            if (len > 0 && !throttle.handleLine(buf)) {
                handleCommand(buf);
            }
            len = 0;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// A DRV8874 pulls nFAULT low on overcurrent, overtemperature or undervoltage.
// Shut the offending channel down rather than letting it retry into a short.
static void pollFaults() {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        const bool faulted = channels.faulted(i);
        if (faulted && !faultLatched[i]) {
            channels.set(i, ChannelMode::Off, false, 0);
            channels.flush();
            node.produceEvent(evChannelFault(i));   // tell the layout
            Serial.printf("channel %c FAULT - shut down\n", 'A' + i);
        }
        faultLatched[i] = faulted;
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\nFemtoLCC — Uncommon Models"));

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    pinMode(PIN_I2C_INT, INPUT_PULLUP);

    if (!expander.begin()) {
        Serial.println(F("MCP23018 not responding — check I2C and U3"));
    }

    channels.begin();
    Serial.println(F("channels ready, all off"));

    // Every channel is off at this point, so this reads the true zero-current
    // offset of each sense chain. Do it before any track power is applied.
    occupancy.calibrate();
    Serial.println(F("occupancy baseline calibrated"));

    AOLCB::Configuration config;
    if (can.begin(config)) {
        Serial.printf("CAN up at %lu bit/s on the LCC bus\n", (unsigned long)LCC_BITRATE);
    } else {
        Serial.println(F("CAN failed to start — check the MCP2562 at U2"));
    }

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        node.addConsumer(evChannelOn(i));
        node.addConsumer(evChannelOff(i));
        node.addConsumer(evChannelDCC(i));
        node.addProducer(evChannelFault(i));
        node.addProducer(evBlockOccupied(i));
        node.addProducer(evBlockClear(i));
    }
    node.onEvent(handleLccEvent);
    node.begin(can);
    Serial.println(F("LCC node starting, claiming an alias"));

    Serial.println(F("type 'status' or 'stop'; 'a dc 128' drives block A"));
    Serial.println(F("'source on' makes channel A a DCC command station"));
    Serial.println(F("'traction on' translates LCC throttle commands into DCC"));
}

void loop() {
    // Drives the alias handshake, answers protocol queries and dispatches
    // events. Anything the node did not consume is returned for inspection.
    std::queue<AOLCB::Message> unhandled = node.update();
    while (!unhandled.empty()) {
        traction.handleFrame(unhandled.front());
        unhandled.pop();
    }
    dccSource.update();

    static AOLCB::LinkState lastState = AOLCB::LinkState::Inhibited;
    if (node.getState() != lastState) {
        lastState = node.getState();
        if (node.isPermitted()) {
            Serial.printf("LCC node permitted, alias 0x%03X\n", node.getAlias());
        }
    }

    pollConsole();
    pollOccupancy();
    pollFaults();

    // Keep the node on the bus if it ever latches bus-off.
    static uint32_t lastCanCheck = 0;
    if (millis() - lastCanCheck > 1000) {
        lastCanCheck = millis();
        can.recoverIfBusOff();
    }
}
