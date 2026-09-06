---
title: "Mechanical"
description: "Board outline, mounting hole pattern, drill schedule and design rules."
weight: 2
---

The board is laid out on an imperial grid, so the inch figures are the round ones.
Millimetres follow in brackets.

## Board outline

| Property | |
|---|---|
| Width | **3.000 in** (76.20 mm) |
| Height | **3.750 in** (95.25 mm) |
| Thickness | **0.063 in** (1.6 mm) |
| Layers | 4 |
| Copper weight | 1 oz outer, 1 oz inner |

## Mounting holes

Four M4 holes on a rectangular pattern, plated, each ringed with eight stitching vias
tying the pad through the stackup.

| Property | |
|---|---|
| Pattern | **2.500 × 3.250 in** (63.50 × 82.55 mm) |
| Inset from each edge | **0.250 in** (6.35 mm) |
| Hole diameter | **0.169 in** (4.3 mm) |
| Pad diameter | **0.339 in** (8.6 mm) |
| Stitching via drill | **0.024 in** (0.6 mm) |
| Stitching vias per hole | 8 |

The pattern is centred on the board, so each hole sits a quarter inch in from both
nearest edges. `H1` is top-left, `H2` top-right, `H3` bottom-left and `H4` bottom-right.

Hole positions from the board's lower-left corner:

| Ref | X | Y |
|---|---|---|
| `H1` | 0.250 in (6.35 mm) | 3.500 in (88.90 mm) |
| `H2` | 2.750 in (69.85 mm) | 3.500 in (88.90 mm) |
| `H3` | 0.250 in (6.35 mm) | 0.250 in (6.35 mm) |
| `H4` | 2.750 in (69.85 mm) | 0.250 in (6.35 mm) |

M4 clearance through a 0.169 in hole leaves 0.012 in of play, and the 0.339 in pad is
large enough that an M4 washer sits entirely on copper.

## Drill schedule

**Plated (PTH) — 442 holes**

| Diameter | | Count | What |
|---|---|---|---|
| **0.012 in** | 0.30 mm | 353 | Signal vias |
| **0.024 in** | 0.60 mm | 32 | Mounting hole stitching vias |
| **0.028 in** | 0.70 mm | 4 | USB-C `P1` shield legs |
| **0.031 in** | 0.80 mm | 2 | Solder jumper `JP6` |
| **0.037 in** | 0.94 mm | 16 | RJ45 `J2` |
| **0.039 in** | 1.00 mm | 18 | Pin header `J1` (6) and PMOD `J4` (12) |
| **0.047 in** | 1.20 mm | 5 | Pluggable headers `J9`, `J10` |
| **0.051 in** | 1.30 mm | 8 | Screw terminals `J5`–`J8` |
| **0.169 in** | 4.30 mm | 4 | M4 mounting holes |

**Non-plated (NPTH) — 4 holes**

| Diameter | | Count | What |
|---|---|---|---|
| **0.023 in** | 0.58 mm | 1 | USB-C `P1` locating slot, 0.039 × 0.023 in oval |
| **0.026 in** | 0.66 mm | 1 | USB-C `P1` locating hole |
| **0.130 in** | 3.30 mm | 2 | RJ45 `J2` mounting posts |

Smallest plated drill is 0.012 in (0.30 mm). The smallest non-plated feature is `P1`'s
oval slot — note that 0.023 in is its minor axis, not a round hole, and some fabs quote
slots separately.

## Design rules

The rules stored in `FemtoLCC.kicad_pro` are set to JLCPCB's capability limits:

| Rule | | Rule | |
|---|---|---|---|
| Clearance | 0.004 in (0.10 mm) | Track width | 0.004 in (0.10 mm) |
| Hole to hole | 0.010 in (0.25 mm) | Hole clearance | 0.008 in (0.20 mm) |
| Copper to edge | 0.012 in (0.30 mm) | Via diameter | 0.018 in (0.45 mm) |
| Annular ring | 0.005 in (0.13 mm) | Text / silk width | 0.006 in (0.15 mm) |

Actual minimums on the board are looser than that floor: 0.008 in (0.20 mm) track and
0.004 in (0.10 mm) space.

`track_dangling` is set to **ignore** — the one-ended GND stubs in the driver section are
deliberate guard traces, not routing leftovers.

DRC passes with **0 errors and 0 unconnected items**, and ERC with 0 violations.
