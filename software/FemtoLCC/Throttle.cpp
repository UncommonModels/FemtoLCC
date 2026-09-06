#include "Throttle.h"
#include "Channels.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

Throttle::Throttle(DCCSource& dcc, Channels& channels)
    : dcc_(dcc), channels_(channels), powered_(false) {
    for (uint8_t i = 0; i < MAX_THROTTLES; ++i) {
        assigned_[i].used = false;
        assigned_[i].key = 0;
        assigned_[i].address = 0;
    }
}

void Throttle::powerOn() {
    if (!dcc_.begin()) {
        Serial.println(F("<X>"));
        return;
    }
    powered_ = true;
    Serial.println(F("<p1>"));
}

void Throttle::powerOff() {
    dcc_.end();
    powered_ = false;
    Serial.println(F("<p0>"));
}

uint16_t Throttle::addressFor(char key) const {
    for (uint8_t i = 0; i < MAX_THROTTLES; ++i) {
        if (assigned_[i].used && assigned_[i].key == key) {
            return assigned_[i].address;
        }
    }
    return 0;
}

void Throttle::assign(char key, uint16_t address) {
    for (uint8_t i = 0; i < MAX_THROTTLES; ++i) {
        if (assigned_[i].used && assigned_[i].key == key) {
            assigned_[i].address = address;
            return;
        }
    }
    for (uint8_t i = 0; i < MAX_THROTTLES; ++i) {
        if (!assigned_[i].used) {
            assigned_[i].used = true;
            assigned_[i].key = key;
            assigned_[i].address = address;
            return;
        }
    }
}

void Throttle::release(char key) {
    for (uint8_t i = 0; i < MAX_THROTTLES; ++i) {
        if (assigned_[i].used && assigned_[i].key == key) {
            assigned_[i].used = false;
            return;
        }
    }
}

bool Throttle::handleLine(char* line) {
    if (line == nullptr || *line == '\0') {
        return false;
    }
    if (line[0] == '<') {
        char* end = strchr(line, '>');
        if (end != nullptr) {
            *end = '\0';
        }
        return handleDccEx(line + 1);
    }
    return handleWiThrottle(line);
}

// --- DCC-EX ----------------------------------------------------------------
//
//   <0>                 power off
//   <1>                 power on
//   <t cab speed dir>   throttle, speed 0-126 or -1 estop, dir 1 fwd / 0 rev
//   <t reg cab speed dir>   the older four-argument form
//   <F cab func 0|1>    set a function
//   <f cab byte>        function group byte, as sent by older clients
//   <e>                 forget all locos
//   <s>                 status
//
bool Throttle::handleDccEx(char* body) {
    const char op = body[0];
    char* args = body + 1;

    switch (op) {
    case '0':
        powerOff();
        return true;

    case '1':
        powerOn();
        return true;

    case 's':
        Serial.println(F("<iDCC-EX FemtoLCC / Uncommon Models>"));
        Serial.print(F("<p")); Serial.print(powered_ ? 1 : 0); Serial.println(F(">"));
        return true;

    case 'e':
        dcc_.forgetAll();
        Serial.println(F("<O>"));
        return true;

    case 't': {
        long v[4] = { 0, 0, 0, 0 };
        uint8_t n = 0;
        for (char* tok = strtok(args, " \t"); tok && n < 4; tok = strtok(nullptr, " \t")) {
            v[n++] = atol(tok);
        }
        // Four arguments means the legacy form with a register in front.
        const long cab   = (n >= 4) ? v[1] : v[0];
        const long speed = (n >= 4) ? v[2] : v[1];
        const long dir   = (n >= 4) ? v[3] : v[2];
        if (cab <= 0) {
            Serial.println(F("<X>"));
            return true;
        }
        dcc_.setSpeed((uint16_t)cab, (int16_t)speed, dir != 0);
        Serial.print(F("<T "));
        Serial.print(cab); Serial.print(' ');
        Serial.print(speed); Serial.print(' ');
        Serial.print(dir); Serial.println(F(">"));
        return true;
    }

    case 'F': {
        long cab = 0, fn = 0, on = 0;
        char* tok = strtok(args, " \t"); if (tok) cab = atol(tok);
        tok = strtok(nullptr, " \t");    if (tok) fn = atol(tok);
        tok = strtok(nullptr, " \t");    if (tok) on = atol(tok);
        if (cab > 0 && fn >= 0 && fn <= 28) {
            dcc_.setFunction((uint16_t)cab, (uint8_t)fn, on != 0);
        }
        return true;
    }

    case 'f': {
        // Group byte form: decode the bits back into individual functions so the
        // loco table stays authoritative.
        long cab = 0, byte1 = 0;
        char* tok = strtok(args, " \t"); if (tok) cab = atol(tok);
        tok = strtok(nullptr, " \t");    if (tok) byte1 = atol(tok);
        if (cab <= 0) {
            return true;
        }
        if ((byte1 & 0xE0) == 0x80) {           // F0-F4
            dcc_.setFunction((uint16_t)cab, 0, (byte1 & 0x10) != 0);
            for (uint8_t i = 1; i <= 4; ++i) {
                dcc_.setFunction((uint16_t)cab, i, (byte1 & (1 << (i - 1))) != 0);
            }
        } else if ((byte1 & 0xF0) == 0xB0) {    // F5-F8
            for (uint8_t i = 0; i < 4; ++i) {
                dcc_.setFunction((uint16_t)cab, (uint8_t)(5 + i), (byte1 & (1 << i)) != 0);
            }
        } else if ((byte1 & 0xF0) == 0xA0) {    // F9-F12
            for (uint8_t i = 0; i < 4; ++i) {
                dcc_.setFunction((uint16_t)cab, (uint8_t)(9 + i), (byte1 & (1 << i)) != 0);
            }
        }
        return true;
    }

    default:
        return false;
    }
}

// --- WiThrottle ------------------------------------------------------------
//
//   N<name>              client name
//   HU<id>               client id
//   *                    heartbeat
//   MT+L1234<;>L1234     assign long address 1234 to throttle T
//   MT-*<;>              release everything on throttle T
//   MTA*<;>V50           speed 50
//   MTA*<;>R1            direction, 1 forward
//   MTA*<;>F10           function 1 pressed  (F then state then number)
//   MTA*<;>X             emergency stop
//   Q                    quit
//
bool Throttle::handleWiThrottle(char* line) {
    switch (line[0]) {
    case 'N':
        // Greet the client the way it expects, so it proceeds to send commands.
        Serial.println(F("VN2.0"));
        Serial.println(F("HTFemtoLCC"));
        Serial.println(F("RL0"));
        Serial.print(F("PPA")); Serial.println(powered_ ? '1' : '0');
        Serial.println(F("*10"));
        return true;

    case 'H':
        return true;                 // client identity, nothing to do

    case '*':
        return true;                 // heartbeat

    case 'Q':
        dcc_.emergencyStopAll();
        return true;

    case 'P':
        // PPA1 / PPA0 - track power.
        if (line[1] == 'P' && line[2] == 'A') {
            if (line[3] == '1') powerOn(); else powerOff();
        }
        return true;

    case 'M': {
        const char key = line[1];
        const char action = line[2];
        char* rest = line + 3;

        if (action == '+') {
            // MT+L1234<;>L1234 - the address follows the L or S prefix.
            char* addr = rest;
            while (*addr && *addr != 'L' && *addr != 'S') ++addr;
            if (*addr) {
                const uint16_t a = (uint16_t)atol(addr + 1);
                if (a > 0) {
                    assign(key, a);
                    Serial.print(F("M")); Serial.print(key);
                    Serial.print(F("+")); Serial.print(addr); Serial.println(F("<;>"));
                }
            }
            return true;
        }
        if (action == '-') {
            release(key);
            return true;
        }
        if (action == 'A') {
            const uint16_t address = addressFor(key);
            if (address == 0) {
                return true;
            }
            char* cmd = strstr(rest, "<;>");
            cmd = cmd ? cmd + 3 : rest;

            switch (cmd[0]) {
            case 'V': {                      // speed 0-126, -1 estop
                const int16_t v = (int16_t)atoi(cmd + 1);
                bool forward = true;
                for (uint8_t i = 0; i < MAX_THROTTLES; ++i) {
                    if (assigned_[i].used && assigned_[i].key == key) {
                        forward = true;      // direction tracked by R below
                    }
                }
                dcc_.setSpeed(address, v, forward);
                return true;
            }
            case 'R': {                      // direction
                const bool forward = cmd[1] != '0';
                // Re-send the current speed with the new direction.
                uint16_t a; int16_t s; bool f;
                int16_t speed = 0;
                for (uint8_t i = 0; dcc_.locoAt(i, a, s, f); ++i) {
                    if (a == address) { speed = s; break; }
                }
                dcc_.setSpeed(address, speed, forward);
                return true;
            }
            case 'F': {                      // F<state><number>
                const bool on = cmd[1] == '1';
                const uint8_t fn = (uint8_t)atoi(cmd + 2);
                dcc_.setFunction(address, fn, on);
                return true;
            }
            case 'X':
                dcc_.setSpeed(address, -1, true);
                return true;
            case 'I':
                dcc_.setSpeed(address, 0, true);
                return true;
            default:
                return true;
            }
        }
        return true;
    }

    default:
        return false;
    }
}
