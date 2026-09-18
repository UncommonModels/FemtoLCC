# FemtoLCC firmware

The firmware for the FemtoLCC board, built on
[OpenMRN](https://github.com/bakerstu/openmrn), the reference implementation of
the OpenLCB/LCC standards.

This is an ESP-IDF project, not an Arduino sketch. OpenMRN is used as an ESP-IDF
component straight from a checkout.

```
software/openmrn/
  CMakeLists.txt             the ESP-IDF project
  sdkconfig.defaults         settings common to every target
  sdkconfig.defaults.esp32c6 console setup for the board
  partitions.csv             3 MB app, 896 kB SPIFFS
  Makefile                   wraps idf.py
  components/openmrn/        OpenMRN as an ESP-IDF component
  openmrn/                   OpenMRN itself: a git submodule, pinned
  patches/                   the ESP32-C6 fixes; see patches/README.md
  main/
    main.cxx                 node identity, stack, CAN, USB, WiFi, start-up
    config.hxx               the CDI root and the settings segment
    FemtoCdi.hxx             the settings themselves, as CDI groups
    FemtoController.{hxx,cxx}  settings to hardware, events to actions
    FemtoBits.{hxx,cxx}      the BitEventInterface adapters
    LiveControl.{hxx,cxx}    memory space 0xE0, for a dispatcher
    ModuleStore.{hxx,cxx}    memory space 0xE1, the module description
    Config.{h,cxx}           setting enums and structs, and driver glue
    Notice.h                 status messages from the drivers
    Arduino.h, Wire.h        the calls the hardware drivers make
    ArduinoCompat.cxx        those calls, over ESP-IDF
    board.h                  the pin map
    Channels.{h,cpp}         the four block driver channels
    Occupancy.{h,cpp}        occupancy from the current sense
    Turnouts.{h,cpp}         turnout motors on the driver channels
    IoPins.{h,cpp}           I/O line logic, and expander port A
    Expander.{h,cpp}         MCP23018 port expander at U3
    I2cBus.{h,cpp}           the I2C bus, for boards that may not be there
    IoBoards.{h,cpp}         I/O expander boards on the Qwiic connector J3
    ServoBoards.{h,cpp}      PCA9685 servo and light boards on J3
    DCCSource.{h,cpp}        DCC generation on channel A
```

## Status

The node ID derivation has been checked on a board — see "Node ID" below, where
getting it wrong was caught by measuring rather than by review. **The rest has
not been run on hardware.** It compiles and links for the board's ESP32-C6;
statements below about behaviour describe what the code does, not what has been
observed. See "What needs testing on hardware".

What is implemented:

- An OpenLCB node on `SimpleCanStack`, with CAN through the ESP32 TWAI
  controller on the pins in `board.h` (GPIO 18 TX, 19 RX) at 125 kbit/s.
- SNIP identity *Uncommon Models / FemtoLCC / v1*, with the firmware version as
  the software version string.
- **A node ID derived from the chip**, so two boards on one bus are different
  nodes without anyone assigning IDs by hand. See "Node ID".
- A CDI built with OpenMRN's `ConfigDef`/`CDI_GROUP` macros: four outputs, each
  a track block, a turnout motor or unused, with their settings and events; the
  eight I/O pins `P0`–`P7`; the Qwiic expansion boards; and WiFi. Because the
  macros generate the XML *and* fix the byte offsets from one description, the
  form a tool draws and the bytes behind it cannot drift apart.
- Settings in the ESP32 file system through OpenMRN's own configuration
  mechanism — a file on SPIFFS exported as memory space 253. The CDI is
  generated into SPIFFS at boot and served from space 255, and the node name and
  description live in the ACDI user space as usual.
- Factory reset that writes default event IDs from the node ID. It also seeds
  OpenMRN's own WiFi group, which would otherwise be left as zeros on a fresh
  board: `Esp32WiFiManager` resets its own settings, but only for listeners
  registered when the settings file is created, and it cannot be constructed
  that early because it takes the network name and password as constructor
  arguments — which have to be read out of the file being created.
- Editing an event ID or an output's use takes effect with no reboot, and the
  node re-announces what it produces and consumes so a tool that has already
  queried it does not keep a stale answer.
- Event consumers and producers wired to the hardware: block power on / off /
  DCC, occupancy occupied and clear, turnout throw and close and thrown and
  closed, I/O pin input active and inactive, output on and off, the expansion
  board lines and servo channels, and the driver fault event.
- **WiFi** through `Esp32WiFiManager`, including its hub and uplink behaviour
  and mDNS. The password is write-only: `main.cxx` wraps memory space 253 so
  those bytes always read back blank.
- **The USB port as a GridConnect bridge**, so the board works as an LCC-USB
  adapter for JMRI. The console is not there; see "Flash" below.
- **Occupancy detection while a block is switched off**, so a block with no
  power on it still reports occupied or clear. The block is pulsed for a couple
  of milliseconds every few hundred and the current is measured during the
  pulse; the four outputs are spread across the interval so only one is ever
  pulsing. The defaults are on, 2 ms and 300 ms, with 0 meaning the default in
  each.
- **Memory space 0xE0, live control**: a dispatcher such as olcbweb drives the
  blocks and locomotives moment to moment, and the lease switches off anything
  it left running if it goes quiet. See "Space 0xE0, live" below.
- **Memory space 0xE1, module storage**: 16 kB of NUL-terminated text held for a
  dispatcher, saved two seconds after the last write.
- **The Qwiic expansion boards** — four I/O boards and two PCA9685 servo/light
  boards — with their settings, events, input transitions, outputs and arrival
  reporting all wired through `FemtoController`.
- **DCC generation on channel A**; see "DCC" below.

What is **not** implemented:

- **Hosting train nodes.** OpenMRN ships `TractionTrain`, `TractionThrottle`
  and `TractionCvSpace`, and they are compiled in, but nothing in this firmware
  constructs them yet. That is the largest piece of open work, and choosing this
  stack is what makes it straightforward: the DCC packet layer to build on is
  already here.
- **A serial console with commands.** UART0 carries boot messages and driver
  status messages, and nothing reads from it. Everything is configured over LCC.
- **A CDI web page.** OpenMRN ships no HTTP server — the only `httpd` in the
  tree belongs to the CC32xx WiFi driver — so serving the settings as a web page
  would mean `esp_http_server` plus a hand-written CDI renderer: a large bypass
  for one feature. It is left out, so the board is configured from JMRI or
  another OpenLCB tool.
- **Throttle protocols on a serial port** (DCC-EX, WiThrottle). Locomotives are
  driven through live control instead.

## Node ID

The node ID is `02.01.57` — Uncommon Models' range — followed by the low three
bytes of the chip's base MAC address. A board whose base MAC is
`98:a3:16:a7:cd:94` is node `02.01.57.A7.CD.94`, and prints that at boot. Every
board is therefore unique as built, with nothing to edit and reflash per board,
and two boards can share a bus without breaking each other's alias allocation. A
board whose eFuse cannot be read falls back to `02.01.57.00.00.01`.

`femto_node_id()` in `main.cxx` is where to change the range if you have your
own.

> **Read the MAC with `esp_read_mac(mac, ESP_MAC_BASE)`, not
> `esp_efuse_mac_get_default()`.** On a part with 802.15.4 — the C6 is one — the
> latter splices the two-byte `MAC_EXT` into the middle and returns the
> eight-byte EUI-64. That overruns a six-byte buffer, and bytes 3 to 5 are then
> the constant `ff:fe` plus one real byte, so every board comes out as
> `02.01.57.FF.FE.xx`. It was caught on hardware, not in review: the board
> announced `02.01.57.FF.FE.A7` where `02.01.57.A7.CD.94` was expected. It is
> the kind of mistake that reads as correct.

The default event IDs and the mDNS host name are derived from the node ID **when
the defaults are written**. A board first configured under a different ID keeps
the events it was given while announcing its new ID — which works, because an
event ID is just an ID, but the two no longer match. A **factory reset**
re-derives both, at the cost of every other setting, and means re-pointing
anything on the layout that used the old event IDs.

## Going through OpenMRN, and where it does not fit

This is meant to be the standards-reference build, so it should look like an
OpenMRN application. Reaching past the library to ESP-IDF is the exception, and
every exception is listed here.

**Through the library:** the stack and node, the CAN driver
(`Esp32HardwareTwai`), WiFi (`Esp32WiFiManager`), the configuration and its
update machinery, both custom memory spaces (`openlcb::MemorySpace`), and — since
patch 0005 — this board's directly-attached pins, which are now real
`Esp32Gpio` objects with the library's own safe power-on level and its
compile-time pin checks.

**The event layer** goes through `BitEventInterface` and OpenMRN's
`BitEventProducer` / `BitEventConsumer` wherever the hardware is genuinely one
bit with one pair of events. The test is simply "is it one bit?".

**Nine of this node's eleven event species go through the library; two stay
hand-rolled.** The nine are block occupancy, a turnout's command and its
position, an I/O pin in and out, an expansion board line in and out, and a servo
channel's command and its arrival. `FemtoBits.hxx` holds the adapters, and the
value of the change is that the library answers the identify queries — the part
that is easiest to get subtly wrong by hand.

Occupancy is the one that had to be checked rather than assumed, because a block
that is not energised and not being pulsed proves nothing, and reporting that as
"clear" would be a lie a dispatcher would act on. It survives: `EventState` is
three-valued, `invert_event_state()` leaves `UNKNOWN` alone, and the reply MTI is
`MTI_PRODUCER_IDENTIFIED_VALID + state`, so an unmeasurable block answers
"unknown" on *both* its events.

A turnout is modelled as *two* bits, not one, because the events it is driven
by and the ones it reports are different IDs: `throw`/`close` are consumed and
`thrown`/`closed` are produced, and one `BitEventInterface` carries one pair.

Two things stay on this firmware's own event handler:

- **A block's power** is three mutually exclusive states — off, DC, DCC — over
  three separate consumed events. A bit is two states, and three
  `BitEventConsumer`s over one output would each answer an identify query
  without knowing about the other two, so a tool would see more than one of them
  claim to be valid at once. It is also where live control (space `0xE0`) calls
  `channel_changed()` to keep the reported state truthful, which has no
  equivalent in the bit model.
- **A driver fault** is not a state at all: it reports that something happened,
  and there is no "unfaulted" event to pair it with.

**Where the hardware lock is taken, and why.** The hardware poll runs on its own
thread (see `FemtoController.hxx`), so one recursive mutex guards every access to
the drivers:

1. *The poll itself* holds it for the whole of each pass — occupancy, faults,
   pins, the expansion boards, turnouts, the `0xE0` lease and the `0xE1` commit.
2. *Everything on the stack's executor* takes it before touching a driver: a
   consumed event, a configuration load, an identify reply, the two custom
   memory spaces, and the bit adapters' `get_current_state()` and `set_state()`,
   which are called from OpenMRN's own handlers.

It is recursive because live control holds it and calls back into
`channel_changed()`. The hardware thread never sends an LCC message: a producer
bit is marked as changed and the 33 Hz refresh loop, which runs on the stack's
executor, does the sending.

**The exceptions, and why:**

- **The block drivers.** Each output is an SN74HC253 multiplexer feeding a
  DRV8874, so "set this output" means driving a PWM line, a direction line and a
  DCC-enable line, two of which sit behind an I2C port expander. No GPIO
  abstraction describes that, so `Channels`, `Occupancy` and `Turnouts` drive the
  hardware directly.
- **The current sense** still uses ESP-IDF's `adc_oneshot` directly rather than
  `Esp32AdcOneShot`. The reason was a defect in the header, not the C6:
  `Esp32ADCInput::hw_init()` called `adc_oneshot_new_unit()` once per pin, so the
  second of two pins on one ADC unit failed — and this board has four on ADC1.
  **At the pinned baseline that is fixed**: upstream now has the
  `Esp32ADCUnitManager` that shares one handle per unit, and it picks `ADC_UNIT_1`
  on the C6 without a patch. So this bypass is no longer justified by the
  library, and moving the current sense onto `Esp32AdcOneShot` is outstanding
  work rather than a decision — it would need `Esp32AdcUnitManager.cxx` adding to
  the component's source list, and the occupancy thresholds are calibrated in raw
  ADC counts, so it wants a board in front of you.
- **The Arduino calls** the hardware drivers make are indexed at run time
  (`PIN_NFAULT[i]`), while `Esp32Gpio` is a compile-time template. `Arduino.h`
  dispatches this board's known pins to the `Esp32Gpio` objects and falls back to
  `gpio_config()` for anything else.
- **The event layer** is still one `EventHandler` of this firmware's own rather
  than `ConfiguredConsumer`/`ConfiguredProducer`/`RefreshLoop`, because whether
  an output's events mean anything depends on its use, which is itself a
  setting. `FemtoController` therefore implements `EventHandler` and
  `ConfigUpdateListener` directly — the machinery *underneath* those helpers.
  Reworking the parts that do fit onto the standard classes is outstanding work,
  not a settled decision.

## Why the hardware drivers look like Arduino code

`Channels`, `Occupancy`, `Turnouts`, `IoPins`, `Expander`, `I2cBus`, `IoBoards`,
`ServoBoards` and `DCCSource` call `millis()`, `pinMode()`, `digitalWrite()`,
`analogRead()`, `ledcAttach()` and `Wire`. That is the API they were first
written for, and they are working, tested code that drives real hardware
correctly.

Rather than rewrite them against ESP-IDF — a change with real risk and no
functional gain — `main/` supplies the narrow API they expect:

- **`Arduino.h` and `Wire.h`** declare exactly the calls those files make:
  `millis`, `pinMode`, `digitalWrite`, `digitalRead`, `analogRead`,
  `ledcAttach`, `ledcWrite`, the hardware timer `DCCSource` drives its waveform
  from, and the I2C calls. `ArduinoCompat.cxx` implements them over ESP-IDF's
  GPIO, ADC oneshot, LEDC, `gptimer` and I2C drivers. It is not a general Arduino
  compatibility layer: every function is there because one of those files calls
  it.
- **`Config.h`** is the setting vocabulary (`ChannelRole`, `MotorType`,
  `PinMode`, `ChannelSettings`, `PinSettings`) that `Turnouts.h` and `IoPins.h`
  include. It describes no byte layout — the settings live in OpenMRN's
  configuration file — but the enum *numbers* are load-bearing, because
  `FemtoCdi.hxx` renders them as the drop-down entries a tool shows and a saved
  setting is stored as the number.

These are ordinary sources of this firmware: edit them here, and add to
`Arduino.h` and `ArduinoCompat.cxx` when a driver needs a call that is not there
yet.

## ESP32-C6 support

**It builds for the ESP32-C6, with six small patches to OpenMRN.** OpenMRN's
ESP32 support was written for the original ESP32 and the S2, S3, C3, H2 and C2,
and knows nothing about the C6.

The patches are *not* applied to the OpenMRN checkout. Each is a copy of one
file under `patches/overrides/`, which the component definition puts first on
the include path and substitutes into the source list. `patches/README.md`
explains each one; `patches/*.patch` are the diffs. Point `OPENMRN_PATH` at any
checkout and it is left untouched.

They are **0001** (`macros.h`), **0002** (the `FreeRTOSConfig.h` shim, which is
not a C6 problem), **0003** (TWAI peripheral naming), **0004** (`Esp32SocInfo`
reset reasons), **0005** (`Esp32Gpio`) and **0007** (`Esp32WiFiManager`'s
`rom/crc.h`). 0006 has been retired: upstream now chooses the ADC unit for the
C6 itself, and ships the shared-unit manager that made `Esp32AdcOneShot` usable
at all.

Because an override *replaces* whatever upstream has at that path, it is only
correct against one revision, which is why `openmrn/` is a pinned submodule.
The overrides are written against **`bd82a644`**. They were previously taken
from `cc1a8b76` in a developer's own checkout; when that checkout moved the
overrides went on shadowing files that had changed underneath them, and the
build broke in two unrelated places. Moving the baseline is now a deliberate act
with a diff to read: `patches/README.md` has the procedure.

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

Small, for what this firmware uses. Patches 0001, 0003, 0004 and 0007 are
mechanical — adding a branch to an existing `#if` chain, and one table copied
from the ROM header — and 0003 is written against `SOC_TWAI_CONTROLLER_NUM` so
it is correct on every SoC and could go upstream as is. A day's work would cover
them properly, with the H2 and C2 gaps in the same chains fixed at the same
time.

Patch 0005 takes that further, into a file a board actually touches:

- `Esp32Gpio.hxx` needed three fixes. `IS_GPIO_OUTPUT` reads the output-enable
  register directly, and the C6's union names its member `val` where the C3's
  names it `data`. The pin-range `static_assert`s fell through to the ESP32
  branch, which reserves GPIO6-11 for flash — on the C6 those are ordinary pins
  and this board uses three of them for nFAULT (the C6's flash is on GPIO24-30).
  And the pull-down check reasons about ESP32 strapping pins. With the patch,
  this board's pins are real `Esp32Gpio` objects.

`Esp32AdcOneShot.hxx` used to need one too — it chose the ADC unit with an `#if`
chain that had no `#else`, so on the C6 `.unit_id` was left out of the
initialiser and zero-initialised to `ADC_UNIT_1`, correct by accident and
silently — and it had a worse defect that was not C6-specific:
`Esp32ADCInput::hw_init()` created a new ADC unit per pin, so it could not drive
more than one pin on a unit. **Upstream has fixed both**, at the pinned
baseline: there is an explicit `#else` and an `Esp32ADCUnitManager` that shares
one handle per unit. That patch (0006) is retired, and the current-sense bypass
above is now a thing to undo rather than a thing to justify.

### Other targets

`TARGET` selects the target and defaults to `esp32c6`, the board's chip. Each
target gets its own build directory and its own `sdkconfig`, so switching
between them does not force a reconfigure of the other.

**Fallback target.** `make TARGET=esp32 build` builds the same code for the
classic ESP32, and **this was verified: it compiles and links cleanly**, with no
patches needed beyond the six above (0002, the `FreeRTOSConfig.h` shim, is
needed there too — it was never a C6 problem; 0001, 0003, 0004, 0005 and 0007
are all guarded on the target and do nothing here).

It has earned its keep twice now. The second catch was the USB GridConnect
bridge: `add_gridconnect_port()` does `::open()` followed by `HASSERT(fd >= 0)`,
and a classic ESP32 has no USB Serial/JTAG, so the call would have aborted the
node at boot rather than merely lacking a bridge. It is now behind
`#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED`.

Keeping it building cost one guard. `ArduinoCompat.cxx` uses `Esp32Gpio`'s
`GPIO_PIN` for the direction and nFAULT lines, and three of those are GPIO 6, 7
and 9 — which on a classic ESP32 are the flash, and which `Esp32Gpio`'s own
`static_assert` rightly refuses. The `GPIO_PIN` block is therefore behind
`#if CONFIG_IDF_TARGET_ESP32C6`, and the fallback uses the plain `gpio_config()`
path. This is a good example of the fallback earning its keep: the assert fired
at compile time on a pin map that was never meant to leave the C6.

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
- **OpenMRN**, which comes with the repository as a pinned submodule at
  `software/openmrn/openmrn`. One line fetches it:

  ```sh
  git submodule update --init --recursive
  ```

  It is pinned to [`UncommonModels/openmrn`](https://github.com/UncommonModels/openmrn)
  at **`bd82a644`**, which is the revision the overrides in `patches/` are
  written against — see "ESP32-C6 support" above. Set `OPENMRN_PATH` to build
  against a checkout of your own instead; the patches are copies, so expect to
  re-base them if you do.
- **A network connection the first time.** `mdns` is no longer part of ESP-IDF;
  `main/idf_component.yml` pulls `espressif/mdns` in as a managed component.

`make deps` fetches the submodule and checks that `IDF_PATH` has an ESP-IDF in
it, printing the `install.sh` line if the toolchain is missing. It does not
install ESP-IDF, which is a large shared toolchain that belongs outside the
repository.

If `export.sh` fails with *"pkg_resources cannot be imported"*, the IDF Python
environment has a setuptools too new for ESP-IDF 5.2's dependency check:

```sh
~/.espressif/python_env/idf5.2_py3.12_env/bin/python -m pip install "setuptools<81"
```

### Build

```sh
cd software/openmrn
make deps                        # once: the submodule, and an IDF check
make build                       # the board: ESP32-C6
make TARGET=esp32 build          # the fallback target
```

From the top of the repository, `make firmware`, `make flash`, `make monitor`
and `make firmware-deps` delegate here; `PORT=` and `TARGET=` pass through.

`make build` runs, in effect:

```sh
. $IDF_PATH/export.sh
idf.py -B build-esp32c6 -DIDF_TARGET=esp32c6 \
       -DSDKCONFIG=build-esp32c6/sdkconfig \
       -DOPENMRN_PATH=$PWD/openmrn reconfigure
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
make flash PORT=/dev/ttyACM0        # the USB-C port at P1
make monitor PORT=/dev/ttyACMx      # a USB-serial adapter on J1, NOT P1
```

**The console is UART0 only** — which on the C6 is GPIO 16 and 17, the `J1`
header. The native USB Serial/JTAG port at `P1` carries **GridConnect**, not
console output: `add_gridconnect_port()` opens the device `O_RDWR` and owns it,
so it cannot also be a console. Flashing still works over `P1`, because the ROM
bootloader answers there.

Nothing reads from the console. It carries the boot banner — including the node
ID — and the drivers' status messages, and that is all; every setting is changed
over LCC.

### Size

ESP32-C6, from `make size`:

| | |
|---|---|
| Total image | 1,102,742 bytes |
| Flash `.text` | 810,770 |
| Flash `.rodata` | 204,416 |
| D/IRAM used | 118,940 of 452,112 (26.3%) |
| `.data` | 13,084 |
| `.bss` | 34,744 |
| IRAM `.text` | 71,112 |

The app partition is 3 MB, so the image uses about a third of it.

## DCC: the fifth bypass, and the flash interlock

Channel A can be a DCC source. This is the one place where the library was tried
and set aside, so the reasoning is recorded here.

**Why not OpenMRN's `dcc/` stack.** It is complete on the logical side —
`dcc::Packet`, `SimpleUpdateLoop`, `PacketSource`/`TrainImpl`, `LocalTrackIf` —
and it is what an LCC traction node should be built on. But `LocalTrackIf` writes
packets synchronously to a **file descriptor** for a device (`/dev/mainline`)
that must support `select()`, and `DccOutput` is a compile-time `HW` template
expecting real hardware hooks. There is no such device driver for the ESP32
anywhere in `freertos_drivers/esp32/` — no DCC, no RailCom, no RMT waveform
generator. "Use OpenMRN's DCC stack" therefore means "write the C6 track-output
driver first", which is a larger and different job.

**What is here instead.** `DCCSource` drives the waveform from a hardware timer,
and `Arduino.h` declares the timer calls it wants — `timerBegin`, `timerAlarm`,
`timerWrite`, `timerAttachInterrupt`, `timerDetachInterrupt`, `timerEnd` and
`hw_timer_t` — over ESP-IDF's `gptimer`. The mapping is close to one for one:
`timerBegin(1000000)` is a timer counting at 1 MHz, and
`timerAlarm(t, half, false, 0)` is a one-shot alarm `half` microseconds out,
because the waveform re-arms the alarm on every half-bit — a '1' is 58 µs and a
'0' is 100 µs.

### The interlock, and what a dispatcher sees

**The hazard.** Writing flash disables the flash cache while each sector is
erased and written, for tens of milliseconds. Code that is not resident in RAM
cannot execute during that window. The DCC timer interrupt is not resident:
`CONFIG_GPTIMER_ISR_IRAM_SAFE` requires the callback *and everything it reaches*
to be in RAM, and this one reaches static helpers and `memcpy` inside
`DCCSource.cpp`, which is not annotated for IRAM. An interrupt during a flash
write is therefore a crash — and one that presents as a random hang, not an
obvious fault. (Annotating `DCCSource.cpp` would lift the restriction; it is
open work rather than something the design forbids.)

The only thing in this firmware that writes flash while running is
`ModuleStore::commit()`, saving the module description for space `0xE1`.

**The decision: the commit is deferred, not the source paused.** While the DCC
source is running the description is held in RAM; it is written as soon as the
source stops, which `end()` makes happen promptly. Pausing the source around the
commit was the obvious alternative and is worse on both counts: the waveform
would stop for the length of the write regardless, because the interrupt cannot
run with the cache disabled, so pausing buys nothing — and stopping the
waveform mid-bit leaves the H-bridge parked at one polarity, which is DC across
a track that is supposed to be carrying DCC.

**What a dispatcher writing `0xE1` mid-session actually gets:** the write
succeeds immediately and reads back immediately, because the space is served out
of RAM. Only the flash write waits. Nothing blocks, nothing errors, and nothing
hangs. The one consequence worth knowing is that a board losing power while the
DCC source is still running loses a description written during that session —
which is why the description is normally written at setup, before trains run.
`ModuleStore::commit()` carries the same warning in its header for whoever adds
the next caller.

### Space `0xE0`, live

The DCC bytes documented in `LiveControl.hxx`:

- **byte 20** starts and stops the source, and reads back 1 while it runs;
- **bytes 24-31** are a locomotive command — address, speed (`0xFF` is
  emergency stop), direction, and F0-F28 as a bit mask — and only what changed
  is sent, so holding a throttle still does not flood the bus;
- **bytes 32-63** read back the locomotives the source is refreshing.

Channel A stops being live control's to drive while the source owns it, and if
the dispatcher's lease runs out with locomotives running, they are sent an
emergency stop rather than left going.

## What needs testing on hardware

Only the node ID derivation has been checked on a board. In rough order of risk:

1. **CAN.** That `Esp32HardwareTwai` on the C6 actually raises and receives
   frames at 125 kbit/s with patch 0003's `TWAI0_*` naming — the patch makes it
   compile, and only the bus can show it works. Check alias allocation against a
   second node.
2. **I2C and the expander.** `ArduinoCompat`'s `Wire` is a reimplementation.
   The repeated-start path in particular — `endTransmission(false)` followed by
   `requestFrom()`, which `Expander::readRegister` relies on — is implemented
   with `i2c_master_write_read_device()` and has never been on a scope.
3. **The ADC and occupancy.** `analogRead` goes through `adc_oneshot` with the
   widest attenuation and 12 bits. The occupancy thresholds are calibrated in
   ADC counts, so the baseline tare and the 5 mA / 3 mA defaults want checking
   against a real block.
4. **PWM.** `ledcAttach` hands out channels in attach order from one timer at
   16 kHz, 8 bits. Check all four blocks and that nothing whines.
5. **The USB GridConnect bridge.** That the device registers, that JMRI sees the
   CAN bus through it, and that nothing writes console output into the frame
   stream.
6. **The hardware thread and its lock.** The poll runs on its own thread
   (`femto_hw`) and everything that reaches the drivers takes one recursive
   mutex first. What wants watching is the hand-off rather than the stalls: that
   a consumed event, a live-control datagram and the poll never interleave
   badly, and that nothing deadlocks.
7. **Pulsed occupancy timing.** `Occupancy::pulseAndRead()` energises the block
   and samples the ADC in a loop until the pulse deadline, so it blocks its own
   thread for the length of the pulse — about 2 ms at the default setting (the
   300 µs settle is *inside* the pulse, not added to it), once per block per
   pulse interval, which defaults to 300 ms. With four blocks pulsing that is
   about 13 pulses a second, under 3% of wall-clock time. It no longer runs on
   the stack's executor, so it does not delay CAN; what it does affect is how
   promptly the rest of the hardware in the same poll is serviced. Check that
   turnout pulse timing and I/O pin debounce still feel right with pulsed
   detection enabled on several blocks.
8. **The DCC source**, its waveform on a scope, and the flash interlock: that
   writing space `0xE1` while the source runs does not hang the board.
9. **Configuration from JMRI.** Reading the generated CDI, writing settings, a
   factory reset, and that changed event IDs re-register without a reboot.
10. **WiFi.** Joining a network, the hub and uplink modes, and mDNS.
11. **Turnout pulse timing**, which depends on `millis()` now coming from
    `esp_timer`.
