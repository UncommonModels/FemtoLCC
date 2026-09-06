// Serial throttle protocols.
//
// Two grammars share the port, told apart by their first character:
//
//   <...>   DCC-EX native command protocol
//   other   WiThrottle
//
// Both are line-oriented text, so a terminal, JMRI, Engine Driver over a serial
// bridge, or a DCC-EX client can all drive the same DCCSource.
//
// This is a working subset, not the whole of either protocol. What is here is
// what a throttle needs: acquire a loco, set speed and direction, toggle
// functions, and stop everything.

#pragma once

#include <stdint.h>
#include "DCCSource.h"

class Channels;

class Throttle {
public:
    Throttle(DCCSource& dcc, Channels& channels);

    // Feed one complete line, without its terminator. Returns true if the line
    // was recognised as a throttle command.
    bool handleLine(char* line);

private:
    bool handleDccEx(char* line);      // "<...>" already stripped of the brackets
    bool handleWiThrottle(char* line);

    void powerOn();
    void powerOff();

    DCCSource& dcc_;
    Channels& channels_;

    // WiThrottle assigns locos to throttle letters; a client may drive several.
    static const uint8_t MAX_THROTTLES = 6;
    struct Assignment {
        char key;           // throttle letter, 'T', 'S', ...
        uint16_t address;
        bool used;
    };
    Assignment assigned_[MAX_THROTTLES];

    uint16_t addressFor(char key) const;
    void assign(char key, uint16_t address);
    void release(char key);
    bool powered_;
};
