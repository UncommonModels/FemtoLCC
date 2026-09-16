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
with entry 13, `CHIP_ESP32C6`, which was also missing — without it the C6 would
have printed its own model as "Unknown".

## Keeping these in step with OpenMRN

The overrides are copies, so they go stale if the checkout moves. They were
taken from OpenMRN at **`cc1a8b76`** (2 February 2025). To refresh one, copy the
file from `$OPENMRN_PATH/src/...` again, re-apply the change described above,
and regenerate the `.patch` with `diff -u`.

If OpenMRN gains real C6 support, delete the override and the build picks the
upstream file back up automatically — `components/openmrn/CMakeLists.txt` only
substitutes a file that exists in `overrides/`.
