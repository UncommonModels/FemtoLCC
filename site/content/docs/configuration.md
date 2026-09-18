---
title: "Configuration"
description: "Setting up the outputs, I/O pins, expansion boards and WiFi from JMRI, and connecting over the LCC bus, USB or WiFi."
weight: 6
---

FemtoLCC is configured over LCC itself, the same way as any other LCC node: a
configuration tool reads the node's description of its settings and draws a form.
Nothing needs reflashing to turn a block output into a turnout driver, wire a push
button, or put the board on WiFi.

There is no serial console to type commands at, and no settings page served by the
board. Everything below is done from JMRI, or from any other OpenLCB configuration
tool.

## Connecting JMRI

The board speaks LCC on three links at once and bridges everything between them,
so JMRI can reach it — and every other node on the CAN bus — through whichever is
handiest.

| Link | JMRI connection |
|---|---|
| USB | LCC, **CAN via GridConnect**, the board's USB port |
| WiFi | LCC, **CAN via GridConnect Network Interface**, `femtolcc-<node-id>.local`, port `12021` |
| LCC bus | any LCC adapter JMRI already supports, plugged into the bus |

In JMRI: *Edit → Preferences → Connections*, add a connection, choose **LCC** as
the system manufacturer (called **OpenLCB** in older versions), then the
connection type above.

WiFi is off on a new board, so the first time you set one up, connect over USB or
over the LCC bus.

### Over USB

Plug the board into the computer with USB-C at `P1`. It appears as
`/dev/ttyACM0` on Linux, `/dev/cu.usbmodem…` on a Mac, or a COM port on Windows.
Choose that port in JMRI. The speed setting does not matter: it is a USB port, not
a real serial line.

That port carries **only** LCC traffic, in GridConnect form. The board works as an
LCC-USB adapter: JMRI sees the whole CAN bus, and anything connected over WiFi.
It is not a console — opening it in a terminal shows GridConnect frames, not
status messages. The board's own messages come out of the 6-pin UART header at
`J1` instead; see [The console](#the-console).

### Over WiFi

Fill in the **WiFi network** section of the configuration form and restart the
board. WiFi settings are read once at start-up, so they only take effect after a
restart.

| Setting | |
|---|---|
| **WiFi** | *On* or *Off*. Off is the default |
| **Network name** | the network to join |
| **Password** | write-only: it is stored, but always reads back blank |
| **Host name prefix** | the start of the name the board announces over mDNS; blank means `femtolcc-` |

The password reading back blank is deliberate, so it cannot be recovered from the
board by anyone who can reach it over LCC. A configuration tool that checks what
it wrote will report a mismatch on that one field, which is expected. Leave the
field alone to keep the current password; write a new one to change it.

The board announces itself over mDNS as the prefix followed by its node ID, so a
board that is node `02.01.57.A7.CD.94` is `femtolcc-020157a7cd94.local`. Every
board has a different name, because each takes its node ID from the chip inside
it. You never have to be told the name: JMRI shows the node ID in the node list,
and the board prints it on the `J1` console at start-up, so the name follows from
it. If the name does not resolve on your network, use the board's IP address —
from your router's list of clients — with port `12021` instead.

#### Hub and uplink

The **WiFi hub and uplink** section decides what the board does with the network.
*Connection Mode* is the main setting:

| Connection Mode | What the board does |
|---|---|
| Uplink only | Connects out to an existing LCC hub. **The default** |
| Hub only | Accepts connections; JMRI and other tools connect *to* the board |
| Hub+Uplink | Both |
| Disabled | Neither; WiFi carries no LCC traffic |

A new board is set to **Uplink only**, so it looks for a hub to join — JMRI's own
hub server, for instance. Under *Node Uplink Configuration*, *Search Mode* chooses
between finding that hub over mDNS (*Auto Address*, by *mDNS Service*) and being
told where it is (*Manual Address*, an *IP Address* and *Port Number*). An IP
address is the more predictable of the two, since a name has to be looked up
first.

Set *Connection Mode* to **Hub only** to have JMRI connect to the board instead.
*Hub Listener Port* is the port it listens on, `12021` by default.

## Opening the configuration

In JMRI choose *LCC → Configure Nodes*. The board appears as **FemtoLCC** by
Uncommon Models; select it and press *Open Configuration*. Change what you need
and press *Write* beside a setting, or *Save Changes* for the lot.

Settings for the outputs, I/O pins and expansion boards take effect a moment after
they are written — no restart. WiFi settings take effect after a restart: use
*Restart* in the configuration window, or unplug the board.

Give the board a name and description in the **Node** section at the top; JMRI
shows them in the node list.

## Outputs

Each of the four outputs, A to D at terminals `J5`–`J8`, is set to one use. The
form numbers them 1 to 4.

| Use | What it does |
|---|---|
| Track block | Powers a block with DC or DCC and reports whether it is occupied |
| Turnout motor | Drives a turnout motor to thrown or closed |
| Unused | Held off, and sends no events |

### Track block

A block can be powered three ways: **DC** at full power, **DCC** passed through
from the track signal wired to the board, or **off**. Events switch between them.
*At power-on* chooses which the block starts in.

*DCC polarity: Reversed* swaps the rails while passing DCC through, for a reversing
section.

The board detects occupancy from the current the block draws. It reports the block
**occupied** once the current reaches *Occupied at* and **clear** once it falls below
*Clear below*. The defaults, 5 mA and 3 mA, catch a locomotive or a lit car. A
4.7 kΩ resistive wheelset draws only about 3 mA, so lower both settings if you
rely on wheelsets; 10 kΩ wheelsets draw too little to detect reliably.

#### Detection while off

A block with no power on it draws no current, so there would be nothing to
detect. Instead the board **pulses** it: full voltage on the block for about two
milliseconds, a measurement taken while it is there, and the block switched off
again — about three times a second. A block then reports occupied or clear
whether it is powered or not, which is what lets a dispatcher see where a train
is standing in a block it is not powering, and give it the next block.

The pulse cannot move a train. Two milliseconds in three hundred is well under
one per cent of the power a block would normally get, and it is far too short
for a motor to respond to at all.

What it can do is make lit stock flicker. An LED coach standing in the block is
lit for two milliseconds each time, which in a dark room some people will see.
If that bothers you — or you would simply rather the board left dead track dead
— set *Detect while off* to **Off** for that output. The block then reports its
state only while it has power.

*Pulse length* and *Pulse interval* are alongside it. The defaults, 2 ms every
300 ms, suit everything the board can detect, and the four outputs are spread
across the interval so only one is ever pulsing.

### Turnout motor

| Motor type | For | How it is driven |
|---|---|---|
| Stall motor | Tortoise, Cobalt and similar slow-motion motors | Powered continuously, one polarity per position |
| Pulse | Two-wire latching motors | Powered for *Pulse length*, then switched off |

*Drive strength* runs a stall motor below full voltage (255 is full). If the turnout
throws when it should close, set *Direction* to *Reversed* rather than rewiring.
*At power-on* sets the turnout closed or thrown when the board starts, or leaves it
alone.

The board has no feedback from the motor. It reports **thrown** or **closed** as
soon as it has commanded the move.

A driver fault — a short, overheating or low supply — switches the output off and
sends its **Fault** event, whatever the output's use. Command the output again once
the cause is fixed.

## I/O pins

The eight lines `P0`–`P7` on the expander can each be an input or an output. The form
numbers them 1 to 8, so pin 1 is `P0`.

| Mode | Use it for |
|---|---|
| Input with pull-up | A push button, toggle switch or contact wired between the pin and ground |
| Input | A signal that drives the line itself, high or low |
| Output | An LED, relay or other load |
| Unused | Nothing; the pin rests with its pull-up on |

An input is **active** when it is pulled to ground — a button pressed, a switch
closed. It sends *Input active* and *Input inactive* as it changes. *Debounce* is how
long a new level must hold before it counts; 20 ms suits most switches.

Outputs can only pull a line to ground; they cannot drive it high. Wire the load from
the supply, through a resistor for an LED, into the pin. *Output on* pulls the pin
low and lights the LED; *Output off* lets it go. A relay coil needs a flyback diode
across it.

*Polarity: Inverted* swaps active and inactive for an input, and on and off for an
output.

## Expansion boards (Qwiic)

The small 4-pin connector `J3` is a Qwiic / STEMMA QT port. Plug in I/O expander
boards for more inputs and outputs, and PCA9685 boards for servo turnouts and
lights. They are set up in two sections of the form: **I/O expansion boards**
(up to four) and **Servo and light boards** (up to two).

### Which boards work

| Section | Chip | What you get | Addresses |
|---|---|---|---|
| I/O expansion boards | MCP23017 | 16 lines, with pull-ups | `0x21`–`0x27` |
| | PCF8574 or PCF8574A | 8 lines | `0x21`–`0x27`, or `0x38`–`0x3F` for the A |
| | TCA9534 or PCA9554, as on SparkFun's Qwiic GPIO | 8 lines, no pull-ups | `0x21`–`0x27`, or `0x38`–`0x3F` for the A versions |
| Servo and light boards | PCA9685, as on Adafruit's 16-channel servo driver or SparkFun's Qwiic servo driver | 16 channels | `0x40`–`0x7F` |

Choose the chip under *Board type*, then its *I2C address*.

### Addresses

Every board on the connector needs its own address, set with solder jumpers or
switches on the board, and the form's *I2C address* must match it.

- `0x20` belongs to the expander on the FemtoLCC itself. Many MCP23017 and
  PCF8574 boards come set to `0x20`, so move them to another address before
  plugging them in.
- On a PCA9685 the address is `0x40` until you bridge jumpers: A0 adds 1, A1 2,
  A2 4, A3 8, A4 16 and A5 32. Avoid `0x70`, which every PCA9685 also answers
  until the board has set it up.
- Two boards set to the same address, or an address the chip cannot have, are
  refused. The board is reported on the `J1` console and not used.

### Wiring

Use a Qwiic / STEMMA QT cable, and daisy-chain boards from one to the next. The
connector carries ground, 3.3 V and the two bus lines.

- **3.3 V only.** Qwiic and STEMMA QT boards are 3.3 V already. A board made for
  5 V, such as a bare PCF8574 module, must not pull the bus lines up to 5 V: the
  FemtoLCC's processor would be damaged. All the chips above run happily at 3.3 V,
  so power such a board from the connector's 3.3 V and remove or disconnect its
  own pull-ups. If it has to run at 5 V, put a level shifter between it and
  `J3`.
- Keep the cables short, a metre or so in all. The bus runs at 400 kHz, or 100 kHz
  while a PCF8574 is configured, since that chip is slower.
- A board plugged in while the FemtoLCC is running is found within a few
  seconds. Plugging in can disturb the bus for a moment, so do it with trains
  stopped.

> **Power servos from their own 5–6 V supply**, wired to the power terminal on
> the servo board, never from the Qwiic 3.3 V pin. Servos draw hundreds of
> milliamps each, far more when they stall, and the connector's 3.3 V only powers
> the chip. Size the supply for all the servos moving at once.

### I/O expansion lines

Each line is set up exactly like the [I/O pins](#io-pins): unused, input, input
with pull-up, or output, with the same polarity and debounce, and its own events.
The form numbers boards 1 to 4 and lines 1 to 16. Line 1 is the chip's first pin:
on an MCP23017 lines 1–8 are `A0`–`A7` and lines 9–16 are `B0`–`B7`. The 8-line
chips use lines 1–8 (`P0`–`P7`) and ignore the rest.

As on `P0`–`P7`, an input is active when pulled to ground and an output pulls to
ground when on, so a button to ground and an LED from the supply work the same
way everywhere. *Inverted* swaps both.

- **MCP23017**: current chips only allow outputs on `A7` and `B7`, lines 8 and 16.
- **PCF8574**: outputs can only pull to ground. Every input has a weak pull-up
  built in.
- **TCA9534, PCA9554**: no pull-ups, so for a button fit a resistor (10 kΩ, say)
  from the line to 3.3 V.

### Servo turnouts

Set a channel's *Use* to a *Servo turnout* choice. The choice also says where the
turnout goes when the board starts: closed, thrown, or left alone. The form numbers
channels 1 to 16, so channel 1 is the output marked 0 on the board.

- *Closed position* and *Thrown position* are pulse widths in microseconds. 1500 µs
  is the middle of a servo's travel and a sensible starting point: write a value,
  send the channel its *Throw* or *Close* event to watch it move, and adjust until
  the points sit right. To reverse a servo, swap the two.
- *Travel or fade time* is how long the servo takes from one position to the other,
  so it moves slowly like a real turnout motor.
- *Thrown* and *Closed* are sent when the servo gets there.
- *Servos at rest*, set per board: *Stop pulses* (the default) switches a servo's
  signal off once it is in position, so it does not buzz. The throwbar holds the
  points. *Hold position* keeps driving it.

A servo left alone at power-on gets no signal until its first command, and then
moves at its own full speed that once.

### Lights

Set *Use* to a *Light* choice for an LED or lamp. *Throw / light on* and *Close /
light off* switch it, *Brightness* sets how bright it is when on (the scale
follows the eye, so 128 looks about half as bright as 255), and the *Travel or
fade time* fades it up and down. Choose an *Inverted light* for an LED wired from
the supply into the channel. Lights are dimmed at the servos' 50 Hz, so at low
brightness some people, and most cameras, will see a flicker.

### When a board is missing

A configured board that does not answer is reported on the `J1` console
(`I/O board 1 (MCP23017 at 0x21) not answering`). The FemtoLCC tries it again
every few seconds. When it answers, the board is set up and its outputs and servos
go where they were last commanded. Until then, its inputs and servos answer
**unknown** when a panel or JMRI asks their state, rather than claiming a state
they cannot see.

## Events

Every event the board sends or acts on is set in the form. Each starts with a
value unique to the board: its node ID followed by a two-byte suffix.

| Event | Default suffix | |
|---|---|---|
| Block *n* power on (DC) | `01.0n` | acted on |
| Block *n* power off | `01.1n` | acted on |
| Block *n* DCC on | `01.2n` | acted on |
| Output *n* fault | `02.0n` | sent |
| Block *n* occupied | `03.0n` | sent |
| Block *n* clear | `03.1n` | sent |
| Turnout *n* throw | `04.0n` | acted on |
| Turnout *n* close | `04.1n` | acted on |
| Turnout *n* thrown | `04.2n` | sent |
| Turnout *n* closed | `04.3n` | sent |
| Pin *p* input active | `05.0p` | sent |
| Pin *p* input inactive | `05.1p` | sent |
| Pin *p* output on | `06.0p` | acted on |
| Pin *p* output off | `06.1p` | acted on |
| Expansion line input active | `07.ii` | sent |
| Expansion line input inactive | `08.ii` | sent |
| Expansion line output on | `09.ii` | acted on |
| Expansion line output off | `0A.ii` | acted on |
| Servo channel throw / light on | `0B.ii` | acted on |
| Servo channel close / light off | `0C.ii` | acted on |
| Servo turnout thrown | `0D.ii` | sent |
| Servo turnout closed | `0E.ii` | sent |

*n* is 0–3 for outputs A–D, and *p* is 0–7 for pins `P0`–`P7`. Every board has its
own node ID, taken from the chip inside it, so on a board that is
`02.01.57.A7.CD.94`, *block A power on* is `02.01.57.A7.CD.94.01.00`. JMRI shows
the board's node ID in the node list, and the board prints it on the `J1` console
at start-up.

For the expansion boards *ii* is 16 × (board − 1) + (line − 1) in hex, with the
board and line (or channel) numbered as in the form: board 1, line 1 is `00`,
board 1, line 16 is `0F`, and board 2, channel 3 is `12`.

To make a button throw a turnout, copy the button's *Input active* event into the
turnout's *Throw* field (or the other way round). Several outputs can share an
event, so one "all off" event can switch off every block. An event of all zeros
does nothing.

Only the events for an output's current use are active. A panel or JMRI asking
whether a block is occupied, or which way a turnout is set, gets an answer.

## Running trains from a dispatcher

The configuration decides what each output is for. A dispatcher program such as
olcbweb can then drive the track blocks moment to moment — DC speed and direction
for the train in each block — through a separate live-control area the board
offers over LCC. Output A can also generate DCC, with locomotive address, speed,
direction and functions driven from the same area, so a dispatcher can run DCC
locomotives as well as DC blocks.

Nothing a dispatcher sets is saved, and it holds only while the dispatcher keeps
in touch: if the dispatcher stops, the blocks it was powering switch off within a
few seconds, and any locomotives running under DCC are sent an emergency stop.

The board does not host LCC train nodes, so a JMRI throttle cannot drive a
locomotive through it directly; that is what the live-control area is for.
Details are in the firmware
[README](https://github.com/UncommonModels/FemtoLCC/tree/main/software/openmrn).

## Starting over

*Factory Reset* in JMRI's configuration window puts every setting back to its
default — names, events and WiFi included — and restarts the board. The event IDs
are re-derived from the board's node ID, so anything on the layout pointing at the
old ones needs re-pointing.

## The console

The 6-pin UART header at `J1` is a plain serial console at 115200 baud. It is
**output only**: there are no commands to type. It carries the start-up banner —
including the board's node ID, and the WiFi network it is joining, if any — and
messages about expansion boards appearing and disappearing.

Use it when something is not behaving and you want to see what the board thinks is
going on. You need a USB-serial adapter for it; the USB-C port at `P1` is not a
console, because it carries LCC traffic. Flashing new firmware, though, does go
through `P1`.
