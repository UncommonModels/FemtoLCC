---
title: "Errata"
description: "Known faults in built boards, how to spot them, and how to rework them."
weight: 5
---

Known problems with boards as built, listed by the revision printed on the silkscreen.
Each entry gives the symptom first, since that is what you will see on the bench.

## FemtoLCC v1

### E1 — `R5` not fitted: the module never leaves reset

**Symptom.** The board powers up but the ESP32-C6 does nothing. Plugged into a computer
over USB-C at `P1`, nothing enumerates at all — no `303a:1001` device, no
`/dev/ttyACM*` port, not even a failed enumeration. The UART header at `J1` is silent
too, and holding BOOT while tapping RESET makes no difference.

**Cause.** `R5` is the 47 k pull-up from `+3.3V` to the module's `EN` (`CHIP_PU`, `U14`
pin 3). The C6 has no internal pull-up on that pin, so with `R5` absent `EN` floats low
and the chip is held in reset permanently. The schematic and PCB are correct; the fault
is in the fab pipeline. `tools/do-not-populate.txt` listed `R5` as "unpopulated by
design", so it was dropped from the assembly BOM and CPL and JLC never placed it.

**Affected.** Every v1 board assembled from the repository's fab outputs.

**Rework.** Fit a 47 k 0402 resistor at `R5`. Anything from 10 k to 47 k works; a leaded
resistor from `U14` pin 3 to `+3.3V` does as a bodge if 0402 is awkward.

A solder bridge across the `R5` pads also brings the board up, and it is safe — the
reset button pulls `EN` to ground through `R24` (2.2 k), so pressing it only draws about
1.5 mA. But it costs two things:

- **The RESET button stops working.** `EN` is held at 3.3 V however hard `SW1` pulls.
- **The power-on delay is lost.** `R5` and `C46` (1 µF) normally hold `EN` low for
  around 50 ms while the 3.3 V rail settles. Tied straight to the rail, `EN` rises with
  it, which Espressif advises against and which can show up as the odd failed cold boot.

Flashing is unaffected either way: esptool resets the C6 through its USB Serial/JTAG
peripheral, not through `EN`.

**Fix for future builds.** The `R5` line was removed from `tools/do-not-populate.txt`
and the assembly files regenerated. `R5` now goes out in the BOM and CPL with its
`lcsc-parts.csv` match, C25792, the same part as `R14`.

**Status.** Fixed in the fab files for future builds. The DNP entry was removed on
2026-09-14. Boards already built still need the rework above.
