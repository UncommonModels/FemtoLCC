# Patches to OpenMRN

OpenMRN does not know about the ESP32-C6. Its ESP32 support was written for the
original ESP32 and the S2, S3, C3, H2 and C2, and the C6 differs from all of
them in ways that stop it compiling. These are the changes that fix that.

**Nothing in the OpenMRN checkout is modified.** Each patch is a copy of one
OpenMRN file with a small change, kept in `overrides/` under the same path it
has in OpenMRN's `src/`. `../components/openmrn/CMakeLists.txt` puts
`overrides/` first on the include path, so a patched header shadows the
original, and substitutes the patched path into the source list for a patched
`.cxx`. Point `OPENMRN_PATH` at any checkout and it is left untouched.

The `.patch` files beside this one are `diff -u` between the original and the
override, for review and for sending upstream. They are generated, not applied:
the overrides are the real artefact.

## The baseline

The overrides are copies, so they are only correct against one revision of
OpenMRN. That revision is now **pinned**: `../openmrn` is a git submodule of
[`UncommonModels/openmrn`](https://github.com/UncommonModels/openmrn) at
**`bd82a644`** ("Merge branch 'bakerstu:master' into master", 17 September
2026), and `OPENMRN_PATH` defaults to it.

This is what the submodule is for. The previous baseline was `cc1a8b76`
(2 February 2025) in a developer's own moving checkout; when that checkout was
updated the overrides silently went on shadowing files that had moved on
underneath them, and the build broke in two places at once — a format string
upstream had already fixed, and an enum upstream had renamed. An override
replaces whatever upstream now has, so it is only ever as good as the commit it
was taken from. Pinning makes moving the baseline a deliberate act with a diff
to read.

## The patches

### 0001 — `utils/macros.h`: ESP32-C6 `ets_sys.h`

*C6-specific. Small and clearly upstreamable.*

`macros.h` picks the SoC-specific `rom/ets_sys.h` from `CONFIG_IDF_TARGET_*` and
ends the chain with

```
#else
#error Unknown/Unsupported ESP32 variant.
#endif
```

There is no branch for the C6, so every translation unit that includes
`macros.h` — which is nearly all of OpenMRN — fails, first with that `#error`
and then with a flood of `'ets_printf' was not declared in this scope` from the
`HASSERT` and `DIE` macros below it. This was the entire unpatched failure: 56
errors from one missing `#elif`.

The patch adds branches for the C6, and for the H2 and C2 which were missing the
same include (they appear in `Esp32SocInfo.hxx`'s equivalent chain but not
here). The headers exist in ESP-IDF already.

### 0002 — `include/freertos/FreeRTOSConfig.h`: stop it shadowing ESP-IDF's

*Not C6-specific. An OpenMRN/ESP-IDF packaging clash; would happen on any
target.*

OpenMRN's `include/` directory must be on the include path so its sources can
find `openmrn_features.h`, `can_frame.h` and `nmranet_config.h`. It also
contains `include/freertos/FreeRTOSConfig.h`, a configuration for bare-metal
FreeRTOS ports.

CMake puts a component's own include directories ahead of those of the
components it requires, so when ESP-IDF's `esp_task.h` asks for
`<freertos/FreeRTOSConfig.h>` it gets OpenMRN's rather than ESP-IDF's. That file
has no branch for an ESP-IDF target, so it fails with `#error please provide the
FreeRTOSConfig.h for your target`, and then redefines `configTICK_RATE_HZ` and
`configUSE_IDLE_HOOK` over ESP-IDF's real values.

The override sits earlier still on the include path and forwards to ESP-IDF's
real one, which is reachable unqualified because ESP-IDF also puts
`components/freertos/config/include/freertos` on the path.

A cleaner upstream fix would be for OpenMRN's ESP-IDF component not to expose
`include/` wholesale. That is a packaging change, not a one-line one, which is
why it is handled here instead.

Upstream has not touched this file since `cc1a8b76`, so the override carries
over unchanged.

### 0003 — `freertos_drivers/esp32/Esp32HardwareTwai.cxx`: TWAI peripheral naming

*C6-specific in effect, but written so it is general. Upstreamable as is.*

SoCs with one TWAI controller call its peripheral, interrupt and GPIO matrix
signals `PERIPH_TWAI_MODULE`, `ETS_TWAI_INTR_SOURCE`, `TWAI_TX_IDX`,
`TWAI_RX_IDX`, `TWAI_CLKOUT_IDX` and `TWAI_BUS_OFF_ON_IDX`. The C6 has two
(`SOC_TWAI_CONTROLLER_NUM == 2`), so ESP-IDF numbers them all `TWAI0_*` and
`TWAI1_*` and the unnumbered names do not exist. Every use in the driver fails
to compile; the compiler unhelpfully suggests `TWAIN_*`, which is the C6's
image-sensor interface.

The driver only ever drives the first controller, so the patch defines
`OPENMRN_*` aliases selected on `SOC_TWAI_CONTROLLER_NUM` and uses those. On a
single-controller SoC the generated code is identical.

**Re-based onto `bd82a644`.** Upstream rewrote a good deal of this file between
the two baselines — an error-passive transmit path, a `tx_lost` statistic, and
every statistics format string changed from `%" PRIu32` to `%zu` to match the
`size_t` counters. The stale override was still carrying the old format strings,
which is what broke the build. The override is now the `bd82a644` file with only
the alias block and its six call sites changed.

### 0004 — `freertos_drivers/esp32/Esp32SocInfo.{hxx,cxx}`: C6 reset reasons

*C6-specific. Mechanical; upstreamable.*

`Esp32SocInfo.hxx` selects the SoC's `rom/rtc.h` the same way `macros.h` selects
`ets_sys.h`, and has no C6 branch — but unlike `macros.h` it ends with a plain
`#endif` rather than an `#error`, so the failure surfaces later as
`'rtc_get_reset_reason' was not declared in this scope`.

`Esp32SocInfo.cxx` likewise has a `RESET_REASONS[]` table per SoC and none for
the C6, giving `'RESET_REASONS' was not declared in this scope`.

The patch adds the C6 `rom/rtc.h` include and a `RESET_REASONS[]` table built
from `components/esp_rom/include/esp32c6/rom/rtc.h`, and extends `CHIP_NAMES[]`
with entry 13, `CHIP_ESP32C6` — without which the C6 would print its own model
as "Unknown" — and, while there, the placeholders up to entry 16, `ESP32-H2`.

Neither file changed upstream between the two baselines.

### 0005 — `freertos_drivers/esp32/Esp32Gpio.hxx`: ESP32-C6 support

*C6-specific. Three separate gaps; all mechanical and upstreamable.*

Without this the ESP32 fallback branches apply to the C6 and are wrong in ways
that reject perfectly good pins:

1. `IS_GPIO_OUTPUT` reads the GPIO output-enable register directly, because
   ESP-IDF exposes no `gpio_get_direction()`. The C3 branch uses
   `GPIO.enable.data`; the C6's `gpio_enable_reg_t` union names that member
   `val`, and the fallback also consults `GPIO.enable1`, which on the C6 covers
   GPIO32-34 and so never applies (the part has GPIO0-30). This one is a hard
   compile error on the C6, not merely a wrong answer.
2. The pin-range `static_assert`s have no C6 branch. On `bd82a644` they no
   longer fall through to the ESP32's map — upstream restored the
   `#elif CONFIG_IDF_TARGET_ESP32` guard — so an un-patched C6 build gets no
   range check at all. The patch supplies the right one: GPIO0-30, with GPIO24-30
   reserved for the in-package SPI flash (`SPI_CS0` 24, `SPI_Q` 25, `SPI_WP` 26,
   `SPI_HD` 28, `SPI_CLK` 29, `SPI_D` 30). This board uses GPIO6, 7 and 9 for
   `nFault_A`, `nFault_B` and `nFault_C`, which the ESP32 branch would have
   refused as flash pins.
3. The `GpioInputPin` pull-down `static_assert` likewise has no C6 branch. The
   C6's strapping pin with a pull-up is GPIO9, the same as the C3's, so the
   patch simply extends the C3 branch to cover both.

**Re-based onto `bd82a644`.** Upstream fixed three things in this file that the
stale override was quietly undoing: the `soc/adc_channel.h` include, the
`SOC_GPIO_VALID_OUTPUT_GPIO_MASK` in the C3 macro, and a `PIN_NUM == 15` typo
in the ESP32 pull-down assert that should always have been `!=`.

### 0007 — `freertos_drivers/esp32/Esp32WiFiManager.cxx`: ESP32-C6 `rom/crc.h`

*C6-specific. One line; clearly upstreamable.*

`Esp32WiFiManager.cxx` picks the SoC's `rom/crc.h` from another
`CONFIG_IDF_TARGET_*` chain, and this one ends in `#else // default to ESP32`
rather than an `#error`. On a C6 that resolves to `<esp32/rom/crc.h>`, which is
not on the include path for a C6 build, so the file fails to compile with a
missing header rather than with anything that names the real problem.

The patch adds the C6 branch. This is new since the `cc1a8b76` baseline in the
sense that it had never been written down here: the developer's own checkout had
the fix applied locally and uncommitted, so the project had never had to carry
it. Building against a pinned commit is what surfaced it.

It has a second, smaller hunk which is a property of the override mechanism
rather than of the C6. The file includes its own header as
`#include "Esp32WiFiManager.hxx"`, which resolves against the directory of the
file being compiled — and the copy is compiled from `overrides/`, where only the
`.cxx` lives. The override qualifies it as
`#include "freertos_drivers/esp32/Esp32WiFiManager.hxx"`, which is what the
other two patched `.cxx` files in this directory already say, and what upstream
says everywhere else. Any future `.cxx` override needs the same treatment.

### 0006 — retired

`freertos_drivers/esp32/Esp32AdcOneShot.hxx` needed a patch at `cc1a8b76`: the
ADC unit was chosen with an `#if` chain per SoC that had no `#else`, so on the
C6 `.unit_id` was left out of the designated initialiser and zero-initialised to
`ADC_UNIT_1` — right by accident, and silently.

**`bd82a644` covers it.** Upstream rewrote the header: the unit is now chosen
into a local with an explicit `#else` branch defaulting to `ADC_UNIT_1` with a
`#warning`, and — much more usefully — the per-pin `adc_oneshot_new_unit()` call
that made the header unusable for a board with several pins on one ADC is gone,
replaced by an `Esp32ADCUnitManager` (`Esp32AdcUnitManager.cxx`) that shares one
handle per unit. That was the defect this project documented as the reason its
current sense bypasses the header, and it is fixed.

The override and its `.patch` are therefore deleted. Nothing in this firmware
includes the header today; reworking the current sense onto it is now possible
and is follow-up work, which would need `Esp32AdcUnitManager.cxx` adding to the
component's source list.

## Keeping these in step with OpenMRN

Patch 0005 is a header, so it needs no entry in
`components/openmrn/CMakeLists.txt`: the override directory is first on the
include path and shadows it automatically. Only a patched `.cxx` has to be
substituted into the source list — 0003, 0004 and 0007 are listed in
`PATCHED_SOURCES` there.

To move the baseline:

```sh
cd openmrn && git fetch && git checkout <new commit> && cd ..
# for each override, diff it against the new upstream file before trusting it
diff -u openmrn/src/<path> patches/overrides/<path>
```

Re-apply each change described above to the new upstream file rather than
keeping the old copy, regenerate the `.patch` with `diff -u`, rebuild both
targets, and update the baseline commit named above. Then commit the submodule's
new gitlink, so the pin and the overrides move together.

**Also check the source list.** `../components/openmrn/CMakeLists.txt` names
every OpenMRN file this firmware compiles, one by one, so a file upstream has
*added* is missing rather than picked up — and if the stack has come to depend
on it the failure is an undefined reference at link time with no hint that a
source is absent. Moving to `bd82a644` needed `openlcb/FilteringCanHubFlow.cxx`
adding for exactly that reason. `ls` the directories in the list against it
after moving the pin.

If OpenMRN gains real C6 support, delete the override and the build picks the
upstream file back up automatically — `components/openmrn/CMakeLists.txt` only
substitutes a file that exists in `overrides/`, and an override that is only a
header just disappears from the include path.

## Notes for porting OpenMRN, found on the way

These are not patches. They are things that cost time to discover and would cost
the next person the same, recorded here because they are properties of the
library rather than of this board.

**`EventState` is an MTI offset, not a plain enum.** It is
`VALID = 0, INVALID = 1, RESERVED = 2, UNKNOWN = 3`, and the identify replies are
built as `MTI_PRODUCER_IDENTIFIED_VALID + state`. `invert_event_state()`
deliberately swaps valid and invalid while leaving `UNKNOWN` alone. The
consequence is worth knowing before modelling any three-valued sensor: a
`BitEventInterface` whose `get_current_state()` returns `UNKNOWN` answers
"unknown" on *both* events of its pair, which is correct and is what you want.
The trap is assuming the bit classes are two-valued and collapsing the sensor to
a bool on the way in — the "I cannot tell" reply is then silently lost, and a
tool is told a thing is false when the node does not know.

**The bit handlers register in their constructors and unregister in their
destructors**, and their event IDs are fixed at construction. So an event ID
edited in a configuration tool means destroying and rebuilding the handler, and
the old one must be destroyed *before* the new one is made — otherwise the old
destructor unregisters the registry entry the new constructor has just added,
and the node silently stops answering for that event.

**An IRAM-safe timer ISR is all-or-nothing, and a copied ISR cannot opt in.**
`gptimer`'s control calls — `gptimer_set_alarm_action`, `gptimer_start`,
`gptimer_stop`, `gptimer_set_raw_count` — are documented as callable from
interrupt context and are placed in IRAM by `CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM`,
so re-arming an alarm from inside the handler is supported and safe. But
`CONFIG_GPTIMER_ISR_IRAM_SAFE` requires the callback *and every function it
reaches* to be resident in RAM, `memcpy` and small static helpers included. An
interrupt handler you cannot annotate — because it lives in vendor code, or in
a file you have chosen not to touch — therefore cannot be made
cache-safe at all, and the cost lands at the application level: nothing may
write flash while that interrupt can fire. Worth designing for early, because
the failure is a crash during a flash erase, which looks like a random hang
rather than a fault with an obvious cause.

**`RefreshLoop` can be added to but not subtracted from.** `add_member()` exists;
there is no remove, and stopping the loop needs a wait on the executor that
cannot be done from inside it. Anything whose set of producers changes at run
time therefore wants a fixed set of `Polling` shells holding swappable
producers, rather than members that come and go.
