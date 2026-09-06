---
title: "Build workflow"
description: "The make targets that regenerate every output in this repository."
weight: 4
---

All fabrication outputs and this documentation site are generated from the KiCad
project by `make`. Nothing in `production/` should be edited by hand.

```
make help          list every target
make check         ERC + DRC, non-zero exit on any violation
make stock-check   live JLC stock check for every BOM line
make upload        stage the three JLCPCB upload files in production/upload
make production    gerbers, drill, zip, BOM, CPL and the 3D render
make docs          build the documentation site into site/public
make serve         Hugo dev server with live reload on :1313
make all           check, then production, then docs
```

## Individual targets

| Target | Output |
|---|---|
| `gerbers` | `production/gerbers/*.g*` |
| `drill` | `production/gerbers/*.drl` plus gerberX2 drill maps |
| `zip` | `production/FemtoLCC-gerbers.zip` |
| `bom` | `production/FemtoLCC-BOM.csv` — assembly BOM, filtered to the CPL |
| `bom-full` | `production/FemtoLCC-parts-reference.csv` — every part, reference only |
| `upload` | `production/upload/` — the three files to send JLC, and nothing else |
| `cpl` | `production/FemtoLCC-CPL.csv` |
| `render` / `render-bottom` | `production/render-*.png` |
| `site-assets` | re-renders the board preview and stages it into `site/static/img/` |
| `schematic` | `production/FemtoLCC-schematic.pdf` |
| `erc` / `drc` | reports in `production/`, non-zero exit on violations |
| `stock-check` | live JLC stock report, `BOARDS=n` to set the build size |

Targets are dependency-tracked against `FemtoLCC.kicad_pcb` and the schematic files, so
re-running `make production` after touching nothing does nothing. Use `make -B <target>`
to force a rebuild.

## The board preview regenerates itself

`make docs` and `make serve` both depend on `site-assets`, which depends on the render
itself. So the preview on the front page is rebuilt from the current board whenever the
board is newer than the image, and skipped when it is not:

```
$ make docs          # board unchanged
Staged board preview from production/render-top.png
Total in 43 ms

$ make docs          # after saving the board
Rendering time 18.175 s
Successfully created 3D render image
Staged board preview from production/render-top.png
```

The render costs about 18 seconds, which is why it is dependency-tracked rather than
unconditional. The staged copy under `site/static/img/` is committed, so the Pages
workflow can build the site without KiCad — CI runs `hugo` directly rather than going
through this target.

## Zone fills and DRC

`make drc` passes `--refill-zones`. Without it, zone fills saved before a via was added
still cover that via, and DRC reports phantom `0.0000 mm` clearance errors against the
inner copper pours — twenty of them appeared on this board after one routing session. The
gerber export refills for the same reason via `--check-zones`. Neither command writes to
the board file, so refill and save in Pcbnew (**B**) when the stored fills go stale.

## KiCad CLI discovery

The Makefile prefers a native `kicad-cli` on `PATH` and falls back to the Flatpak:

```make
KICAD_CLI ?= $(shell command -v kicad-cli >/dev/null 2>&1 \
	&& echo kicad-cli \
	|| echo 'flatpak run --command=kicad-cli org.kicad.KiCad')
```

Override it for a different install:

```
make production KICAD_CLI=/opt/kicad/bin/kicad-cli
```

One constraint worth knowing: the Flatpak sandbox cannot see `/tmp` or paths outside
`$HOME`. Every output path in the Makefile is repo-relative for that reason — pointing a
target at `/tmp` will report success and write nothing.

## The two conversion scripts

`kicad-cli` cannot emit JLCPCB's CSV dialects directly, so two small scripts in `tools/`
post-process its output. Both are pure standard-library Python and read stdin/stdout or
file arguments.

**`tools/pos2jlc.py`** converts a KiCad position file into a CPL: it renames the columns
to `Designator, Mid X, Mid Y, Layer, Rotation`, normalises rotations into 0–360° and
writes CRLF line endings.

**`tools/bom2jlc.py`** groups a flat per-part BOM export into order lines. Parts merge
only when value, footprint, LCSC number, MPN and manufacturer all match, so two
otherwise identical passives with different part numbers stay on separate lines. It also
strips the library prefix from footprint names and sorts references naturally, so `C2`
precedes `C10`.

Both were verified to reproduce the existing hand-made `production/` files byte for byte
before part assignments were layered on top.

**`tools/lcsc-parts.csv`** holds the LCSC assignments, keyed on value plus footprint,
read through the shared `tools/partmap.py`. It overrides whatever the schematic carries,
because several symbols hold incorrect part numbers.

Rows with an empty LCSC column are deliberately unassigned, and that drives the
consistency rule JLC requires: `pos2jlc.py` drops those placements from the CPL, then
`bom2jlc.py --placements` filters the BOM down to exactly what the CPL places. Both
print what they dropped:

```
pos2jlc: not placed, fitted by hand: J2
pos2jlc: not placed, no part assigned: H1, H2, H3, H4, J1, JP6, SW1
bom2jlc: not assembled, absent from the CPL: J1, J2, JP6, SW1
```

An `Assemble` column in the mapping controls this. Blank means assemble it provided an
LCSC number is assigned; `no` keeps it out of both files. A part absent from the mapping
is never placed — anything JLC assembles needs a part number, so an unknown part is a gap
to fix rather than a default to assume. That is what keeps the mounting holes out.

**`tools/do-not-populate.txt`** lists designators left deliberately empty. They are
dropped from the assembly BOM and the CPL but kept in the reference list, which documents
every position on the board. Use it when the part mapping cannot separate the position
from its neighbours — it is keyed on value plus footprint, so `R5` and `R14` (both 47 k
0402) are indistinguishable to it. Setting DNP on the symbol in KiCad has the same effect,
since both exports pass `--exclude-dnp`.

Run `make bom-full` for the unfiltered parts list covering the hand-fitted parts too.

`make production` also stages `production/upload/` holding just the gerber zip, the
assembly BOM and the CPL. Upload that directory's contents and nothing else — sending
the reference parts list instead is what makes JLC report *"designators don't exist in
the CPL file"*.

**`tools/jlcstock.py`** queries the JLC parts index live and reports stock coverage per
line, searching for alternatives on anything short or thin. It backs `make stock-check`.

## Publishing

Pushing to `main` builds this site with Hugo and deploys it to GitHub Pages through
`.github/workflows/pages.yml`. The workflow builds documentation only — it does not run
KiCad — so the board render is committed under `site/static/img/`.
