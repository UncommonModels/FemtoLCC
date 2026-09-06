# FemtoLCC — JLCPCB Production Package

Generated with KiCad 10.0.6 from `FemtoLCC.kicad_pcb`.
Board: **76.20 × 95.25 mm** (3.000 × 3.750 in), 4 layers, 110 footprints, 348 vias,
fully routed. Four M4 mounting holes on a 63.50 × 82.55 mm (2.500 × 3.250 in) pattern.

## Files

| File | Use |
|---|---|
| `FemtoLCC-gerbers.zip` | Upload to JLCPCB PCB order (gerbers + Excellon drill) |
| `FemtoLCC-BOM.csv` | Upload to JLCPCB assembly — 33 lines, 100 parts, all with LCSC numbers |
| `FemtoLCC-parts-reference.csv` | Reference only - all 37 lines. **Never upload this** |
| `upload/` | The three files to send JLCPCB, and nothing else |
| `FemtoLCC-CPL.csv` | Upload to JLCPCB assembly (100 placements, all top side) |
| `gerbers/` | Loose gerbers + drill maps for inspection |
| `render-top.png` | Visual reference |

## Order settings

| Option | Value |
|---|---|
| Layers | 4 |
| Dimensions | 76.20 × 95.25 mm |
| Board thickness | 1.6 mm |
| **Outer copper** | **1 oz** |
| **Inner copper** | **1 oz** (not JLC's 0.5 oz default — the design assumes 1 oz; select it explicitly) |
| Impedance control | Not required |
| Surface finish | HASL (lead-free or leaded), or ENIG |
| Via covering | Tented (set in board file) |
| Min track/space on board | 0.20 mm / 0.10 mm |
| Min drill | 0.30 mm PTH, 0.58 mm NPTH |

Gerbers are RS-274X, metric, 4.6 format, absolute origin, X2 attributes disabled,
soldermask subtracted from silkscreen. Drill is Excellon, metric, absolute, PTH/NPTH separated.

## Design rule status

DRC passes with **0 errors and 0 unconnected items** against JLCPCB 4-layer 1 oz limits.
The rules stored in `FemtoLCC.kicad_pro` are now set to JLC capability:

    clearance          0.10 mm      track width      0.10 mm
    hole-to-hole       0.25 mm      hole clearance   0.20 mm
    copper-to-edge     0.30 mm      via diameter     0.45 mm
    annular ring       0.13 mm      text/silk width  0.15 mm

`track_dangling` is set to **ignore** — the one-ended GND stubs in the driver
section are intentional guard traces, not routing leftovers.

## Before ordering assembly

LCSC part numbers are now assigned for all 30 JLC-assembled lines, from
`tools/lcsc-parts.csv`, picked against stock with Basic parts preferred. The mapping
overrides the schematic, which still carries three incorrect `Manufacturer Part Number`
fields.

The BOM and CPL are generated from the same mapping and cover exactly the same 100
designators. JLC rejects an upload where they disagree ("The below parts won't be
assembled due to data missing"), so upload `FemtoLCC-BOM.csv`, not the full one.

Four parts are fitted by hand and appear only in the full BOM: `J1` (no part assigned),
`J2` (dual-port RJ45, not stocked at JLC), `JP6` (solder jumper) and `SW1` (unresolved footprint).

The Phoenix terminal blocks are replaced with generic Kefa parts JLC stocks, and the PMOD
socket has a part. Those seven through-hole parts ARE in the CPL, which requires JLC's
through-hole assembly service - quoted separately from SMT and not offered on every
order. A standard SMT order rejects them with "Failed processing the CPL file."; if that
happens, set `Assemble=no` on those four rows in `tools/lcsc-parts.csv` and re-run.

Open items before an assembly order:

1. **Build size is capped at ~29 boards** by `U3` (MCP23018, 52 in stock) and the four
   `SN74HC253DR` (117 in stock). Check with `make stock-check BOARDS=n`.
2. **Through-hole parts are not in the CPL.** `J1`, `J4` and `J5`–`J10` are through-hole;
   JLC quotes these separately and rarely stocks Phoenix parts.
3. **`SW1` is unresolved** — an `SW_Push` symbol on a `C_1206_3216Metric` capacitor
   footprint. Left without a part number until that is settled.
4. **Confirm the ESP32-C6 flash variant.** `U14` is assigned the 4 MB `-N4`; the
   schematic value carries no suffix.
5. **Verify the RJ45 variant.** `J2` is assigned the closest catalogue match to
   `RJHSE508002`; the Amphenol family varies in shielding and magnetics.
6. **Verify rotations in JLC's previewer.** Their convention differs from KiCad's for
   SOT-23, SOIC, LEDs and connectors.

## Regenerating

Everything in this directory is generated. Do not edit these files by hand.

    make production      # gerbers, drill, zip, BOM, CPL, render + stages upload/
    make upload          # restage production/upload/ (the 3 files JLC wants)
    make check           # ERC + DRC, non-zero exit on any violation
    make stock-check     # live JLC stock check for every BOM line
    make help            # all targets

Full documentation is in the `site/` Hugo project, published to
https://uncommonmodels.github.io/FemtoLCC/

Note: the repo `.gitignore` excludes `*.csv`, so the BOM and CPL are untracked by default.
