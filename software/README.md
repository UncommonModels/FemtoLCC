# FemtoLCC firmware

Firmware for the FemtoLCC board: an ESP32-C6 node with four block driver
channels, an isolated DCC input and an LCC bus interface.

```
software/FemtoLCC/
  FemtoLCC.ino           main sketch — serial console, fault handling, CAN bring-up
  board.h                pin map, read out of the KiCad netlist
  Channels.{h,cpp}       the four block driver channels
  Expander.{h,cpp}       MCP23018 port expander at U3
  ESP32CANInterface.{h,cpp}   AOLCB::Interface backed by the ESP32 TWAI controller
```

## LCC

The node speaks OpenLCB over CAN. It claims an alias, announces itself, answers
protocol queries and produces and consumes events, so an LCC throttle, panel or
JMRI will discover it and can switch blocks.

This is built on [AOLCB](https://github.com/UncommonModels/AOLCB), which now
implements the CAN link layer: the CID1–4 / RID / AMD alias handshake with the
mandatory 200 ms wait, collision detection and recovery, AME replies, Verify
Node ID, and the consumer/producer identified messages. The frame format was
taken from the OpenLCB CAN Frame Transfer standard and cross-checked against a
reference implementation rather than written from memory.

### Events

Event IDs are the node ID in the top six bytes with a suffix in the low two,
which is the usual convention and keeps them unique to the board.

| Event | Suffix | Direction | Effect |
|---|---|---|---|
| Channel *n* on | `0x0100 + n` | consumed | Channel *n* to DC drive, full duty |
| Channel *n* off | `0x0110 + n` | consumed | Channel *n* idle |
| Channel *n* DCC | `0x0120 + n` | consumed | Channel *n* passes the track signal |
| Channel *n* fault | `0x0200 + n` | produced | Driver *n* reported a fault |
| Block *n* occupied | `0x0300 + n` | produced | Something is drawing current in block *n* |
| Block *n* clear | `0x0310 + n` | produced | Block *n* is empty |

*n* is 0–3 for channels A–D. With the default node ID `02.01.57.00.00.01`,
"channel A on" is `02.01.57.00.00.01.01.00`.

Add or change these in `FemtoLCC.ino` — register them with `addConsumer()` and
`addProducer()` before `node.begin()`, or the identify replies will not mention
them.

### Watching it come up

```
FemtoLCC — Uncommon Models
channels ready, all off
CAN up at 125000 bit/s on the LCC bus
LCC node starting, claiming an alias
LCC node permitted, alias 0x2A7
```

`permitted` means the alias handshake finished and the node is on the bus. If it
never appears, the node is not seeing its own frames — check the MCP2562 at `U2`,
the bus wiring at `J2`, and that termination is fitted at exactly the two ends of
the bus.

## Building

`software/Makefile` owns the build. Drive it from here, or from the top-level
Makefile which delegates to it.

```
make -C software deps      # install the ESP32 core, once
make -C software build
make -C software flash PORT=/dev/ttyUSB0
make -C software monitor
```

From the repository root the same targets are `make firmware`, `make flash`,
`make monitor` and `make firmware-deps`. `PORT=` and `FQBN=` pass straight
through.

`AOLCB_DIR` points at your AOLCB checkout (default `~/git/AOLCB`) and is dropped
automatically once AOLCB is installed through the library manager.

The core is a large install — roughly 900 MB of downloads and several GB
unpacked, because it carries both the Xtensa and RISC-V toolchains.

Last build: **357 KB flash (27%), 18 KB RAM (5%)**.

The core is a large install — roughly 900 MB of downloads and several GB
unpacked, because it carries both the Xtensa and RISC-V toolchains.

Connect over the 6-pin UART header at `J1`, or over USB-C at `P1`.

## Serial console

115200 baud, one command per line:

| Command | Effect |
|---|---|
| `a dc 128` | Channel A, DC drive, duty 128 forward |
| `a dc -128` | Channel A, DC drive, duty 128 reverse |
| `b dcc` | Channel B passes the isolated track signal through |
| `c dcc -` | Channel C, track signal, reversed polarity |
| `d off` | Channel D idle |
| `stop` | Every channel off |
| `status` | Per-channel mode, direction, duty, current, block state and faults |
| `detect 5 3` | Set occupied/clear thresholds in mA (default 5 / 3) |
| `calibrate` | Turn everything off and re-measure the zero-current baseline |

Channels are `a`–`d`, matching the silkscreen and the `J5`–`J8` terminal blocks.

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
| anything else | WiThrottle |

Anything neither protocol claims falls through to the board's own console, so
`status` and `a dc 128` keep working alongside a throttle.

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

## Occupancy detection

Anything drawing current in a block — a locomotive, a lit car, a resistive
wheelset — shows up on the DRV8874's IPROPI output, which each channel already
feeds to an ADC pin. The firmware turns that into a debounced occupied/clear
state and reports transitions as LCC events, so a signalling panel or dispatcher
can subscribe to them.

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
under a moving train. Separate occupied and clear thresholds give hysteresis so a
load sitting on the threshold does not chatter.

Detection only means anything while the block is energised. A channel that is off
— or in DC mode at zero duty — reports `Unknown` rather than `Clear`, because no
current flows either way and absence of current proves nothing.

### Calibration

`Occupancy::calibrate()` runs at startup with every channel off, measuring each
sense chain's zero-current offset so it can be subtracted from later readings.
**Do it before applying track power.** Re-run it any time with `calibrate` on the
console.

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
first fault rather than letting the driver retry into a short. Clear it with an
explicit command once the cause is fixed.

## Pin map

Every assignment in `board.h` was extracted from the netlist in
`FemtoLCC.kicad_pcb`, not transcribed by hand, and each carries its schematic net
name as a comment. If the board is rerouted, regenerate rather than edit.

| Function | GPIO | Net |
|---|---|---|
| CAN TX / RX | 18 / 19 | `/cantx`, `/canrx` |
| DCC input | 5 | `/dcc_p` |
| I2C SDA / SCL / INT | 20 / 21 / 22 | `/i2c_sda`, `/i2c_scl`, `/I2Cint` |
| UART TX / RX | 16 / 17 | `/uart_tx`, `/uart_rx` |
| PWM A–D | 8, 10, 11, 15 | `/pwm_A`…`/pwm_D` |
| Sense A–D | 0, 1, 2, 3 | `/sense_A`…`/sense_D` |
| nFault A–D | 6, 7, 9, 23 | `/nFault_A`…`/nFault_D` |
| Dir A | 4 | `/dir_A` |

Dir B–D are MCP23018 GPB5–7; the four `dcc_en` lines are GPB0–3; the status LED
is GPB4. Port A (GPA0–7) is uncommitted, brought out as `/P0`–`/P7`.

## Node ID

`NODE_ID` in the sketch is a placeholder. Replace it with an ID from a range you
own before putting the board on a shared bus — two nodes answering to one ID
will break alias allocation for both.
