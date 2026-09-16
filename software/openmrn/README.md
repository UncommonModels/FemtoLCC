# FemtoLCC firmware — OpenMRN

An alternative firmware for the FemtoLCC board, built on
[OpenMRN](https://github.com/bakerstu/openmrn), the reference implementation of
the OpenLCB/LCC standards. It does the same job as the firmware in
`../FemtoLCC`, which is built on [AOLCB](https://github.com/UncommonModels/AOLCB),
and the two are alternatives: **only one can be on the board at a time, and
settings do not carry over between them.**

This is an ESP-IDF project, not an Arduino sketch. OpenMRN is used as an ESP-IDF
component straight from a checkout; `../FemtoLCC` is built with `arduino-cli`.

```
software/openmrn/
  CMakeLists.txt             the ESP-IDF project
  sdkconfig.defaults         settings common to every target
  sdkconfig.defaults.esp32c6 console setup for the board
  partitions.csv             3 MB app, 896 kB SPIFFS
  Makefile                   wraps idf.py
  sync-drivers.sh            refresh the hardware driver copies, or check them
  drivers.manifest           what was copied, and when
  components/openmrn/        OpenMRN as an ESP-IDF component
  patches/                   the ESP32-C6 fixes; see patches/README.md
  main/
    main.cxx                 node identity, stack, CAN, WiFi, start-up
    config.hxx               the CDI root and the settings segment
    FemtoCdi.hxx             the settings themselves, as CDI groups
    FemtoController.{hxx,cxx}  settings to hardware, events to actions
    Config.h                 setting enums and structs, without AOLCB
    Arduino.h, Wire.h        the Arduino calls the drivers make
    ArduinoCompat.cxx        those calls, over ESP-IDF
    board.h                  copy: the pin map
    Channels.{h,cpp}         copy: the four block driver channels
    Occupancy.{h,cpp}        copy: occupancy from the current sense
    Turnouts.{h,cpp}         copy: turnout motors on the driver channels
    IoPins.{h,cpp}           copy: I/O line logic, and expander port A
    Expander.{h,cpp}         copy: MCP23018 port expander at U3
```

## Why you might choose it

- **It is the reference implementation.** OpenMRN is what the OpenLCB standards
  are written against. If you care that the node's behaviour on the wire is
  exactly right — alias arbitration, datagrams, streams, the Memory
  Configuration protocol, event identify semantics — this is the stack that
  defines it.
- **It has features AOLCB does not.** Train nodes and hosting traction are the
  big ones: OpenMRN ships `TractionTrain`, `TractionThrottle`,
  `TractionCvSpace` and the DCC packet layer, so a board built on it can host
  train nodes properly rather than translating traction commands into DCC as
  `../FemtoLCC/LccTraction.cpp` does. It also has broadcast time, streams and a
  bootloader over CAN. **None of that is wired up in this firmware yet** — it is
  compiled in and available to build on, which is the point of choosing this
  stack.
- **The CDI cannot drift.** OpenMRN's `CDI_GROUP` macros generate the XML and
  fix the byte offsets from one description. The AOLCB firmware has hand-written
  XML in `Cdi.cpp` and a separate layout in `Config.h`, kept in step by
  `tools/check_cdi.py`.

Reasons to prefer `../FemtoLCC` are in the table at the end: it does considerably
more with the board today.

## Status

**Nothing here has been run on hardware.** It compiles and links for the board's
ESP32-C6; every statement below about behaviour is about what the code does, not
about what has been observed. See "What needs testing on hardware".

What is implemented:

- An OpenLCB node on `SimpleCanStack`, with CAN through the ESP32 TWAI
  controller on the pins in `board.h` (GPIO 18 TX, 19 RX) at 125 kbit/s.
- SNIP identity matching the AOLCB firmware — *Uncommon Models / FemtoLCC / v1* —
  with its own software version string, `OpenMRN 0.1.0`, so a configuration tool
  can tell the two apart. The node ID is the same, `02.01.57.00.00.01`, which is
  safe because only one firmware is on the board at a time.
- A CDI built with OpenMRN's `ConfigDef`/`CDI_GROUP` macros covering the same
  ground as the AOLCB CDI: four outputs, each a track block, a turnout motor or
  unused, with their settings and events; the eight I/O pins `P0`–`P7`; and WiFi.
- Settings in the ESP32 file system through OpenMRN's own configuration
  mechanism — a file on SPIFFS exported as memory space 253 — not AOLCB's NVS
  storage. The CDI is generated into SPIFFS at boot and served from space 255,
  and the node name and description live in the ACDI user space as usual.
- Factory reset that writes default event IDs using the same node-ID-plus-suffix
  scheme as the AOLCB firmware, so a layout's event IDs mean the same thing
  under either. It also seeds OpenMRN's own WiFi group, which would otherwise be
  left as zeros on a fresh board: `Esp32WiFiManager` resets its own settings, but
  only for listeners registered when the settings file is created, and it cannot
  be constructed that early because it takes the network name and password as
  constructor arguments — which have to be read out of the file being created.
- Editing an event ID or an output's use takes effect with no reboot, and the
  node re-announces what it produces and consumes so a tool that has already
  queried it does not keep a stale answer.
- Event consumers and producers wired to the hardware: block power on / off /
  DCC, occupancy occupied and clear, turnout throw and close and thrown and
  closed, I/O pin input active and inactive, output on and off, and the driver
  fault event.
- WiFi through `Esp32WiFiManager`, including its hub and uplink behaviour and
  mDNS.

What is **not** implemented, and is in the AOLCB firmware — see the table:
I/O expansion boards and servo boards on the Qwiic connector, DCC generation on
channel A and everything built on it, the USB GridConnect bridge, the serial
console, the CDI web server, and memory spaces `0xE0` and `0xE1`.

### Why not `ConfiguredConsumer` and `ConfiguredProducer`

OpenMRN's GPIO-shaped helpers — `ConfiguredConsumer`, `ConfiguredProducer`,
`MultiConfiguredPC`, `Esp32Gpio` — are the obvious way to wire events to
hardware, and this firmware uses none of them, deliberately:

- A block is a three-state thing (off, DC, DCC) driven by three consumed events;
  those classes model a single on/off bit.
- Occupancy is not a GPIO. It is oversampled current-sense readings with
  hysteresis and asymmetric on/off delays, which `Occupancy.cpp` already does.
- The I/O pins are behind an I2C port expander, so they are not `Gpio` objects
  either, and `IoPins.cpp` already debounces them.
- Whether an output's events mean anything depends on its use, which is itself a
  setting.

`FemtoController` therefore implements `EventHandler` and `ConfigUpdateListener`
directly — the machinery *underneath* those helpers, which is the part that
decides standards conformance. `Esp32Gpio`, `Esp32AdcOneShot` and
`Esp32BootloaderHal` are not used at all; the first two are also among the
OpenMRN files with no ESP32-C6 support.

## ESP32-C6 support

**It builds for the ESP32-C6, with four small patches to OpenMRN.** OpenMRN's
ESP32 support was written for the original ESP32 and the S2, S3, C3, H2 and C2,
and knows nothing about the C6.

The patches are *not* applied to the OpenMRN checkout. Each is a copy of one
file under `patches/overrides/`, which the component definition puts first on
the include path and substitutes into the source list. `patches/README.md`
explains each one; `patches/*.patch` are the diffs. Point `OPENMRN_PATH` at any
checkout and it is left untouched.

### What happened, in order

1. **Unpatched.** ESP-IDF's own components all built. OpenMRN then failed with
   56 errors, every one of them from a single cause: `src/utils/macros.h` picks
   a SoC-specific `rom/ets_sys.h` and has no branch for the C6, so it hits
   `#error Unknown/Unsupported ESP32 variant`, and the `HASSERT`/`DIE` macros
   below it then fail with `'ets_printf' was not declared in this scope`.
   Because nearly every OpenMRN file includes `macros.h`, that one missing
   `#elif` stopped the whole component.

2. **After patch 0001** (add the C6 `ets_sys.h` branch) the count dropped to two
   failing files:
   - `Esp32HardwareTwai.cxx` — the C6 has two TWAI controllers, so ESP-IDF names
     everything `TWAI0_*`/`TWAI1_*` and the unnumbered `PERIPH_TWAI_MODULE`,
     `ETS_TWAI_INTR_SOURCE`, `TWAI_TX_IDX`, `TWAI_RX_IDX`, `TWAI_CLKOUT_IDX` and
     `TWAI_BUS_OFF_ON_IDX` do not exist.
   - `Esp32SocInfo.cxx` — no C6 branch for `rom/rtc.h` or for the reset-reason
     table, giving `'rtc_get_reset_reason' was not declared` and
     `'RESET_REASONS' was not declared`.

3. **After patches 0003 and 0004** the whole stack compiled and linked.

One more problem surfaced that is **not** a C6 issue at all and would happen on
any ESP-IDF target: OpenMRN's `include/` directory has to be on the include
path, and it contains `include/freertos/FreeRTOSConfig.h`, which shadows
ESP-IDF's. ESP-IDF's `esp_task.h` then picks up the wrong one and fails with
`#error please provide the FreeRTOSConfig.h for your target`. Patch 0002 is a
forwarding shim. This is a packaging clash between OpenMRN's `include/` layout
and ESP-IDF 5.x, not a port problem.

### How much work is a proper port?

Small, for what this firmware uses. Patches 0001, 0003 and 0004 are mechanical —
adding a branch to an existing `#if` chain, and one table copied from the ROM
header — and 0003 is written against `SOC_TWAI_CONTROLLER_NUM` so it is correct
on every SoC and could go upstream as is. A day's work would cover them
properly, with the H2 and C2 gaps in the same chains fixed at the same time.

A *complete* C6 port of OpenMRN's ESP32 layer is larger, because two files this
firmware does not use are also unported and are more than name changes:

- `Esp32Gpio.hxx` — its `IS_GPIO_OUTPUT` macro reads `GPIO.enable`/`GPIO.enable1`
  directly, and the C6's `gpio_dev_t` does not have that shape. It also carries
  per-SoC `static_assert`s on valid pin numbers with no C6 case, so the ESP32
  fallbacks would reject several of this board's pins.
- `Esp32AdcOneShot.hxx` — picks the ADC unit from the pin number per SoC and
  falls through to a `#warning` and ADC1 for anything else. That happens to be
  right for the C6, which only has ADC1, but it is a fallback, not support.

Neither is in the way here: this firmware talks to GPIO, ADC, LEDC and I2C
through ESP-IDF directly (`main/ArduinoCompat.cxx`).

### Other targets

`TARGET` selects the target and defaults to `esp32c6`, the board's chip. Each
target gets its own build directory and its own `sdkconfig`, so switching
between them does not force a reconfigure of the other.

**Fallback target.** `make TARGET=esp32 build` builds the same code for the
classic ESP32, and **this was verified: it compiles and links cleanly**, with no
patches needed beyond the four above (0002, the `FreeRTOSConfig.h` shim, is
needed there too — it was never a C6 problem). The image is 1,024,160 bytes.
No extra toolchain install was required, because the Xtensa toolchain was
already present.

The fallback exists to check that nothing has quietly become C6-specific. It is
**not** a firmware you can use: the FemtoLCC board is a C6, and `board.h`'s pin
map — GPIO 18/19 for CAN, 20/21 for I2C, 0–3 for the current sense — does not
correspond to anything on a classic ESP32. It builds; it would not drive a
board.

## Building

### What you need

- **ESP-IDF v5.2** at `~/esp/esp-idf`, or set `IDF_PATH`. This was developed
  against `v5.2-dev-1183-gd2fb7240a7`.
- **The RISC-V toolchain.** `cd ~/esp/esp-idf && ./install.sh esp32c6`.
- **An OpenMRN checkout** at `~/git/openmrn`, or set `OPENMRN_PATH`. This was
  developed against `cc1a8b76`.
- **A network connection the first time.** `mdns` is no longer part of ESP-IDF;
  `main/idf_component.yml` pulls `espressif/mdns` in as a managed component.

If `export.sh` fails with *"pkg_resources cannot be imported"*, the IDF Python
environment has a setuptools too new for ESP-IDF 5.2's dependency check:

```sh
~/.espressif/python_env/idf5.2_py3.12_env/bin/python -m pip install "setuptools<81"
```

### Build

```sh
cd software/openmrn
make build                       # the board: ESP32-C6
make TARGET=esp32 build          # the fallback target
```

`make build` runs, in effect:

```sh
. $IDF_PATH/export.sh
idf.py -B build-esp32c6 -DIDF_TARGET=esp32c6 \
       -DSDKCONFIG=build-esp32c6/sdkconfig \
       -DOPENMRN_PATH=$HOME/git/openmrn reconfigure
ninja -C build-esp32c6 -j2
```

`idf.py` in ESP-IDF 5.2 has no `--jobs` flag, which is why CMake is run through
`idf.py` and ninja is then driven directly. `JOBS` defaults to 2 because OpenMRN
is about 130 translation units and the machine this was written on is short of
memory; raise it if you have the RAM.

Other targets: `make size`, `make size-components`, `make menuconfig`,
`make clean`, `make distclean`, and `make help`.

### Flash

```sh
make flash PORT=/dev/ttyACM0
make monitor PORT=/dev/ttyACM0
```

The console is UART0 — which on the C6 is GPIO 16 and 17, the `J1` header — with
the native USB Serial/JTAG port at `P1` as a secondary console, so output goes to
both. That matches the AOLCB firmware's `CDCOnBoot=cdc`.

> Flashing this **replaces** the AOLCB firmware. The settings do not carry over:
> they are in a different format in a different place (a SPIFFS file here, NVS
> there). Write them down before you switch, and expect to set the board up
> again. Going back is the same in reverse.

### Size

ESP32-C6, from `make size`:

| | |
|---|---|
| Total image | 1,068,872 bytes |
| Flash `.text` | 778,356 |
| Flash `.rodata` | 203,400 |
| D/IRAM used | 110,940 of 452,112 (24.5%) |
| `.data` | 13,068 |
| `.bss` | 27,184 |
| IRAM `.text` | 70,688 |

The app partition is 3 MB, so the image uses about a third of it.

## The hardware driver copies

`board.h`, `Channels`, `Occupancy`, `Turnouts`, `IoPins` and `Expander` in
`main/` are **byte-identical copies** of the files in `../FemtoLCC`. Both
firmwares drive the same board, so they share the code that drives it.

They are copies rather than edits, and they are not modified here at all. That
is what makes the drift check trustworthy:

```sh
./sync-drivers.sh --check    # report which copies differ; exit 1 if any do
./sync-drivers.sh            # copy anything that changed
```

`drivers.manifest` records the SHA-256 of each file as taken, and when.

They compile unchanged because `main/` supplies the three things they expect
that this project does not otherwise have:

- **`Config.h`** — the setting enums and structs (`ChannelRole`, `MotorType`,
  `PinMode`, `ChannelSettings`, `PinSettings`). `Turnouts.h` and `IoPins.h`
  include `"Config.h"`; in `../FemtoLCC` that header is the whole
  configuration-space layout and pulls in AOLCB's `<ConfigStorage.h>`. Here it
  is only the vocabulary — no byte layout, no AOLCB. It keeps the name so the
  copies need no edit.
- **`Arduino.h` and `Wire.h`** — `millis`, `pinMode`, `digitalWrite`,
  `digitalRead`, `analogRead`, `ledcAttach`, `ledcWrite`, and the I2C calls
  `Expander.cpp` makes. `ArduinoCompat.cxx` implements them over ESP-IDF's GPIO,
  ADC oneshot, LEDC and I2C drivers. It is not a general Arduino compatibility
  layer: every function is there because one of those five files calls it.

If a copy ever *has* to diverge, that is a signal to fix one of those three
files instead.

## This firmware and the AOLCB one

| | OpenMRN (this) | AOLCB (`../FemtoLCC`) |
|---|---|---|
| **Stack** | OpenMRN, the reference implementation | AOLCB |
| **Build** | ESP-IDF, `idf.py` | Arduino, `arduino-cli` |
| **Flash image** | 1,068,872 bytes | 1,195,200 bytes |
| **Static RAM** | 12.8 kB `.data` + 26.5 kB `.bss` | larger; the two are not measured the same way |
| **CDI** | generated from `CDI_GROUP` macros, served from SPIFFS | hand-written XML in `Cdi.cpp`, checked against `Config.h` by `tools/check_cdi.py` |
| **Settings live in** | a SPIFFS file, exported as space 253 | NVS flash, exported as space 253 |
| **Outputs A–D** | block / turnout / unused, with events | same |
| **Occupancy** | yes | yes |
| **I/O pins P0–P7** | yes | yes |
| **I/O expansion boards** (Qwiic) | **no** | MCP23017, PCF8574, TCA9534 — up to 4 |
| **Servo and light boards** (PCA9685) | **no** | up to 2, 16 channels each |
| **Space `0xE0`, live control** | **no** | yes — a dispatcher such as olcbweb drives blocks and locos moment to moment, with a lease |
| **Space `0xE1`, module storage** | **no** | yes — 16 kB of layout description held for a dispatcher |
| **WiFi** | `Esp32WiFiManager`: hub or uplink, mDNS | own hub: GridConnect over TCP on 12021, hub or client, mDNS |
| **WiFi password** | readable back over LCC | write-only |
| **USB GridConnect** | **no** — console only | yes, the board works as an LCC-USB adapter |
| **DCC source** | **no** | generated on channel A by toggling `dir_A`; `DCCSource.cpp` |
| **LCC traction** | not wired up, but OpenMRN's `TractionTrain` is compiled in and is the better foundation | traction commands translated to DCC; does not host train nodes |
| **Serial throttle** | **no** | DCC-EX and WiThrottle on the console |
| **Web configuration** | **no** | `CdiWebServer` serves the settings form |
| **Serial console** | **no** | `config`, `status`, `wifi`, `i2c`, `servo`, `factory`, … |

The flash figures are close, but they are not measuring the same thing: the
AOLCB image carries the DCC source, two throttle protocols, the expansion board
drivers and a web server, none of which are here, while this image carries a
fuller standards stack — datagrams, streams, traction, broadcast time — most of
which is not yet used.

## What needs testing on hardware

None of this has been on a board. In rough order of risk:

1. **CAN.** That `Esp32HardwareTwai` on the C6 actually raises and receives
   frames at 125 kbit/s with patch 0003's `TWAI0_*` naming — the patch makes it
   compile, and only the bus can show it works. Check alias allocation against a
   second node.
2. **I2C and the expander.** `ArduinoCompat`'s `Wire` is a reimplementation.
   The repeated-start path in particular — `endTransmission(false)` followed by
   `requestFrom()`, which `Expander::readRegister` relies on — is implemented
   with `i2c_master_write_read_device()` and has never been on a scope.
3. **The ADC and occupancy.** `analogRead` goes through `adc_oneshot` with the
   widest attenuation and 12 bits, which should match what the Arduino core
   gives, but the occupancy thresholds are calibrated in ADC counts against the
   Arduino core's behaviour. The baseline tare and the 5 mA / 3 mA defaults want
   checking against a real block.
4. **PWM.** `ledcAttach` hands out channels in attach order from one timer at
   16 kHz, 8 bits. Check all four blocks and that nothing whines.
5. **The executor and the 5 ms poll.** Everything — events, configuration
   loads, the hardware poll — runs on the OpenMRN executor, so a slow I2C
   transfer stalls the stack. Watch for missed CAN frames with all four channels
   busy.
6. **Configuration from JMRI.** Reading the generated CDI, writing settings, a
   factory reset, and that changed event IDs re-register without a reboot.
7. **WiFi.** Joining a network, the hub and uplink modes, and mDNS.
8. **Turnout pulse timing**, which depends on `millis()` now coming from
   `esp_timer`.
