// The FemtoLCC settings, as CDI groups.
//
// These macros render the XML a configuration tool reads *and* lay out the
// bytes of memory space 253, both from this one description, so the form and
// the storage behind it cannot drift apart.
//
// Everything the board can be set to is here: the four outputs, the eight I/O
// pins, the Qwiic I/O and servo boards, and WiFi. See README.md.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_FEMTOCDI_HXX_
#define _FEMTOLCC_FEMTOCDI_HXX_

#include "openlcb/ConfigRepresentation.hxx"
#include "openlcb/MemoryConfig.hxx"

#include "Config.h"
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

/// PulseDetect. Note that On is 0, so a board whose bytes were never written
/// still pulses.
static const char FEMTO_PULSE_DETECT_MAP[] =
    "<relation><property>0</property><value>On</value></relation>"
    "<relation><property>1</property><value>Off</value></relation>";

/// IoBoardType.
static const char FEMTO_IO_BOARD_MAP[] =
    "<relation><property>0</property><value>None</value></relation>"
    "<relation><property>1</property><value>MCP23017</value></relation>"
    "<relation><property>2</property><value>PCF8574</value></relation>"
    "<relation><property>3</property><value>TCA9534 / PCA9554</value></relation>";

/// ServoBoardType.
static const char FEMTO_SERVO_BOARD_MAP[] =
    "<relation><property>0</property><value>None</value></relation>"
    "<relation><property>1</property><value>PCA9685</value></relation>";

/// ServoUse. The choice also says where the channel goes at power-on.
static const char FEMTO_SERVO_USE_MAP[] =
    "<relation><property>0</property><value>Unused</value></relation>"
    "<relation><property>1</property><value>Servo turnout, closed at power-on</value></relation>"
    "<relation><property>2</property><value>Servo turnout, thrown at power-on</value></relation>"
    "<relation><property>3</property><value>Servo turnout, left alone</value></relation>"
    "<relation><property>4</property><value>Light, off at power-on</value></relation>"
    "<relation><property>5</property><value>Light, on at power-on</value></relation>"
    "<relation><property>6</property><value>Inverted light, off at power-on</value></relation>"
    "<relation><property>7</property><value>Inverted light, on at power-on</value></relation>";

/// Whether a servo keeps its pulses once it is in position.
static const char FEMTO_SERVO_REST_MAP[] =
    "<relation><property>0</property><value>Stop pulses</value></relation>"
    "<relation><property>1</property><value>Hold position</value></relation>";

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
CDI_GROUP_ENTRY(detect_while_off, Uint8ConfigEntry,
    Name("Detect while off"), Default(0),
    MapValues(FEMTO_PULSE_DETECT_MAP),
    Description("Pulse the block while it is switched off, so it can still be "
                "detected. On by default. Turn it off if lit stock in the "
                "block flickers. It is ignored unless the output is a track "
                "block, and no pulse is ever sent while the block is powered, "
                "is passing DCC through, or its driver has faulted."));
CDI_GROUP_ENTRY(pulse_len_100us, Uint8ConfigEntry,
    Name("Pulse length (100 us)"), Default(20), Min(0), Max(50),
    Description("How long each pulse lasts, in hundreds of microseconds: 20 "
                "is 2 ms, the default. A longer pulse reads more steadily, a "
                "shorter one is less visible in lit stock. 10 to 50; 0 uses "
                "the default."));
CDI_GROUP_ENTRY(pulse_interval_ms, Uint16ConfigEntry,
    Name("Pulse interval (ms)"), Default(300), Min(0), Max(10000),
    Description("How often each block is pulsed. 300 ms is the default, and "
                "the four outputs are spread across the interval so only one "
                "is ever pulsing. 100 to 10000; 0 uses the default."));
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
// I/O expansion boards on the Qwiic connector J3
//
// Each line is set up exactly like an I/O pin, so PinConfig is reused: unused,
// input, input with pull-up or output, with the same polarity, debounce and
// events. The 8-line chips use lines 1-8 and ignore the rest.
// ---------------------------------------------------------------------------

/// The lines on one I/O expansion board. Named because CDI_GROUP_ENTRY is a
/// variadic macro and would split RepeatedGroup<PinConfig, XIO_LINES> at its
/// comma.
using IoBoardLines = RepeatedGroup<PinConfig, XIO_LINES>;

CDI_GROUP(IoBoardConfig);
CDI_GROUP_ENTRY(type, Uint8ConfigEntry, Name("Board type"), Default(0),
    MapValues(FEMTO_IO_BOARD_MAP),
    Description("The chip on the board. MCP23017 has 16 lines with pull-ups; "
                "PCF8574 and TCA9534 have 8."));
CDI_GROUP_ENTRY(address, Uint8ConfigEntry, Name("I2C address"), Default(0x21),
    Min(0), Max(127),
    Description("The address set on the board, as a number: 33 is 0x21. "
                "MCP23017 may use 0x21-0x27; PCF8574 and TCA9534 may also use "
                "0x38-0x3F for the A versions. 0x20 belongs to the expander on "
                "the FemtoLCC itself and may not be used. A factory reset numbers the "
                "boards up from this one: board 1 gets 0x21, board 2 0x22, "
                "and so on."));
CDI_GROUP_ENTRY(lines, IoBoardLines, Name("Lines"), RepName("Line"));
CDI_GROUP_END();

// ---------------------------------------------------------------------------
// Servo and light boards on the Qwiic connector J3
// ---------------------------------------------------------------------------

CDI_GROUP(ServoChannelConfig);
CDI_GROUP_ENTRY(description, StringConfigEntry<20>, Name("Description"),
    Description("What this channel drives."));
CDI_GROUP_ENTRY(use, Uint8ConfigEntry, Name("Use"), Default(0),
    MapValues(FEMTO_SERVO_USE_MAP),
    Description("What the channel drives, and where it goes when the board "
                "starts. A servo left alone gets no signal until its first "
                "command."));
CDI_GROUP_ENTRY(event_throw, EventConfigEntry, Name("Throw / light on"),
    Description("Acted on: throws the servo, or switches the light on."));
CDI_GROUP_ENTRY(event_close, EventConfigEntry, Name("Close / light off"),
    Description("Acted on: closes the servo, or switches the light off."));
CDI_GROUP_ENTRY(event_thrown, EventConfigEntry, Name("Thrown"),
    Description("Sent when a servo turnout reaches its thrown position."));
CDI_GROUP_ENTRY(event_closed, EventConfigEntry, Name("Closed"),
    Description("Sent when a servo turnout reaches its closed position."));
CDI_GROUP_ENTRY(time_ms, Uint16ConfigEntry, Name("Travel or fade time (ms)"),
    Default(1000), Min(0), Max(60000),
    Description("How long a servo takes from one position to the other, so it "
                "moves slowly like a real turnout motor, or how long a light "
                "takes to fade. 0 moves at full speed."));
CDI_GROUP_ENTRY(closed_us, Uint16ConfigEntry, Name("Closed position (us)"),
    Default(1300), Min(0), Max(3000),
    Description("The pulse width for closed. 1500 is the middle of a servo's "
                "travel. To reverse a servo, swap this with thrown."));
CDI_GROUP_ENTRY(thrown_us, Uint16ConfigEntry, Name("Thrown position (us)"),
    Default(1700), Min(0), Max(3000),
    Description("The pulse width for thrown."));
CDI_GROUP_ENTRY(brightness, Uint8ConfigEntry, Name("Brightness"), Default(255),
    Description("How bright a light is when on. The scale follows the eye, so "
                "128 looks about half as bright as 255."));
CDI_GROUP_END();

/// The channels on one servo/light board; named for the same reason.
using ServoBoardChannels = RepeatedGroup<ServoChannelConfig, SERVO_CHANNELS>;

CDI_GROUP(ServoBoardConfig);
CDI_GROUP_ENTRY(type, Uint8ConfigEntry, Name("Board type"), Default(0),
    MapValues(FEMTO_SERVO_BOARD_MAP));
CDI_GROUP_ENTRY(address, Uint8ConfigEntry, Name("I2C address"), Default(0x40),
    Min(0), Max(127),
    Description("The address set on the board, as a number: 64 is 0x40. A "
                "PCA9685 may use 0x40-0x7F. Avoid 0x70, which every PCA9685 "
                "also answers to. A factory reset numbers the boards up from this "
                "one: board 1 gets 0x40 and board 2 0x41."));
CDI_GROUP_ENTRY(hold, Uint8ConfigEntry, Name("Servos at rest"), Default(0),
    MapValues(FEMTO_SERVO_REST_MAP),
    Description("Stop pulses switches a servo's signal off once it is in "
                "position, so it does not buzz; the throwbar holds the "
                "points."));
CDI_GROUP_ENTRY(channels, ServoBoardChannels, Name("Channels"),
    RepName("Channel"));
CDI_GROUP_END();

// ---------------------------------------------------------------------------
// WiFi
//
// OpenMRN's own WiFiConfiguration (included by the segment below) covers the
// hub and uplink behaviour but not which network to join, because
// Esp32WiFiManager takes those as constructor arguments. This group supplies
// them, so that the network can be set from a configuration tool.
//
// The password is write-only: main.cxx wraps memory space 253 so that these
// bytes always read back blank. See README.md.
// ---------------------------------------------------------------------------

CDI_GROUP(WiFiCredentialsConfig);
CDI_GROUP_ENTRY(enable, Uint8ConfigEntry, Name("WiFi"), Default(0),
    MapValues(FEMTO_ENABLE_MAP),
    Description("Takes effect after a restart."));
CDI_GROUP_ENTRY(ssid, StringConfigEntry<33>, Name("Network name"),
    Description("The name of the WiFi network to join."));
CDI_GROUP_ENTRY(password, StringConfigEntry<64>, Name("Password"),
    Description("Write-only: it is stored but always reads back blank, so it "
                "cannot be recovered from the node. A configuration tool that "
                "verifies what it wrote will report a mismatch on this field "
                "only, which is expected. Leave it alone to keep the current "
                "password; write a new one to change it."));
CDI_GROUP_ENTRY(hostname, StringConfigEntry<32>, Name("Host name prefix"),
    Description("Prefix for the name the board announces over mDNS; the node "
                "ID is appended to it. Leave blank for \"femtolcc-\"."));
CDI_GROUP_END();

} // namespace openlcb

#endif // _FEMTOLCC_FEMTOCDI_HXX_
