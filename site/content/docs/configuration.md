---
title: "Configuration"
description: "Setting up the outputs, I/O pins, expansion boards and WiFi from JMRI, and connecting over the LCC bus, WiFi or USB."
weight: 6
---

FemtoLCC is configured over LCC itself, the same way as any other LCC node: a
configuration tool reads the node's description of its settings and draws a form.
Nothing needs reflashing to turn a block output into a turnout driver, wire a push
button, or put the board on WiFi.

## Connecting JMRI

The board speaks LCC on three links at once and bridges everything between them,
so JMRI can reach it — and every other node on the CAN bus — through whichever is
handiest.

| Link | JMRI connection |
|---|---|
| USB | LCC, **CAN via GridConnect**, the board's serial port |
| WiFi | LCC, **CAN via GridConnect Network Interface**, `femtolcc-xxxx.local`, port `12021` |
| LCC bus | any LCC adapter JMRI already supports, plugged into the bus |

In JMRI: *Edit → Preferences → Connections*, add a connection, choose **LCC** as
the system manufacturer (called **OpenLCB** in older versions), then the
connection type above.

### Over USB

Plug the board into the computer with USB-C at `P1`. It appears as
`/dev/ttyACM0` on Linux, `/dev/cu.usbmodem…` on a Mac, or a COM port on Windows.
Choose that port in JMRI. The speed setting does not matter: it is a USB port, not
a real serial line.

The same port is the board's console. When JMRI connects and starts sending LCC
traffic the board notices, sends LCC traffic back, and stops printing its own
status messages so they do not get in JMRI's way. The board then works as an
LCC-USB adapter: JMRI sees the whole CAN bus, and anything connected over WiFi.

The **USB → GridConnect output** setting chooses how this works: *Automatic*
(the default, as above), *Always on*, or *Off* to keep the port a plain console.

### Over WiFi

WiFi is off until you set it up. The quickest way is the console — open the USB
port in any serial terminal and type:

```
wifi MyNetwork my network password
```

The first word is the network name; everything after it is the password. The
board saves them and joins straight away, printing its address:

```
WiFi: connected, IP 192.168.1.57, femtolcc-0001.local
LCC hub listening on port 12021
```

You can also fill in the **WiFi** section of the configuration form and reboot
the board. Either way, point JMRI at `femtolcc-0001.local` (the number is the end of
the board's node ID; the console command `wifi` shows it) or at the IP address, port
`12021`.

By default the board **acts as a hub**: JMRI, and up to three other tools, connect
to it. Set **Hub mode** to *Connect to a hub* to have it connect out to an existing
LCC hub instead, such as JMRI's own hub server. Give the hub's address as an IP
address — a name has to be looked up first, which can pause the board.

`wifi off` turns WiFi off again.

## Opening the configuration

In JMRI choose *LCC → Configure Nodes*. The board appears as **FemtoLCC** by
Uncommon Models; select it and press *Open Configuration*. Change what you need
and press *Write* beside a setting, or *Save Changes* for the lot.

### In a web browser

Once the board is on WiFi you can skip JMRI: open `http://femtolcc-0001.local/`
(or the board's IP address) and the same settings appear as a web page, served
by the board itself. Change what you need and press *Save*. The page also has
*Reboot* and *Factory reset* buttons. Anyone on your layout network can open it,
just as anyone there could reach the board from JMRI.

Settings for the outputs, I/O pins and expansion boards take effect a moment
after they are written — no restart. WiFi settings take effect after a restart: use *Restart*
in the configuration window, or unplug the board.

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
state only while it has power. `pulse a off` on the console does the same.

These settings are in their own **Detection while off** section of the form, one
entry per output, rather than under *Track block*: they belong there, but the
bytes they use sit at the end of the board's configuration space. *Pulse length*
and *Pulse interval* are there too — the defaults, 2 ms every 300 ms, suit
everything the board can detect, and the four outputs are spread across the
interval so only one is ever pulsing.

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
  refused. The board is reported on the console and not used.

The console command `i2c` lists every address that answers, and which board
each one is.

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

- *Closed position* and *Thrown position* are pulse widths in microseconds. Find
  them with the console: `servo 1 1 1500` sends board 1, channel 1 to the middle,
  and you can try other values until the points sit right. To reverse a servo,
  swap the two.
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

A configured board that does not answer is reported once on the console
(`I/O board 1 (MCP23017 at 0x21) not answering`) and shows as `NOT ANSWERING` in
`status`. The FemtoLCC tries it again every few seconds. When it answers, the
board is set up and its outputs and servos go where they were last commanded.
Until then, its inputs and servos report their state as unknown.

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

*n* is 0–3 for outputs A–D, and *p* is 0–7 for pins `P0`–`P7`. With the placeholder
node ID `02.01.57.00.00.01`, *block A power on* is `02.01.57.00.00.01.01.00`.

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
for the train in each block, or DCC — through a separate live-control area the
board offers over LCC. Nothing it sets is saved, and it holds only while the
dispatcher keeps in touch: if the dispatcher stops, the blocks it was powering
switch off within a few seconds. Details are in the firmware
[README](https://github.com/UncommonModels/FemtoLCC/tree/main/software).

## Starting over

*Factory Reset* in JMRI's configuration window, or `factory` on the console, puts
every setting back to its default — names, events and WiFi included — and
restarts the board.

## The console

The USB port doubles as a serial console, at any speed, one command per line. The
configuration commands:

| Command | Effect |
|---|---|
| `config` | Show the current configuration |
| `status` | Show each output's state, current, faults and whether it is pulsed while off, and the I/O pins |
| `pulse` | Show whether each output is pulsed while it is switched off |
| `pulse a on` / `pulse a off` | Turn that on or off for output A |
| `pulse a now` | Pulse output A once and show what it drew, in mA |
| `wifi` | Show WiFi status: network, address, connected clients |
| `wifi <name> <password>` | Save a network, turn WiFi on and join it |
| `wifi off` | Turn WiFi off |
| `usb` | Show the USB GridConnect mode |
| `usb gridconnect on\|off\|auto` | Set it |
| `a throw` / `a close` | Move output A's turnout |
| `i2c` | List what answers on the Qwiic connector, and the configured boards |
| `x 1 5 on` / `x 1 5 off` | Switch I/O expansion board 1, line 5 |
| `servo 1 3 1500` | Send board 1, channel 3 a 1500 µs pulse, to find a position (`0` stops it) |
| `servo 1 3 throw` | Move it as its events would: `throw`, `close`, `on` or `off` |
| `factory` | Factory reset |
| `reboot` | Restart |

The full list is in the firmware
[README](https://github.com/UncommonModels/FemtoLCC/tree/main/software).
