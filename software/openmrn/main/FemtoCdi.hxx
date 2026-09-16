// The FemtoLCC settings, as CDI groups.
//
// This covers the same ground as ../FemtoLCC/Cdi.cpp — four outputs, eight I/O
// pins and WiFi — but where that file is hand-written XML whose offsets are
// kept in step with Config.h by a checking script, these macros render the XML
// and lay out the bytes from one description. There is nothing to keep in step.
//
// What is deliberately not here, and is in the AOLCB firmware: the I/O
// expansion boards and the servo/light boards on the Qwiic connector, and the
// USB GridConnect setting. See README.md.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_FEMTOCDI_HXX_
#define _FEMTOLCC_FEMTOCDI_HXX_

#include "openlcb/ConfigRepresentation.hxx"
#include "openlcb/MemoryConfig.hxx"

#include "board.h"

namespace openlcb
{

// ---------------------------------------------------------------------------
// Value maps. These render as <relation> entries, which is what makes a
// configuration tool draw a drop-down instead of a number box. The numbers
// match the enums in Config.h.
// ---------------------------------------------------------------------------

/// ChannelRole.
static const char FEMTO_ROLE_MAP[] =
    "<relation><property>0</property><value>Unused</value></relation>"
    "<relation><property>1</property><value>Track block</value></relation>"
    "<relation><property>2</property><value>Turnout motor</value></relation>";

/// PowerOnMode.
static const char FEMTO_BLOCK_POWER_MAP[] =
    "<relation><property>0</property><value>Off</value></relation>"
    "<relation><property>1</property><value>DC</value></relation>"
    "<relation><property>2</property><value>DCC</value></relation>";

/// DCC polarity, and turnout motor direction.
static const char FEMTO_POLARITY_MAP[] =
    "<relation><property>0</property><value>Normal</value></relation>"
    "<relation><property>1</property><value>Reversed</value></relation>";

/// MotorType.
static const char FEMTO_MOTOR_MAP[] =
    "<relation><property>0</property><value>Stall motor</value></relation>"
    "<relation><property>1</property><value>Pulse</value></relation>";

/// TurnoutPowerOn.
static const char FEMTO_TURNOUT_POWER_MAP[] =
    "<relation><property>0</property><value>Closed</value></relation>"
    "<relation><property>1</property><value>Thrown</value></relation>"
    "<relation><property>2</property><value>Leave alone</value></relation>";

/// PinMode.
static const char FEMTO_PIN_MODE_MAP[] =
    "<relation><property>0</property><value>Unused</value></relation>"
    "<relation><property>1</property><value>Input</value></relation>"
    "<relation><property>2</property><value>Input with pull-up</value></relation>"
    "<relation><property>3</property><value>Output</value></relation>";

/// Pin polarity.
static const char FEMTO_INVERT_MAP[] =
    "<relation><property>0</property><value>Normal</value></relation>"
    "<relation><property>1</property><value>Inverted</value></relation>";

/// On/off, for WiFi.
static const char FEMTO_ENABLE_MAP[] =
    "<relation><property>0</property><value>Off</value></relation>"
    "<relation><property>1</property><value>On</value></relation>";

// ---------------------------------------------------------------------------
// An output used as a track block
// ---------------------------------------------------------------------------

CDI_GROUP(BlockConfig);
CDI_GROUP_ENTRY(power_on, Uint8ConfigEntry, Name("At power-on"), Default(0),
    MapValues(FEMTO_BLOCK_POWER_MAP),
    Description("How the block is powered when the board starts."));
CDI_GROUP_ENTRY(dcc_reversed, Uint8ConfigEntry, Name("DCC polarity"),
    Default(0), MapValues(FEMTO_POLARITY_MAP),
    Description("Reversed swaps the rails while passing DCC through, for a "
                "reversing section."));
CDI_GROUP_ENTRY(occupied_ma, Uint16ConfigEntry, Name("Occupied at (mA)"),
    Default(5),
    Description("The block is reported occupied once it draws this much. The "
                "default catches a locomotive, a lit car or a 4.7 kOhm "
                "resistive wheelset; 10 kOhm wheelsets draw too little to "
                "detect reliably. Detection only works while the block has "
                "power."));
CDI_GROUP_ENTRY(clear_ma, Uint16ConfigEntry, Name("Clear below (mA)"),
    Default(3),
    Description("The block is not reported clear until it falls below this. "
                "The gap between the two is what stops a block on the "
                "threshold from chattering."));
CDI_GROUP_ENTRY(event_on, EventConfigEntry, Name("Power on (DC)"),
    Description("Acted on: powers the block with DC."));
CDI_GROUP_ENTRY(event_off, EventConfigEntry, Name("Power off"),
    Description("Acted on: switches the block off."));
CDI_GROUP_ENTRY(event_dcc, EventConfigEntry, Name("DCC on"),
    Description("Acted on: passes the DCC track signal through to the "
                "block."));
CDI_GROUP_ENTRY(event_occupied, EventConfigEntry, Name("Occupied"),
    Description("Sent when the block becomes occupied."));
CDI_GROUP_ENTRY(event_clear, EventConfigEntry, Name("Clear"),
    Description("Sent when the block becomes clear."));
CDI_GROUP_END();

// ---------------------------------------------------------------------------
// An output used as a turnout motor
// ---------------------------------------------------------------------------

CDI_GROUP(TurnoutConfig);
CDI_GROUP_ENTRY(motor, Uint8ConfigEntry, Name("Motor type"), Default(0),
    MapValues(FEMTO_MOTOR_MAP),
    Description("Stall motors (Tortoise, Cobalt and similar) are powered "
                "continuously, one polarity per position. Pulse suits "
                "two-wire latching motors, which are powered for the pulse "
                "length and then switched off."));
CDI_GROUP_ENTRY(pulse_ms, Uint16ConfigEntry, Name("Pulse length (ms)"),
    Default(200), Description("Pulse motors only."));
CDI_GROUP_ENTRY(duty, Uint8ConfigEntry, Name("Drive strength"), Default(255),
    Description("255 is full supply voltage. Lower values run a stall motor "
                "below it."));
CDI_GROUP_ENTRY(reverse, Uint8ConfigEntry, Name("Direction"), Default(0),
    MapValues(FEMTO_POLARITY_MAP),
    Description("Set to Reversed if the turnout throws when it should close, "
                "rather than rewiring the motor."));
CDI_GROUP_ENTRY(power_on, Uint8ConfigEntry, Name("At power-on"), Default(2),
    MapValues(FEMTO_TURNOUT_POWER_MAP),
    Description("Where the turnout goes when the board starts."));
CDI_GROUP_ENTRY(event_throw, EventConfigEntry, Name("Throw"),
    Description("Acted on: drives the turnout to thrown."));
CDI_GROUP_ENTRY(event_close, EventConfigEntry, Name("Close"),
    Description("Acted on: drives the turnout to closed."));
CDI_GROUP_ENTRY(event_thrown, EventConfigEntry, Name("Thrown"),
    Description("Sent once the turnout has been commanded thrown. The board "
                "has no feedback from the motor."));
CDI_GROUP_ENTRY(event_closed, EventConfigEntry, Name("Closed"),
    Description("Sent once the turnout has been commanded closed."));
CDI_GROUP_END();

// ---------------------------------------------------------------------------
// One of the four outputs, A-D at J5-J8
// ---------------------------------------------------------------------------

CDI_GROUP(ChannelConfig);
CDI_GROUP_ENTRY(description, StringConfigEntry<24>, Name("Description"),
    Description("What this output drives."));
CDI_GROUP_ENTRY(role, Uint8ConfigEntry, Name("Use"), Default(0),
    MapValues(FEMTO_ROLE_MAP),
    Description("What the output is for. Only the events of the use chosen "
                "here are active."));
CDI_GROUP_ENTRY(event_fault, EventConfigEntry, Name("Fault"),
    Description("Sent whatever the use, when the driver reports a short, "
                "overheating or low supply. The output is switched off; "
                "command it again once the cause is fixed."));
CDI_GROUP_ENTRY(block, BlockConfig, Name("Track block"));
CDI_GROUP_ENTRY(turnout, TurnoutConfig, Name("Turnout motor"));
CDI_GROUP_END();

// ---------------------------------------------------------------------------
// One of the eight I/O pins, P0-P7 on the expander's port A
// ---------------------------------------------------------------------------

CDI_GROUP(PinConfig);
CDI_GROUP_ENTRY(description, StringConfigEntry<20>, Name("Description"),
    Description("What this pin is wired to."));
CDI_GROUP_ENTRY(mode, Uint8ConfigEntry, Name("Mode"), Default(0),
    MapValues(FEMTO_PIN_MODE_MAP),
    Description("An input is active when pulled to ground, so a button or "
                "switch wired to ground wants Input with pull-up. An output "
                "can only pull to ground: wire the load from the supply into "
                "the pin."));
CDI_GROUP_ENTRY(invert, Uint8ConfigEntry, Name("Polarity"), Default(0),
    MapValues(FEMTO_INVERT_MAP),
    Description("Inverted swaps active and inactive for an input, and on and "
                "off for an output."));
CDI_GROUP_ENTRY(debounce_ms, Uint16ConfigEntry, Name("Debounce (ms)"),
    Default(20),
    Description("How long a new level must hold before it counts. Inputs "
                "only; 20 ms suits most switches."));
CDI_GROUP_ENTRY(event_active, EventConfigEntry, Name("Input active"),
    Description("Sent when the input becomes active."));
CDI_GROUP_ENTRY(event_inactive, EventConfigEntry, Name("Input inactive"),
    Description("Sent when the input becomes inactive."));
CDI_GROUP_ENTRY(event_on, EventConfigEntry, Name("Output on"),
    Description("Acted on: pulls the pin to ground."));
CDI_GROUP_ENTRY(event_off, EventConfigEntry, Name("Output off"),
    Description("Acted on: lets the pin go."));
CDI_GROUP_END();

// ---------------------------------------------------------------------------
// WiFi
//
// OpenMRN's own WiFiConfiguration (included by the segment below) covers the
// hub and uplink behaviour but not which network to join, because
// Esp32WiFiManager takes those as constructor arguments. This group supplies
// them, so that the network can be set from a configuration tool as it can on
// the AOLCB firmware.
//
// Unlike the AOLCB firmware, the password is NOT write-only: it reads back as
// it was written. See README.md.
// ---------------------------------------------------------------------------

CDI_GROUP(WiFiCredentialsConfig);
CDI_GROUP_ENTRY(enable, Uint8ConfigEntry, Name("WiFi"), Default(0),
    MapValues(FEMTO_ENABLE_MAP),
    Description("Takes effect after a restart."));
CDI_GROUP_ENTRY(ssid, StringConfigEntry<33>, Name("Network name"),
    Description("The name of the WiFi network to join."));
CDI_GROUP_ENTRY(password, StringConfigEntry<64>, Name("Password"),
    Description("Readable by anyone who can read this node's configuration."));
CDI_GROUP_ENTRY(hostname, StringConfigEntry<32>, Name("Host name prefix"),
    Description("Prefix for the name the board announces over mDNS; the node "
                "ID is appended to it. Leave blank for \"femtolcc-\"."));
CDI_GROUP_END();

} // namespace openlcb

#endif // _FEMTOLCC_FEMTOCDI_HXX_
