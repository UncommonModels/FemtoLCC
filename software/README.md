# FemtoLCC firmware

Firmware for the FemtoLCC board: an ESP32-C6 node with four block driver
channels, an isolated DCC input and an LCC bus interface. It is configured
remotely over LCC — from JMRI or any other OpenLCB configuration tool — and can
be reached over the CAN bus, WiFi and USB.

```
software/FemtoLCC/
  FemtoLCC.ino           main sketch — setup, serial console, USB bridge
  board.h                pin map, read out of the KiCad netlist
  Config.{h,cpp}         configuration space layout, defaults, settings
  Cdi.cpp                the CDI: the XML that describes the settings to JMRI
  Controller.{h,cpp}     applies the settings; maps events to actions
  Channels.{h,cpp}       the four block driver channels
  Turnouts.{h,cpp}       turnout motors on the driver channels
  Occupancy.{h,cpp}      occupancy detection from the current sense
  IoPins.{h,cpp}         I/O line logic (PinBank), and expander port A as P0-P7
  IoBoards.{h,cpp}       I/O expander boards on the Qwiic connector J3
  ServoBoards.{h,cpp}    PCA9685 servo and light boards on J3
  I2cBus.{h,cpp}         the I2C bus, for boards that may not be there
  Expander.{h,cpp}       MCP23018 port expander at U3
  WifiLink.{h,cpp}       the bus: CAN, WiFi and USB joined by one hub
  Notice.{h,cpp}         console status messages, held back while USB carries LCC
  DCCSource.{h,cpp}      DCC generation on channel A
  Throttle.{h,cpp}       DCC-EX and WiThrottle on the serial port
  LccTraction.{h,cpp}    LCC traction commands to DCC
software/tools/
  check_cdi.py           checks the CDI's offsets against Config.h
```

The OpenLCB stack — the node, Memory Configuration, the CAN, TCP and serial
transports and NVS storage — is [AOLCB](https://github.com/UncommonModels/AOLCB).

## LCC

The node speaks OpenLCB. It claims an alias, announces itself, answers protocol
queries, produces and consumes events, and supports Simple Node Information,
datagrams and the Memory Configuration protocol with a CDI, so JMRI's *Configure
Nodes* tool can show and edit every setting.

AOLCB implements the CAN link layer: the CID1–4 / RID / AMD alias handshake with
the mandatory 200 ms wait, collision detection and recovery, AME replies, Verify
Node ID, and the consumer/producer identified messages. The frame format was
taken from the OpenLCB CAN Frame Transfer standard and cross-checked against a
reference implementation rather than written from memory.

The node runs on three links at once, joined by an `AOLCB::Hub` that passes
frames between them:

| Link | How |
|---|---|
| CAN | the LCC bus at `J2`, 125 kbit/s, through the MCP2562 |
| WiFi | GridConnect over TCP, port 12021, as a hub or connecting to one |
| USB | GridConnect on the console port, so the board is an LCC-USB adapter |

A JMRI on WiFi or USB therefore sees the whole CAN bus, and the CAN nodes see
JMRI. The node works with no CAN bus at all: a frame nobody acknowledges is
dropped on the CAN side without holding up the others.

## Configuration

Everything that decides what the board is for lives in the LCC configuration
space (memory space 253) and is edited from JMRI:

- **Outputs A–D**: each is a *track block*, a *turnout motor* or *unused*.
  - Block: power-on state (off, DC, DCC), DCC polarity (reversed, for a
    reversing section), occupied and clear thresholds in mA, and its events.
  - Turnout: motor type — *stall* (Tortoise and similar, driven continuously,
    one polarity per position) or *pulse* (two-wire latching motors, driven for
    the pulse length then switched off) — pulse length, drive strength (PWM
    duty, to run a stall motor below the supply voltage), direction, power-on
    position (closed, thrown, or leave alone), and its events.
  - Either use: the fault event.
- **Detection while off**, one entry per output: whether a block that is
  switched off is pulsed so it can still be detected, and the pulse length and
  interval. It belongs with *Track block* and is a group of its own only because
  its bytes are at the end of the space.
- **I/O pins P0–P7** on expander port A: unused, input, input with pull-up, or
  output; polarity; input debounce; and their events.
- **I/O expansion boards 1–4** on the Qwiic connector: chip, I2C address, and 16
  lines set up exactly like the I/O pins (8-line chips use lines 1–8).
- **Servo and light boards 1–2** (PCA9685): address, whether servos keep their
  pulses at rest, and 16 channels, each unused, a servo turnout (closed and
  thrown pulse widths, travel time, power-on position) or a light (brightness,
  fade time, polarity, power-on state), with their events.
- **WiFi**: on/off, network name, password, hostname, hub mode (be a hub, or
  connect to one), hub address and TCP port.
- **USB**: whether LCC traffic is sent to the console port as GridConnect.

### From JMRI

1. Connect JMRI to the layout — over WiFi or USB to this board, or through any
   LCC adapter on the bus. See below.
2. *LCC → Configure Nodes*. The board shows up as *FemtoLCC* by Uncommon
   Models. Select it, *Open Configuration*.
3. Edit, then *Write* a field or *Save Changes*.

### From a web browser

With WiFi up, the board serves the same form at `http://femtolcc-0001.local/`
(or its IP address). The page is built from the board's CDI, so it has exactly
the fields JMRI shows, and it reads and writes the same configuration bytes —
settings apply the same way, 250 ms after the last write. It also has *Reboot*
and *Factory reset* (type the node ID to confirm). Everything is served by the
board; no internet connection is needed.

The web server is AOLCB's `CdiWebServer`. It has no password: anyone on the
layout network can change the settings, as they can from JMRI.

Output, pin and expansion board settings take effect 250 ms after the tool's last
write, with no reboot: the board re-reads the configuration, reconfigures the
channels, pins and boards, re-registers its events and announces them again. An output whose *use*
changed takes its power-on state; the others keep running with their new
settings.

WiFi settings take effect after a reboot. JMRI's *Restart* button (Memory
Configuration Reset/Reboot) does it; the board acknowledges first.

*Factory Reset* in JMRI, or `factory` on the console, puts every setting back to
its default — node name, events and WiFi included — and reboots.

The WiFi password is write-only over LCC. It reads back as blank, and writing a
blank back (which a tool does when saving the whole form) keeps the stored one.

Events can be shared: one event can switch several blocks, and a button's
*Input active* event pasted into a turnout's *Throw* field makes the button throw
it — the board acts on its own events as well as sending them, since the bus
never echoes a node's messages back to it (one level deep, so two settings that
feed each other cannot loop). An event ID of all zeros means none. Only the
events for an output's current
use are registered, and their states are kept up to date, so a panel asking
whether a block is occupied or which way a turnout lies gets a real answer.

### Storage and layout

The configuration space is 4608 bytes, kept in NVS (namespace `femtolcc`) by
`AOLCB::NvsConfigStorage`. It must stay at or under 4608: NVS is 20 KB, and a
commit writes the new copy before dropping the old. The first 128 bytes are the
ACDI user block — node name and description — and application settings start
at 128:

| Offset | Size | Contents |
|---|---|---|
| 0 | 128 | ACDI user block: version byte, name, description |
| 128 | 4 | Layout magic, `FLC` and a layout number; not in the CDI |
| 132 | 4 × 93 | Outputs A–D |
| 504 | 8 × 36 | Pins P0–P7 |
| 792 | 197 | WiFi |
| 989 | 1 | USB |
| 990 | 4 | Expansion magic, `FLX` and a number; not in the CDI |
| 994 | 4 × 578 | I/O expansion boards 1–4: type, address, 16 × 36-byte lines |
| 3306 | 2 × 643 | Servo and light boards 1–2: type, address, hold, 16 × 40-byte channels |
| 4592 | 4 × 4 | Detection while off, for outputs A–D: mode, pulse length, interval |

An expansion line is laid out exactly like a pin, `CFG_PIN_*` offsets and all. A
servo channel is packed into 40 bytes so that both boards fit under 4608: the
power-on state and a light's polarity are part of its *Use*, and travel and fade
time share one field.

Integers and event IDs are big-endian, as configuration tools read them. Every
field's offset is a constant in `Config.h`, and the CDI in `Cdi.cpp` must agree
with them. `software/tools/check_cdi.py` walks the CDI the way a tool does and
compares every field with `Config.h`, and checks the XML is well-formed. Run it
after touching either file:

```
python3 software/tools/check_cdi.py
```

When a firmware update moves a field, bump the layout number in
`CFG_LAYOUT_MAGIC`. A board finding a different magic at boot writes the defaults,
keeping its name and description.

Everything from 990 on came later and has a magic of its own, `CFG_EXP_MAGIC`. A
board updated from a firmware without it loads its old 1024-byte blob into the
start of the space (`NvsConfigStorage` zero-fills the rest), finds the expansion
magic missing, and writes only the expansion defaults (`addExpansionDefaults()`),
printing `expansion board settings added`. Its name, WiFi, outputs and pins are
kept. Going back to an older firmware resets everything, because a stored blob
bigger than the space is not loaded.

The detection fields at 4592 came later still and sit inside that same expansion
area, so a board coming from a firmware older than the expansion magic has their
defaults written with everything else. A board that already has the magic does
not — nothing rewrites an area whose magic is already right — and reads zeros
there. **Zero therefore means "the default" in all three fields**, and the
default for the mode byte is *on*, so an upgraded board starts pulsing its
blocks without being configured. That is also why *on* is 0 and *off* is 1,
the opposite way round to every other setting.

### Events

Defaults are the node ID in the top six bytes with a suffix in the low two, which
is the usual convention and keeps them unique to the board. All of them can be
changed in the configuration.

| Event | Suffix | Direction | Effect |
|---|---|---|---|
| Block *n* on | `0x0100 + n` | consumed | DC drive, full duty |
| Block *n* off | `0x0110 + n` | consumed | Channel idle |
| Block *n* DCC | `0x0120 + n` | consumed | Pass the track signal through, at the configured polarity |
| Output *n* fault | `0x0200 + n` | produced | Driver *n* reported a fault and was shut down |
| Block *n* occupied | `0x0300 + n` | produced | Something is drawing current in block *n* |
| Block *n* clear | `0x0310 + n` | produced | Block *n* is empty |
| Turnout *n* throw | `0x0400 + n` | consumed | Drive the turnout thrown |
| Turnout *n* close | `0x0410 + n` | consumed | Drive the turnout closed |
| Turnout *n* thrown | `0x0420 + n` | produced | Sent once the turnout has been commanded thrown |
| Turnout *n* closed | `0x0430 + n` | produced | Sent once the turnout has been commanded closed |
| Pin *p* active | `0x0500 + p` | produced | Input went active |
| Pin *p* inactive | `0x0510 + p` | produced | Input went inactive |
| Pin *p* on | `0x0600 + p` | consumed | Output on |
| Pin *p* off | `0x0610 + p` | consumed | Output off |
| Expansion line *i* active | `0x0700 + i` | produced | Input went active |
| Expansion line *i* inactive | `0x0800 + i` | produced | Input went inactive |
| Expansion line *i* on | `0x0900 + i` | consumed | Output on |
| Expansion line *i* off | `0x0A00 + i` | consumed | Output off |
| Servo channel *i* throw / light on | `0x0B00 + i` | consumed | Move the servo thrown, or light up |
| Servo channel *i* close / light off | `0x0C00 + i` | consumed | Move the servo closed, or go dark |
| Servo channel *i* thrown | `0x0D00 + i` | produced | Sent when the servo reaches thrown |
| Servo channel *i* closed | `0x0E00 + i` | produced | Sent when the servo reaches closed |

*n* is 0–3 for channels A–D and *p* is 0–7 for `P0`–`P7`. With the default node
ID `02.01.57.00.00.01`, "block A on" is `02.01.57.00.00.01.01.00`. On the
expansion boards *i* is 16 × board + line (or channel), all counted from 0, so
the CDI's board 2, line 3 is `0x12`.

## Turnouts

A turnout channel drives its motor with the H-bridge in DC mode: closed is
forward, thrown is reverse, swapped by the *Direction* setting. A stall motor
stays driven at the configured duty; a pulse motor is driven for the pulse length
and then switched off. There is no position feedback, so *thrown* and *closed*
are produced as soon as the move is commanded. Occupancy detection is off on a
turnout channel.

## I/O pins

Expander port A, `P0`–`P7`. The MCP23018's outputs are open drain: they can only
pull a line low. So the natural wiring is to ground, and levels are named that
way:

- An **input** is active when pulled low — a button or contact to ground, with
  the pull-up mode. Inputs are read every 5 ms and debounced per pin.
- An **output** is on when it pulls low. Wire an LED from the supply through a
  resistor into the pin.

*Inverted* swaps both. Unused pins are inputs with the pull-up on. Output state
survives a configuration change as long as the pin stays an output.

The debounce and event logic is `PinBank`, in `IoPins.h`. `IoPins` drives it
for port A and `IoBoards` for each expansion board, so the expansion lines
behave exactly the same.

## Expansion boards (Qwiic)

`J3` is a 4-pin JST-SH connector wired GND, 3.3V, SDA, SCL: a Qwiic / STEMMA QT
port on the board's I2C bus (GPIO20/21, pulled up to 3.3 V by 2.2k at R2/R3).
The on-board MCP23018 sits on the same bus at `0x20`, so nothing else may use
that address. Everything is 3.3 V; a 5 V board needs its pull-ups removed or a
level shifter, because the ESP32-C6 is not 5 V tolerant.

| Section | Boards | Chip | Lines | Addresses |
|---|---|---|---|---|
| I/O expansion boards | 1–4 | MCP23017 | 16 | `0x21`–`0x27` |
| | | PCF8574 / PCF8574A | 8 | `0x21`–`0x27` / `0x38`–`0x3F` |
| | | TCA9534 / PCA9554 (and their A versions) | 8 | `0x21`–`0x27` / `0x38`–`0x3F` |
| Servo and light boards | 1–2 | PCA9685 | 16 channels | `0x40`–`0x7F` |

An address the chip cannot have, `0x20`, or one already used by an earlier
board is refused: the board is reported and not used. The PCF8574 is a 100 kHz
part, so the bus runs at 400 kHz unless one is configured, and at 100 kHz while
one is. `I2cBus` sets a 10 ms bus timeout, down from the core's 50 ms; a missing
board answers with a NACK at once, so the timeout only matters if something
holds the bus.

**Coming and going.** Each configured board is probed at boot and whenever the
settings are applied. One that does not answer is reported once on the console
(`I/O board 1 (MCP23017 at 0x21) not answering`) and in `status`, and probed
again every 3 s, one board per loop pass; when it answers it is set up and
reported, and its outputs and servos go where they were last commanded. A board
that is answering is set up again every 3 s too — the I/O boards' registers
rewritten, the PCA9685's `MODE1` read back — so one unplugged and plugged back
between checks does not stay reset. Any failed transfer marks a board missing.
While a board is missing, its inputs' and servos' event states are *unknown*.

**I/O lines** work exactly like `P0`–`P7`, with the same levels: an input is
active when pulled low, an output on pulls low, *Inverted* swaps both. Line 1 is
the chip's first pin: `GPA0` to `GPA7` then `GPB0` to `GPB7` on the MCP23017,
`P0` to `P7` on the others. Notes by chip:

- MCP23017: push-pull outputs and 100k pull-ups. Microchip's current datasheet
  makes `GPA7` and `GPB7` (lines 8 and 16) output-only.
- PCF8574: quasi-bidirectional. An output on sinks; anything else is held high
  by a weak current source, which is also every input's pull-up. There is no
  direction register, so every line that is not an output is written 1.
- TCA9534 / PCA9554: push-pull, no pull-ups, so *input with pull-up* acts as
  *input*.

Inputs are read with one transfer per board (two bytes on the MCP23017), one
board per loop pass, round-robin, each at most every 10 ms, and only boards that
have inputs. Output changes are written on the next pass, so several lines
switched by one event go out together.

**Servo turnouts.** The PCA9685 is run at 50 Hz (prescale 121 on its 25 MHz
oscillator), with All Call turned off. A move steps once a 20 ms frame from the
current pulse width to the target over the travel time; a move reversed part-way
takes the time for the distance left. When the width reaches the target the
servo counts as arrived and *thrown* or *closed* is produced. 400 ms later its
pulses stop — full off — so it does not buzz, unless the board's *Servos at rest*
is *Hold position*. Power-on closed or thrown sends it straight there; *left
alone* sends nothing until the first command, which then jumps rather than
ramps, since there is nowhere known to ramp from. Changed positions apply at
once, moving the servo quietly to the new width. To reverse a servo, swap its
two positions. Changes to a channel are written in one transfer spanning the
channels that changed.

**Lights** fade between off and their brightness over the fade time. The
brightness scale is squared, so 128 looks about half as bright as 255. An
inverted light is lit by a low output, for an LED wired from the supply into
the pin. They share the board's 50 Hz frame.

**Servo power.** Servos draw far more than the Qwiic 3.3 V pin can supply, and
stall current spikes would upset the node. Power them from a separate 5–6 V
supply into the servo board's power terminal; the Qwiic cable carries only the
chip's logic supply and the common ground.

Every I2C transfer here happens from the main loop and none of them waits.
The only pause is 500 µs while a PCA9685's oscillator starts, once per board
found.

## WiFi

WiFi is off by default. From the console:

```
wifi MyNetwork my network password
```

The first word is the SSID and the rest of the line the password, so the
password may contain spaces (an SSID with spaces has to be set through the CDI).
This stores both, enables WiFi and joins at once. `wifi` alone shows the status,
`wifi off` disables it. The same settings are in the CDI's *WiFi* group, where
changes apply after a reboot.

Joining never holds up `setup()`: the board comes up on CAN straight away and
prints the address when WiFi connects:

```
WiFi: connected, IP 192.168.1.57, femtolcc-0001.local
LCC hub listening on port 12021
```

The hostname defaults to `femtolcc-` and the low two bytes of the node ID. In
the default hub mode the board listens on port 12021 for up to four GridConnect
clients and advertises itself over mDNS as `<hostname>.local`, service
`_openlcb-can._tcp`. In *connect to a hub* mode it connects out to the configured
address instead and retries every 5 s; give that address as an IP, since a name
needs a DNS lookup that can stall the loop for seconds when it fails.

### Connecting JMRI over the network

*Edit → Preferences → Connections*, add a connection:

- System manufacturer: **LCC** (**OpenLCB** in older JMRI)
- System connection: **CAN via GridConnect Network Interface**
- IP address / host name: `femtolcc-0001.local`, or the IP the console printed
- TCP port: `12021`

## USB as an LCC adapter

The console port carries GridConnect as well as the console, so the board works
as an LCC-USB adapter for JMRI, bridged to the CAN bus and WiFi:

- System manufacturer: **LCC**
- System connection: **CAN via GridConnect**
- Serial port: `/dev/ttyACM0` (a COM port on Windows); any speed, since it is USB

Lines are told apart by their first character: `:` is a GridConnect frame, `<`
DCC-EX, anything else WiThrottle or the console. No console command starts with
`:`. A frame ends at its `;`, so frames sent back to back without newlines are
handled.

What the board sends is set by *USB → GridConnect output* in the CDI, or
`usb gridconnect on|off|auto`:

| Mode | Behaviour |
|---|---|
| Automatic (default) | Output starts off, and turns on when the first valid frame arrives from the computer — which JMRI sends as it connects |
| Always on | Output from power-on |
| Off | Console only; frames from the computer are still accepted |

While output is on, the console's own status messages — occupancy changes,
events acted on, faults, WiFi status — are held back so the host sees frames and
nothing else. Replies to typed commands still print; a GridConnect parser
resynchronises on the next `:`. In automatic mode output stays on until a reboot
or `usb gridconnect auto`, which turns it off again.

Console output is never waited for: `Serial` is given a 4 KB transmit buffer and a
zero timeout, so with nothing reading the port output is dropped rather than
stalling the node.

## Watching it come up

```
FemtoLCC — Uncommon Models, firmware 0.2.0
channels ready, all off
occupancy baseline calibrated
configuration loaded
CAN up at 125000 bit/s on the LCC bus
LCC node starting, claiming an alias
LCC node permitted, alias 0x2A7
```

`permitted` means the alias handshake finished and the node is on the bus. If it
never appears, the node is not seeing its own frames — check the MCP2562 at `U2`,
the bus wiring at `J2`, and that termination is fitted at exactly the two ends of
the bus. On first boot `no configuration stored - defaults written` replaces
`configuration loaded`. The first boot after an update from a firmware without
expansion boards prints `expansion board settings added` first. Each configured
expansion board then reports `answering` or `not answering`.

## Building

`software/Makefile` owns the build. Drive it from here, or from the top-level
Makefile which delegates to it.

```
make -C software deps      # install the ESP32 core, once
make -C software build
make -C software flash PORT=/dev/ttyACM0
make -C software monitor
```

From the repository root the same targets are `make firmware`, `make flash`,
`make monitor` and `make firmware-deps`. `PORT=` and `FQBN=` pass straight
through.

The default `FQBN` is `esp32:esp32:esp32c6:CDCOnBoot=cdc`. The `CDCOnBoot=cdc`
option is what puts `Serial` on USB. Without it the console goes to UART0 on `J1`
and the USB port stays silent.

The C6's native USB port enumerates as `/dev/ttyACM*`, not `/dev/ttyUSB*`. With
more than one board plugged in, pick one by its stable name:
`PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`.

`AOLCB_DIR` points at your AOLCB checkout (default `~/git/AOLCB`) and is dropped
automatically once AOLCB is installed through the library manager.

The core is a large install — roughly 900 MB of downloads and several GB
unpacked, because it carries both the Xtensa and RISC-V toolchains.

Last build: **1171 KB flash (91%), 64 KB RAM (19%)**.

The WiFi stack is most of that. The default partition scheme's 1.25 MB app slot
still has room, but not a great deal of it.

## Serial console

The console is on USB-C at `P1`, through the C6's USB Serial/JTAG port, because
the firmware is built with CDC on boot. The 6-pin UART header at `J1` is UART0.
It carries the boot ROM's messages and can flash the board. It also carries the
console if you build without `CDCOnBoot=cdc`.

115200 baud (any rate works over USB), one command per line:

| Command | Effect |
|---|---|
| `a dc 128` | Channel A, DC drive, duty 128 forward |
| `a dc -128` | Channel A, DC drive, duty 128 reverse |
| `b dcc` | Channel B passes the isolated track signal through |
| `c dcc -` | Channel C, track signal, reversed polarity |
| `d off` | Channel D idle |
| `a throw` / `a close` | Move channel A's turnout (turnout channels only) |
| `stop` | Every channel off |
| `status` | Per-channel use, mode, direction, duty, current, block or turnout state, whether it is pulsed while off, and faults; I/O pin states; each expansion board, whether it answers, and its lines and channels |
| `config` | The stored configuration, readably |
| `detect 5 3` | Set occupied/clear thresholds in mA for all four channels, and store them |
| `pulse` | Whether each output is pulsed while it is switched off, and how |
| `pulse a on` / `pulse a off` | Turn detection while off on or off for one output, and store it |
| `pulse a now` | One detection pulse on channel A, with what it drew in mA — for the bench |
| `calibrate` | Turn everything off and re-measure the zero-current baseline |
| `wifi` | WiFi status: enabled, SSID, connected, IP, hub clients |
| `wifi <ssid> <password>` | Store, enable and join |
| `wifi off` | Disable WiFi |
| `usb` | USB GridConnect mode and whether it is sending |
| `usb gridconnect on\|off\|auto` | Set it |
| `i2c` | Scan the bus, naming what answers, then list the configured expansion boards and whether each answers |
| `x 1 5 on` / `x 1 5 off` | I/O expansion board 1, line 5: switch an output (as its events would). `x1.5 on` works too |
| `x 1 5` | Show that line |
| `servo 1 3 1500` | Servo board 1, channel 3: a raw 1500 µs pulse, held until the next command, for finding positions. `0` stops it |
| `servo 1 3 throw` | Move it as its events would: `throw`, `close`, `on` or `off` |
| `factory` | Factory reset: defaults for everything, then reboot |
| `reboot` | Restart |
| `source on` / `source off` / `source` | Channel A as a DCC source; see below |
| `locos` | Locomotives being refreshed |
| `traction on` / `traction off` | LCC to DCC translation; see below |

Channels are `a`–`d`, matching the silkscreen and the `J5`–`J8` terminal blocks.
The console drives the hardware directly, whatever a channel's configured use.
Expansion boards, lines and servo channels count from 1, as in the CDI, so
servo channel 1 is the output marked 0 on a PCA9685 board.

## Channel A as a DCC source

Channel A can generate DCC instead of passing an external signal through, turning
the board into a small command station.

Only channel A can do this, and the wiring is why: its direction line is on a real
GPIO (`/dir_A`, GPIO4), while B, C and D have theirs behind the MCP23018. An I2C
write per half-bit is nowhere near the 58 µs DCC needs.

The generation itself falls out of the existing multiplexer truth table. With
`dcc_en` low and the PWM line held high:

| `dir` | IN1 | IN2 | Output |
|---|---|---|---|
| 0 | 1 | 0 | OUT1 +, OUT2 − |
| 1 | 0 | 1 | OUT1 −, OUT2 + |

Toggling `dir` at DCC bit timings swings the H-bridge between polarities, which
*is* a DCC signal. A hardware timer at 1 MHz drives it: 58 µs per half-bit for a
one, 100 µs for a zero, per NMRA S-9.1. No extra hardware.

```
source on     channel A becomes a DCC source
source off    release it
locos         list what is being refreshed
```

Speed and function state is held for up to 8 locomotives and re-sent on a
rotation, because a decoder that stops hearing its address eventually times out.
Idle packets fill any gap — DCC is continuously clocked, and stopping the
waveform drops the carrier.

Long (128–10239) and short (1–127) addresses are both supported, chosen
automatically. 128-step speed, functions F0–F12.

## Throttle protocols

Two grammars share the serial port, told apart by the first character of a line:

| First char | Protocol |
|---|---|
| `<` | DCC-EX native command protocol |
| `:` | GridConnect (LCC), see above |
| anything else | WiThrottle |

Anything neither throttle protocol claims falls through to the board's own
console, so `status` and `a dc 128` keep working alongside a throttle.

### DCC-EX

| Command | Effect |
|---|---|
| `<1>` / `<0>` | Track power on / off (starts and stops the DCC source) |
| `<t cab speed dir>` | Throttle; speed 0–126 or −1 for emergency stop, dir 1 forward |
| `<t reg cab speed dir>` | The older four-argument form |
| `<F cab fn 0\|1>` | Set a function |
| `<f cab byte>` | Function group byte, as older clients send |
| `<s>` | Status |
| `<e>` | Forget all locomotives |

### WiThrottle

Enough of the protocol for a throttle to acquire a loco and drive it: the `N`
handshake, `MT+`/`MT-` assignment, `V` speed, `R` direction, `F` functions, `X`
emergency stop, `I` idle, `PPA` power, and heartbeats.

WiThrottle is normally a TCP protocol. This speaks it over the serial line, which
suits a USB or UART bridge; it is not a network server.

## LCC to DCC translation

With `traction on`, the board watches the LCC bus for OpenLCB Traction Control
commands and turns them into DCC packets, so an LCC throttle drives a DCC
locomotive through channel A.

OpenLCB gives every train a node ID, and reserves `06.01.00.00.xx.xx` for DCC
locomotives — the low 14 bits are the DCC address. The board learns which alias
belongs to which node from the AMD frames on the bus, then decodes commands
addressed to any train node in that range:

| Command | Translated to |
|---|---|
| Set speed / direction | 128-step DCC speed packet |
| Set function | DCC function group packet |
| Emergency stop | DCC emergency stop |

Speed arrives as an IEEE half-precision value in metres per second, scaled to
DCC's 0–126 by `LccTraction::setSpeedScale()` — the default assumes 40 m/s at full
throttle.

### Limits

The board **translates** traction commands; it does not yet **host** the train
nodes. A throttle can only address a train node that has announced itself on the
bus, so today this works when something else provides the train node and the
board acts as the DCC output stage. Hosting a node per locomotive needs one alias
each, which means multi-alias support in AOLCB — that is the next step.

## Live control (memory space 0xE0)

A dispatcher — olcbweb's train operations, for one — drives the outputs moment to
moment through memory space `0xE0`, read and written with the standard Memory
Configuration commands. Nothing in it is saved. It is FemtoLCC's own space: the
OpenLCB standards leave the numbers below `0xEF` unassigned. See `LiveControl.h`.

| Offset | Size | Contents |
|---|---|---|
| 0 | 1 | Version, reads 1 |
| 1 | 1 | Lease in seconds, 0 for none |
| 4 + 4n | 4 | Output *n* (A–D): mode (0 off, 1 DC, 2 DCC), reverse, duty (0–255), status (read only: bit 0 fault, 1 occupied, 2 track block, 3 set here) |
| 20 | 1 | DCC source on output A: write 1 to start, 0 to stop |
| 24 | 8 | Locomotive command: address (2), speed (0–126, `0xFF` e-stop), direction (1 forward), functions F0–F28 (4) |
| 32 | 32 | Locomotives being refreshed, 8 × {address (2), speed, flags (bit 0 forward)} |

Only outputs set up as track blocks take a write, and not output A while it is the
DCC source. A write applies each output whose mode, reverse and duty bytes it
covers; the locomotive command must be written as all 8 bytes at once.

**The lease** keeps a crashed dispatcher from leaving trains running. With a lease
set, if nothing writes to the space for that many seconds, every output last set
here is switched off and every locomotive commanded here is stopped. An output
something else has driven since — an event, the console — is left alone. olcbweb
sets a 5 second lease and rewrites its outputs every 2 seconds.

## Occupancy detection

Anything drawing current in a block — a locomotive, a lit car, a resistive
wheelset — shows up on the DRV8874's IPROPI output, which each channel already
feeds to an ADC pin. The firmware turns that into a debounced occupied/clear
state and reports transitions as LCC events, so a signalling panel or dispatcher
can subscribe to them. Only channels configured as track blocks are watched. A
block that is switched off is pulsed briefly so that it can be measured too —
see *While the block is off* below.

### What it can actually see

IPROPI is 455 µA/A into the 1.43 kΩ resistors at R7/R9/R11/R13, giving 0.65 V per
amp. Against a 12-bit 3.3 V ADC that is **1.24 mA per count**:

| Load | Current | ADC counts | Verdict |
|---|---|---|---|
| Running locomotive | ~300 mA | 242 | unmistakable |
| Idle DCC locomotive | ~60 mA | 48 | reliable |
| Lit passenger car | ~20 mA | 16 | reliable |
| 4.7 kΩ resistive wheelset | ~3 mA | 2.4 | marginal |
| 10 kΩ resistive wheelset | ~1.4 mA | 1.1 | **below the noise floor** |

Oversampling (16 reads per sample) and a per-channel baseline tare are what make
the bottom rows workable at all. **Use 4.7 kΩ or lower wheelsets** — 10 kΩ, which
some detectors handle, will not work reliably on this hardware.

### Behaviour

A block is declared occupied after 20 ms above threshold, and released only after
**1 second** below it. The asymmetry is deliberate: the release delay rides over
dirty track and the gaps between wheelsets, so a block does not flicker clear
under a moving train. Separate occupied and clear thresholds, set per channel in
the configuration (default 5 and 3 mA), give hysteresis so a load sitting on the
threshold does not chatter.

A channel that cannot be measured at all — not a track block, or one whose
driver has faulted — reports `Unknown` rather than `Clear`, because absence of
current proves nothing.

### While the block is off

A block with no power on it draws nothing, so there is nothing to measure. The
firmware therefore **pulses** a block that is switched off: full voltage for
2 ms, sampled while it is applied, then off again, every 300 ms. The reading
goes through the same baseline tare, smoothing, hysteresis and debounce as a
powered one, so the block reports *occupied* or *clear* exactly as it would with
power on it, and the currents in the table above are what a pulse sees, since
the block is at full voltage while it is measured.

This is what a dispatcher needs. olcbweb leaves blocks no train owns switched
off; without pulsing those blocks would never report anything, and a DC train
could never be given the next block.

**It cannot move a train.** 2 ms in 300 is 0.7% of the supply — an average of
some 0.08 V from a 12 V supply — and a motor cannot respond to a 2 ms impulse in
any case. What it *can* do is make lit stock flicker: an LED coach is lit for
2 ms each time, which in a dark room some people will see. That is the one
reason to turn pulsing off.

Pulsing is **on by default** for track blocks, because that is what makes a
dispatcher work out of the box. Turn it off per output with *Detection while
off → Detect while off* in the configuration, or `pulse a off` on the console.

No pulse is ever sent to an output that is not a track block, one that already
has power on it, one passing DCC through — there the multiplexer, not the PWM
line, feeds the driver — or one whose driver has faulted. The pulse goes out in
whichever direction the channel was last set to, so repeated pulses cannot jog a
motor back and forth, and it costs no I2C transaction: `dcc_en` and `dir` are
already where they need to be, so only the PWM line moves. The channel's
reported mode, direction and duty do not change, so the live-control space, the
lease and the event states see nothing of it.

The first 300 µs of a pulse are thrown away while the sense chain settles — the
driver takes a few microseconds to turn on, and IPROPI has to charge the 1.43 kΩ
resistor and the ADC input — and the rest is oversampled, which at about 100 µs
an `analogRead` is some seventeen readings, as many as a powered channel's pass
gets. The four outputs are spread across the interval, so only one is ever
pulsing, and the loop is blocked for one pulse at a time: about 2.4 ms including
the settling and the last reading, less per second than the 1.6 ms every 10 ms
that a powered channel's oversampling already costs.

A block with a short in it trips its driver as it is pulsed. The output's
**fault** event is produced, once, and the output is not pulsed again until it
is driven — or `pulse a now` is typed — so a short cannot produce a fault every
300 ms. `status` shows `FAULT` in its *pulse* column meanwhile.

Pulse length and interval are configurable, 1–5 ms and 100–10000 ms. A longer
pulse reads a little more steadily; a shorter one is less visible in lit stock.

### Calibration

`Occupancy::calibrate()` runs at startup with every channel off, measuring each
sense chain's zero-current offset so it can be subtracted from later readings.
It runs before any configured power-on state is applied. Re-run it any time with
`calibrate` on the console, which turns every channel off first.

## How a channel works

Each channel is an SN74HC253 multiplexer feeding a DRV8874 H-bridge. Two control
lines decide what reaches the driver:

| `dcc_en` | `dir` | IN1 | IN2 | Result |
|---|---|---|---|---|
| 0 | 0 | pwm | low | DC drive, forward |
| 0 | 1 | low | pwm | DC drive, reverse |
| 1 | 0 | dcc_p | dcc_n | DCC pass-through |
| 1 | 1 | dcc_n | dcc_p | DCC pass-through, reversed |

That last row is the useful one: a block can present the track signal at either
polarity, which is what a reversing section needs.

`dcc_en` for all four channels, and `dir` for B, C and D, are on the MCP23018.
Channel A's `dir` and all four `pwm` lines are on the ESP32 directly. `Channels`
hides the split — address channels 0–3 and it routes each line correctly, and
batches expander writes so a multi-channel change is one I2C transaction.

## Current sensing and faults

Each DRV8874 reports load current on IPROPI, into the 1.43 kΩ resistors at
R7/R9/R11/R13, read on ADC1. `Channels::currentMilliamps()` converts using the
455 µA/A ratio from the datasheet.

`nFAULT` is open drain and active low — overcurrent, overtemperature or
undervoltage. The main loop watches all four and shuts a channel down on its
first fault rather than letting the driver retry into a short, and produces the
channel's fault event. Clear it with an explicit command once the cause is
fixed.

## Pin map

Every assignment in `board.h` was extracted from the netlist in
`FemtoLCC.kicad_pcb`, not transcribed by hand, and each carries its schematic net
name as a comment. If the board is rerouted, regenerate rather than edit.

| Function | GPIO | Net |
|---|---|---|
| CAN TX / RX | 18 / 19 | `/cantx`, `/canrx` |
| DCC input | 5 | `/dcc_p` |
| I2C SDA / SCL / INT | 20 / 21 / 22 | `/i2c_sda`, `/i2c_scl`, `/I2Cint` |
| Qwiic `J3` | — | 1 GND, 2 +3.3V, 3 `/i2c_sda`, 4 `/i2c_scl` |
| UART TX / RX | 16 / 17 | `/uart_tx`, `/uart_rx` |
| PWM A–D | 8, 10, 11, 15 | `/pwm_A`…`/pwm_D` |
| Sense A–D | 0, 1, 2, 3 | `/sense_A`…`/sense_D` |
| nFault A–D | 6, 7, 9, 23 | `/nFault_A`…`/nFault_D` |
| Dir A | 4 | `/dir_A` |

Dir B–D are MCP23018 GPB5–7; the four `dcc_en` lines are GPB0–3; the status LED
is GPB4. Port A (GPA0–7) is the eight configurable I/O pins, `/P0`–`/P7`. The
bus is pulled up by 2.2k at R2/R3, and the MCP23018 is at `0x20`; everything on
`J3` shares the bus.

## Node ID

`NODE_ID` in the sketch is a placeholder. Replace it with an ID from a range you
own before putting the board on a shared bus — two nodes answering to one ID
will break alias allocation for both. The default event IDs and hostname are
derived from it when the defaults are written, so after changing it run
`factory` (or configure the events by hand).
