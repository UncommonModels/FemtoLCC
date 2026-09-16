// FemtoLCC — ESP32-C6 LCC node with four block driver channels.
//
//   Uncommon Models — https://uncommonmodels.com
//
// What it does:
//   * four block driver outputs, each set up as a track block (DC or DCC
//     pass-through, with occupancy detection) or a turnout motor
//   * the eight expander lines /P0../P7 as inputs or outputs
//   * I/O expander boards and PCA9685 servo/light boards on the Qwiic
//     connector J3
//   * OpenLCB on the CAN bus, over WiFi as GridConnect/TCP, and over the USB
//     console as GridConnect - an LCC-USB adapter for JMRI
//   * remote configuration from JMRI: the CDI (Cdi.cpp) describes every
//     setting, which lives in NVS flash (Config.h)
//   * DCC generation on channel A, LCC-to-DCC traction translation, and a
//     serial console that can drive everything directly
//
// See software/README.md.

#include <Arduino.h>
#include <AOLCB.h>
#include <CdiWebServer.h>   // not in AOLCB.h: it pulls in the web server

#include "board.h"
#include "Config.h"
#include "Notice.h"
#include "Expander.h"
#include "Channels.h"
#include "Occupancy.h"
#include "Turnouts.h"
#include "IoPins.h"
#include "I2cBus.h"
#include "IoBoards.h"
#include "ServoBoards.h"
#include "Controller.h"
#include "WifiLink.h"
#include "DCCSource.h"
#include "Throttle.h"
#include "LccTraction.h"
#include "LiveControl.h"
#include "ModuleStore.h"

// A node ID must come from a range you own. The 02.01.57.xx.xx.xx block below is
// a placeholder - replace it before putting this on a shared bus, because two
// nodes answering to one ID will break alias allocation for both.
static const uint64_t NODE_ID = 0x020157000001ULL;

// A configuration tool writes a form as a burst of small writes. Settings are
// applied once, this long after the last one, rather than after each.
static const uint32_t APPLY_DELAY_MS = 250;

// Console output is queued for USB in a buffer this big. It must hold the
// longest reply (`config`, with every expansion board in use), because output
// is dropped rather than waited for.
static const size_t CONSOLE_TX_BUFFER = 8192;

// Input from USB waits here between loops. JMRI sends a burst of frames as it
// connects; the default 256 bytes holds only nine.
static const size_t CONSOLE_RX_BUFFER = 1024;

// The USB bridge queues at most eight frames between node updates and drops
// the rest, so the console hands over fewer than that per pass. Anything more
// stays in the receive buffer until the next loop.
static const int GRIDCONNECT_PER_POLL = 6;

static Expander expander(MCP23018_ADDR);
static Channels channels(expander);
static AOLCB::ESP32CANInterface can(PIN_CAN_TX, PIN_CAN_RX, LCC_BITRATE);
static AOLCB::GridConnectStream usbBridge(Serial);
static WifiLink net(can);
static AOLCB::Node node(NODE_ID);

// The same settings as the CDI, as a web page at http://<hostname>.local/.
// Listens once WiFi has an address.
static AOLCB::CdiWebServer web(node);
static Occupancy occupancy(channels);
static Turnouts turnouts(channels);
static IoPins ioPins(expander);
static IoBoards ioBoards;
static ServoBoards servoBoards;
static Controller controller(node, channels, occupancy, turnouts, ioPins, ioBoards, servoBoards);
static DCCSource dccSource(channels);
static Throttle throttle(dccSource, channels);
static LccTraction traction(dccSource);

// Memory space 0xE0: outputs and locomotives driven live by a dispatcher.
static LiveControl live(channels, occupancy, controller, dccSource);

// Memory space 0xE1: this module's part of the track plan, for olcbweb.
static ModuleStore moduleStore;

// The configuration space. The firmware reads `storage`; the node serves
// `lccConfig`, the same bytes with the WiFi password hidden.
static AOLCB::NvsConfigStorage storage("femtolcc", CFG_SPACE_SIZE);
static LccConfigView lccConfig(storage);
static Settings settings;

static bool applyPending = false;
static uint32_t applyRequestedAt = 0;

// --- configuration -------------------------------------------------------------

static void loadConfig() {
    const bool found = storage.begin();
    if (found && loadSettings(storage, settings)) {
        // Saved by a firmware from before the expansion boards: add their
        // defaults, leaving everything else as it was.
        if (addExpansionDefaults(storage, NODE_ID)) {
            storage.commit();
            loadSettings(storage, settings);
            Serial.println(F("expansion board settings added"));
        }
        Serial.println(F("configuration loaded"));
        return;
    }
    // First boot, or a layout written by a different firmware version. Start
    // from the defaults, keeping the node's name and description if it had one.
    writeDefaults(storage, NODE_ID, found);
    storage.commit();
    loadSettings(storage, settings);
    Serial.println(found ? F("configuration layout changed - reset to defaults")
                         : F("no configuration stored - defaults written"));
}

// GridConnect on the USB console. `reset` starts an automatic port with its
// output off - at boot, or when set from the console, where whoever typed the
// command is plainly not a GridConnect host. Otherwise automatic leaves the
// output as it is.
static void applyUsbMode(UsbGridConnect mode, bool reset) {
    switch (mode) {
    case UsbGridConnect::On:  usbBridge.setOutputEnabled(true);   break;
    case UsbGridConnect::Off: usbBridge.setOutputEnabled(false);  break;
    default:
        if (reset) {
            usbBridge.setOutputEnabled(false);
        }
        break;
    }
    setNoticesEnabled(!usbBridge.outputEnabled());
}

// A configuration tool wrote to the node.
static void configWritten(uint32_t offset, uint32_t length) {
    (void)offset;
    (void)length;
    applyPending = true;
    applyRequestedAt = millis();
}

static void applyConfigIfDue() {
    if (!applyPending || millis() - applyRequestedAt < APPLY_DELAY_MS) {
        return;
    }
    applyPending = false;
    if (!loadSettings(storage, settings)) {
        return;
    }
    controller.apply(settings, false);
    applyUsbMode(settings.usbGridConnect, false);
    notice("configuration applied\n");
    if (!net.running(settings.wifi)) {
        notice("WiFi settings changed - they take effect after a reboot\n");
    }
}

static void reboot() {
    Serial.println(F("rebooting"));
    Serial.flush();
    ESP.restart();
}

static void factoryReset() {
    Serial.println(F("factory reset: every setting back to its default"));
    writeDefaults(storage, NODE_ID, false);
    storage.commit();
    reboot();
}

static void handleLccEvent(uint64_t eventId) {
    controller.handleEvent(eventId);
}

// --- console ---------------------------------------------------------------------

static const char* blockText(BlockState s) {
    return s == BlockState::Occupied ? "OCCUPIED"
         : s == BlockState::Clear    ? "clear"
                                     : "-";
}

static const char* turnoutText(TurnoutPosition p) {
    return p == TurnoutPosition::Thrown ? "thrown"
         : p == TurnoutPosition::Closed ? "closed"
                                        : "-";
}

static void printStatus() {
    Serial.println(F("ch  use      mode  dir  duty    mA  state     pulse  fault"));
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        const char* m = channels.mode(i) == ChannelMode::Off ? "off"
                      : channels.mode(i) == ChannelMode::DC  ? "dc "
                                                             : "dcc";
        const ChannelRole role = controller.role(i);
        const char* use = role == ChannelRole::Block   ? "block  "
                        : role == ChannelRole::Turnout ? "turnout"
                                                       : "unused ";
        const char* state = role == ChannelRole::Turnout ? turnoutText(turnouts.position(i))
                                                         : blockText(occupancy.state(i));
        // Occupancy only tracks block channels; read the others directly.
        const uint32_t ma = role == ChannelRole::Block ? occupancy.milliamps(i)
                                                       : channels.currentMilliamps(i);
        // Whether the block is pulsed while it is off, and whether a fault has
        // stopped that.
        const char* pulse = role != ChannelRole::Block ? "-"
                          : occupancy.pulseStopped(i)  ? "FAULT"
                          : occupancy.pulseDetect(i)   ? "on"
                                                       : "off";
        Serial.printf(" %c  %s  %s   %s  %4u  %5lu  %-8s  %-5s  %s\n",
                      'A' + i, use, m,
                      channels.reversed(i) ? "rev" : "fwd",
                      channels.duty(i),
                      (unsigned long)ma,
                      state,
                      pulse,
                      channels.faulted(i) ? "FAULT" : "-");
    }
    for (uint8_t p = 0; p < EXP_GPIO_COUNT; ++p) {
        if (ioPins.mode(p) == PinMode::Output) {
            Serial.printf(" P%u output %s\n", p, ioPins.output(p) ? "on" : "off");
        } else if (ioPins.isInput(p)) {
            Serial.printf(" P%u input  %s\n", p, ioPins.active(p) ? "active" : "inactive");
        }
    }
    ioBoards.printStatus(Serial, true);
    servoBoards.printStatus(Serial, true);
}

static void printConfig() {
    Serial.printf("node %02X.%02X.%02X.%02X.%02X.%02X, firmware %s\n",
                  (unsigned)(NODE_ID >> 40) & 0xFF, (unsigned)(NODE_ID >> 32) & 0xFF,
                  (unsigned)(NODE_ID >> 24) & 0xFF, (unsigned)(NODE_ID >> 16) & 0xFF,
                  (unsigned)(NODE_ID >> 8) & 0xFF,  (unsigned)NODE_ID & 0xFF,
                  FEMTOLCC_VERSION);
    printSettings(settings, Serial);
}

// `wifi` takes its arguments as typed: SSIDs and passwords are case sensitive.
//   wifi                     status
//   wifi <ssid> <password>   store, enable and connect; the password is the
//                            rest of the line, so it may contain spaces
//   wifi off                 disable
static void handleWifi(char* args) {
    while (*args == ' ' || *args == '\t') ++args;
    if (!*args) {
        net.printStatus(Serial);
        return;
    }

    char* ssid = args;
    char* password = ssid;
    while (*password && *password != ' ' && *password != '\t') ++password;
    if (*password) {
        *password++ = '\0';
        while (*password == ' ' || *password == '\t') ++password;
    }
    for (char* end = password + strlen(password); end > password && end[-1] == ' '; ) {
        *--end = '\0';
    }

    if (strcasecmp(ssid, "off") == 0 && !*password) {
        cfgWrite8(storage, cfgWifi(CFG_WIFI_ENABLE), 0);
        storage.commit();
        loadSettings(storage, settings);
        net.disable();
        Serial.println(F("WiFi disabled"));
        return;
    }
    if (strlen(ssid) >= CFG_WIFI_SSID_SIZE || strlen(password) >= CFG_WIFI_PASSWORD_SIZE) {
        Serial.println(F("SSID is at most 32 characters, password at most 63"));
        return;
    }

    cfgWriteString(storage, cfgWifi(CFG_WIFI_SSID), ssid, CFG_WIFI_SSID_SIZE);
    cfgWriteString(storage, cfgWifi(CFG_WIFI_PASSWORD), password, CFG_WIFI_PASSWORD_SIZE);
    cfgWrite8(storage, cfgWifi(CFG_WIFI_ENABLE), 1);
    storage.commit();
    loadSettings(storage, settings);
    Serial.printf("WiFi stored and enabled, joining '%s'\n", settings.wifi.ssid);
    net.connect(settings.wifi.ssid, settings.wifi.password);
}

//   usb                          GridConnect output mode and state
//   usb gridconnect on|off|auto  set the mode
static void handleUsb(char* args) {
    static const char* const modeText[] = { "automatic", "always on", "off" };
    char* what = strtok(args, " \t");
    char* value = strtok(nullptr, " \t");
    if (what) {
        UsbGridConnect mode;
        if (!value || strcmp(what, "gridconnect") != 0) {
            Serial.println(F("usage: usb gridconnect on|off|auto"));
            return;
        } else if (strcmp(value, "on") == 0) {
            mode = UsbGridConnect::On;
        } else if (strcmp(value, "off") == 0) {
            mode = UsbGridConnect::Off;
        } else if (strcmp(value, "auto") == 0) {
            mode = UsbGridConnect::Auto;
        } else {
            Serial.println(F("usage: usb gridconnect on|off|auto"));
            return;
        }
        cfgWrite8(storage, cfgUsb(CFG_USB_GRIDCONNECT), (uint8_t)mode);
        storage.commit();
        loadSettings(storage, settings);
        applyUsbMode(mode, true);
    }
    Serial.printf("USB GridConnect output %s, %s\n",
                  modeText[(uint8_t)settings.usbGridConnect],
                  usbBridge.outputEnabled() ? "sending" : "not sending");
}

// i2c: every address on the bus that answers, then the expansion boards as
// configured and whether each is answering.
static void handleI2c() {
    Serial.printf("I2C bus at %s kHz\n", i2cFast() ? "400" : "100");
    int answering = 0;
    for (uint8_t a = 0x08; a <= 0x7F; ++a) {
        if (!i2cProbe(a)) {
            continue;
        }
        ++answering;
        char what[48] = "not configured";
        if (a == MCP23018_ADDR) {
            snprintf(what, sizeof(what), "on-board MCP23018 (U3)");
        } else if (a == 0x70) {
            snprintf(what, sizeof(what), "PCA9685 All Call: a servo board not set up");
        }
        for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b) {
            if (ioBoards.fitted(b) && ioBoards.address(b) == a) {
                snprintf(what, sizeof(what), "I/O board %u", b + 1);
            }
        }
        for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b) {
            if (servoBoards.fitted(b) && servoBoards.address(b) == a) {
                snprintf(what, sizeof(what), "servo board %u", b + 1);
            }
        }
        Serial.printf("  0x%02X  %s\n", a, what);
    }
    if (!answering) {
        Serial.println(F("  nothing answering"));
    }
    bool any = false;
    for (uint8_t b = 0; b < NUM_XIO_BOARDS; ++b) {
        any = any || settings.ioBoards[b].type != IoBoardType::None;
    }
    for (uint8_t b = 0; b < NUM_SERVO_BOARDS; ++b) {
        any = any || settings.servoBoards[b].type != ServoBoardType::None;
    }
    if (!any) {
        Serial.println(F("no expansion boards configured"));
        return;
    }
    Serial.println(F("expansion boards:"));
    ioBoards.printStatus(Serial, false);
    servoBoards.printStatus(Serial, false);
}

// Boards, lines and channels count from 1, as in the configuration.
//   x <board> <line> on|off   switch an output line on an I/O expansion board
//   x <board> <line>          show the line
// x1.5 on works too.
static void handleExpansionLine(char* args) {
    char* b = strtok(args, " \t.");
    char* l = strtok(nullptr, " \t.");
    char* value = strtok(nullptr, " \t");
    const int board = b ? atoi(b) : 0;
    const int line = l ? atoi(l) : 0;
    if (board < 1 || board > NUM_XIO_BOARDS || line < 1 || line > XIO_LINES) {
        Serial.println(F("usage: x <board 1-4> <line 1-16> [on|off]"));
        return;
    }
    const uint8_t bi = board - 1;
    const uint8_t li = line - 1;
    const PinBank& bank = ioBoards.bank(bi);
    if (!ioBoards.fitted(bi) || li >= bank.count()) {
        Serial.printf("I/O board %d has no line %d\n", board, line);
        return;
    }
    if (value) {
        const bool on = strcmp(value, "on") == 0;
        if (!on && strcmp(value, "off") != 0) {
            Serial.println(F("usage: x <board 1-4> <line 1-16> [on|off]"));
            return;
        }
        if (!controller.setLine(bi, li, on)) {
            Serial.printf("X%d.%d is not an output\n", board, line);
            return;
        }
    }
    const char* state = bank.mode(li) == PinMode::Output ? (bank.output(li) ? "output on" : "output off")
                      : bank.isInput(li) ? (bank.active(li) ? "input active" : "input inactive")
                                         : "unused";
    Serial.printf("X%d.%d %s%s\n", board, line, state,
                  ioBoards.present(bi) ? "" : " (board not answering)");
}

//   servo <board> <channel> <us>                a raw pulse, 500-2500 us, or 0
//                                               to stop it: for finding positions
//   servo <board> <channel> throw|close|on|off  as its events would
static void handleServo(char* args) {
    char* b = strtok(args, " \t.");
    char* c = strtok(nullptr, " \t.");
    char* value = strtok(nullptr, " \t");
    const int board = b ? atoi(b) : 0;
    const int channel = c ? atoi(c) : 0;
    if (board < 1 || board > NUM_SERVO_BOARDS || channel < 1 || channel > SERVO_CHANNELS || !value) {
        Serial.println(F("usage: servo <board 1-2> <channel 1-16> <us>|throw|close|on|off"));
        return;
    }
    const uint8_t bi = board - 1;
    const uint8_t ci = channel - 1;
    if (!servoBoards.fitted(bi)) {
        Serial.printf("servo board %d is not configured\n", board);
        return;
    }
    if (isdigit((unsigned char)value[0])) {
        const long us = atol(value);
        if (us != 0 && (us < 500 || us > 2500)) {
            Serial.println(F("a pulse is 500 to 2500 us, or 0 to stop"));
            return;
        }
        servoBoards.setPulse(bi, ci, (uint16_t)us);
        controller.servoChanged(bi, ci);
        if (us) {
            Serial.printf("S%d.%d pulse %ld us%s\n", board, channel, us,
                          servoBoards.present(bi) ? "" : " (board not answering)");
        } else {
            Serial.printf("S%d.%d pulses off\n", board, channel);
        }
        return;
    }
    const bool thrown = strcmp(value, "throw") == 0 || strcmp(value, "on") == 0;
    if (!thrown && strcmp(value, "close") != 0 && strcmp(value, "off") != 0) {
        Serial.println(F("usage: servo <board 1-2> <channel 1-16> <us>|throw|close|on|off"));
        return;
    }
    if (controller.setServo(bi, ci, thrown)) {
        Serial.printf("S%d.%d %s\n", board, channel, value);
    } else {
        Serial.printf("S%d.%d is not set up as a servo or a light\n", board, channel);
    }
}

// Detection while a block is switched off.
//   pulse             whether each output is pulsed, and how
//   pulse a on|off    set it for one output, and store it
//   pulse a now       one pulse now, and what it drew, for the bench
static void handlePulse(char* args) {
    char* which = strtok(args, " \t");
    char* value = strtok(nullptr, " \t");

    if (!which) {
        for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
            const ChannelSettings& c = settings.channels[i];
            Serial.printf(" %c  detect while off %s, %u us every %u ms%s\n", 'A' + i,
                          c.detectWhileOff ? "on" : "off", c.detectPulseUs, c.detectIntervalMs,
                          controller.role(i) == ChannelRole::Block ? "" : "  (not a block)");
        }
        return;
    }
    if (which[0] < 'a' || which[0] > 'd' || which[1] != '\0') {
        Serial.println(F("usage: pulse [a-d] [on|off|now]"));
        return;
    }
    const uint8_t index = (uint8_t)(which[0] - 'a');

    // A single pulse, whatever the setting says: this is how the reading a
    // block gives is checked on the bench. It needs track power on the board,
    // or the driver is in undervoltage and refuses.
    if (value && strcmp(value, "now") == 0) {
        uint32_t ma = 0;
        if (controller.role(index) != ChannelRole::Block) {
            Serial.printf("channel %c is not a track block\n", which[0]);
        } else if (occupancy.pulseOnce(index, ma)) {
            Serial.printf("channel %c pulsed for %u us: %lu mA\n", which[0],
                          settings.channels[index].detectPulseUs, (unsigned long)ma);
        } else {
            Serial.printf("channel %c cannot be pulsed now - it is powered, passing DCC"
                          " through, or its driver has faulted\n", which[0]);
        }
        return;
    }

    if (value) {
        const bool on = strcmp(value, "on") == 0;
        if (!on && strcmp(value, "off") != 0) {
            Serial.println(F("usage: pulse [a-d] [on|off|now]"));
            return;
        }
        cfgWrite8(storage, cfgPulse(index, CFG_PD_MODE),
                  (uint8_t)(on ? PulseDetect::On : PulseDetect::Off));
        storage.commit();
        loadSettings(storage, settings);
        const ChannelSettings& set = settings.channels[index];
        occupancy.setPulseDetect(index, set.detectWhileOff, set.detectPulseUs,
                                 set.detectIntervalMs);
    }
    const ChannelSettings& c = settings.channels[index];
    Serial.printf("channel %c detect while off %s, %u us every %u ms\n", which[0],
                  c.detectWhileOff ? "on" : "off", c.detectPulseUs, c.detectIntervalMs);
}

// Console grammar, one command per line:
//   a dc 128        channel A, DC drive, duty 128 forward
//   a dc -128       same, reverse
//   b dcc           channel B passes the track signal through
//   c dcc -         same, reversed polarity (reversing section)
//   d off           channel D idle
//   a throw         channel A's turnout thrown (close to close it)
//   stop            every channel off
//   status          print the table above
//   config          print the stored configuration
//   wifi ...        see handleWifi()
//   usb ...         see handleUsb()
//   i2c             scan the bus, list the expansion boards
//   x 1 5 on        I/O expansion board 1, line 5 on; see handleExpansionLine()
//   servo 1 3 1500  servo board 1, channel 3, a 1500 us pulse; see handleServo()
//   pulse a now     one detection pulse on channel A; see handlePulse()
//   factory         every setting back to default, then reboot
//   reboot          restart the board
static void handleCommand(char* line) {
    if (strncasecmp(line, "wifi", 4) == 0 &&
        (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
        handleWifi(line + 4);
        return;
    }

    for (char* p = line; *p; ++p) {
        *p = tolower(*p);
    }

    if (strcmp(line, "stop") == 0) {
        channels.allOff();
        channels.flush();
        for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
            controller.channelChanged(i);
        }
        Serial.println(F("all channels off"));
        return;
    }
    if (strcmp(line, "status") == 0) {
        printStatus();
        return;
    }
    if (strcmp(line, "config") == 0) {
        printConfig();
        return;
    }
    if (strcmp(line, "factory") == 0) {
        factoryReset();
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        reboot();
        return;
    }
    if (strncmp(line, "usb", 3) == 0 && (line[3] == '\0' || line[3] == ' ' || line[3] == '\t')) {
        handleUsb(line + 3);
        return;
    }
    if (strncmp(line, "detect", 6) == 0) {
        char* on = strtok(line + 6, " \t");
        char* off = strtok(nullptr, " \t");
        if (on && off) {
            // Stored for all four channels; the CDI sets them one at a time.
            const uint16_t occupiedMa = (uint16_t)atol(on);
            const uint16_t clearMa = (uint16_t)atol(off);
            for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
                cfgWrite16(storage, cfgChannel(i, CFG_CH_BLK_OCCUPIED_MA), occupiedMa);
                cfgWrite16(storage, cfgChannel(i, CFG_CH_BLK_CLEAR_MA), clearMa);
            }
            storage.commit();
            loadSettings(storage, settings);
            occupancy.setThresholds(occupiedMa, clearMa);
            Serial.printf("thresholds: occupied >= %s mA, clear < %s mA\n", on, off);
        } else {
            Serial.println(F("usage: detect <occupied_mA> <clear_mA>"));
        }
        return;
    }
    if (strncmp(line, "pulse", 5) == 0 && (line[5] == '\0' || line[5] == ' ' || line[5] == '\t')) {
        handlePulse(line + 5);
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
        for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
            controller.channelChanged(i);
        }
        Serial.println(F("baseline recalibrated with all channels off"));
        return;
    }

    if (strncmp(line, "module", 6) == 0) {
        char* arg = strtok(line + 6, " \t");
        if (arg && strcmp(arg, "clear") == 0) {
            moduleStore.clear();
        }
        Serial.printf("module description: %lu bytes, crc %08lX%s\n",
                      (unsigned long)moduleStore.length(), (unsigned long)moduleStore.crc(),
                      moduleStore.dirty() ? ", not yet saved" : "");
        if (moduleStore.length()) {
            Serial.printf("  %.72s\n", moduleStore.text());
        }
        return;
    }

    if (strcmp(line, "i2c") == 0) {
        handleI2c();
        return;
    }
    if (strncmp(line, "servo", 5) == 0 && (line[5] == '\0' || line[5] == ' ' || line[5] == '\t')) {
        handleServo(line + 5);
        return;
    }
    if (line[0] == 'x' && (line[1] == '\0' || line[1] == ' ' || line[1] == '\t' ||
                           isdigit((unsigned char)line[1]))) {
        handleExpansionLine(line + 1);
        return;
    }

    char ch = line[0];
    if (ch < 'a' || ch > 'd') {
        Serial.println(F("? try: a dc 128 | b dcc | c dcc - | d off | a throw | stop | status | config"
                         " | wifi | usb | i2c | x 1 5 on | servo 1 1 1500"));
        return;
    }
    const uint8_t index = ch - 'a';

    char* mode = strtok(line + 1, " \t");
    char* arg = strtok(nullptr, " \t");
    if (!mode) {
        Serial.println(F("? missing mode"));
        return;
    }

    if (strcmp(mode, "throw") == 0 || strcmp(mode, "close") == 0) {
        const bool thrown = mode[0] == 't';
        if (controller.setTurnout(index, thrown)) {
            Serial.printf("turnout %c %s\n", ch, thrown ? "thrown" : "closed");
        } else {
            Serial.printf("channel %c is not configured as a turnout\n", ch);
        }
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
        Serial.println(F("? mode must be off, dc, dcc, throw or close"));
        return;
    }

    channels.flush();
    controller.channelChanged(index);
    Serial.printf("channel %c set\n", ch);
}

// A GridConnect frame from the computer. The first one is how an automatic
// USB port knows a host such as JMRI is there.
static void handleGridConnect(const char* text, size_t len) {
    if (usbBridge.inject(text, len) == 0) {
        return;
    }
    if (!usbBridge.outputEnabled() && settings.usbGridConnect == UsbGridConnect::Auto) {
        notice("USB: GridConnect host found - sending LCC traffic, console messages off\n");
        usbBridge.setOutputEnabled(true);
        setNoticesEnabled(false);
    }
}

// Lines are told apart by their first character: ':' is GridConnect, '<'
// DCC-EX, anything else WiThrottle or the console. No console command or
// throttle grammar starts with ':'.
static void handleLine(char* line, size_t len) {
    if (line[0] == ':') {
        handleGridConnect(line, len);
    } else if (!throttle.handleLine(line)) {
        handleCommand(line);
    }
}

static void pollConsole() {
    static char buf[128];
    static size_t len = 0;
    int frames = 0;

    while (Serial.available() && frames < GRIDCONNECT_PER_POLL) {
        const char c = Serial.read();
        bool end;
        if (c == '\r' || c == '\n') {
            end = len > 0;
        } else {
            if (len < sizeof(buf) - 1) {
                buf[len++] = c;
            }
            // A host may send GridConnect frames back to back with no
            // newline, so ';' ends a line that began with ':'.
            end = c == ';' && buf[0] == ':';
        }
        if (end) {
            buf[len] = '\0';
            if (buf[0] == ':') {
                ++frames;
            }
            handleLine(buf, len);
            len = 0;
        }
    }
}

void setup() {
    // A bigger queue for console output, and no waiting when it is full: with
    // nothing reading the USB port, output is dropped instead of stalling the
    // loop, and GridConnect traffic must never hold up the node.
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxBufferSize(CONSOLE_TX_BUFFER);
    Serial.setRxBufferSize(CONSOLE_RX_BUFFER);
    Serial.setTxTimeoutMs(0);
#endif
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\nFemtoLCC — Uncommon Models, firmware " FEMTOLCC_VERSION));

    // 100 kHz to start; applying the settings moves it to 400 kHz unless a
    // PCF8574 is configured.
    i2cBegin();
    pinMode(PIN_I2C_INT, INPUT_PULLUP);

    if (!expander.begin()) {
        Serial.println(F("MCP23018 not responding — check I2C and U3"));
    }

    // Status LED D4 on, and left on. It hangs from +3.3V through R23 into GPB4,
    // which is open drain, so a low bit is what lights it.
    expander.setPortBBit(EXP_LED_BIT, false);
    expander.flush();

    channels.begin();
    Serial.println(F("channels ready, all off"));

    // Every channel is off at this point, so this reads the true zero-current
    // offset of each sense chain. Do it before any track power is applied.
    occupancy.calibrate();
    Serial.println(F("occupancy baseline calibrated"));

    // Settings, then each output's power-on state, the pins, and the event
    // list the node will announce.
    loadConfig();
    controller.apply(settings, true);
    applyUsbMode(settings.usbGridConnect, true);

    node.setSnip({ "Uncommon Models", "FemtoLCC", "v1", FEMTOLCC_VERSION });
    node.setCdi(CDI_XML);
    node.setConfigStorage(lccConfig);
    node.onConfigWritten(configWritten);
    node.onReboot(reboot);              // called once the acknowledgement is out
    node.onFactoryReset(factoryReset);
    node.onEvent(handleLccEvent);
    node.addMemorySpace(LiveControl::SPACE, live);
    if (moduleStore.begin()) {
        node.addMemorySpace(ModuleStore::SPACE, moduleStore);
    } else {
        Serial.println(F("no flash partition for the module description"));
    }

    // CAN, the WiFi transport if configured, and GridConnect on USB, all on
    // one bus. The console reads the USB port and hands frames over.
    usbBridge.setSharedInput(true);
    net.addPort(usbBridge);
    if (net.begin(settings.wifi)) {
        notice("CAN up at %lu bit/s on the LCC bus\n", (unsigned long)LCC_BITRATE);
    } else {
        notice("CAN failed to start — check the MCP2562 at U2\n");
    }
    node.begin(net.bus());
    notice("LCC node starting, claiming an alias\n");

    web.begin();
    web.advertise(settings.wifi.hostname);

    notice("type 'status', 'config' or 'stop'; 'a dc 128' drives block A\n");
    notice("'wifi <ssid> <password>' joins a network\n");
    notice("'source on' makes channel A a DCC command station\n");
    notice("'traction on' translates LCC throttle commands into DCC\n");
    notice("'i2c' lists the boards on the Qwiic connector\n");
}

void loop() {
    // Drives the alias handshake, answers protocol queries, serves the
    // configuration and dispatches events. Anything the node did not consume
    // is returned for inspection.
    std::queue<AOLCB::Message> unhandled = node.update();
    while (!unhandled.empty()) {
        traction.handleFrame(unhandled.front());
        unhandled.pop();
    }
    dccSource.update();
    net.update();
    web.handle();

    static AOLCB::LinkState lastState = AOLCB::LinkState::Inhibited;
    if (node.getState() != lastState) {
        lastState = node.getState();
        if (node.isPermitted()) {
            notice("LCC node permitted, alias 0x%03X\n", node.getAlias());
        }
    }

    applyConfigIfDue();
    pollConsole();
    controller.poll();
    live.poll();
    moduleStore.poll();

    // Keep the node on the bus if it ever latches bus-off.
    static uint32_t lastCanCheck = 0;
    if (millis() - lastCanCheck > 1000) {
        lastCanCheck = millis();
        can.recoverIfBusOff();
    }
}
