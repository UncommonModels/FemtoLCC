#!/usr/bin/env python3
"""Convert a KiCad CSV position file into JLCPCB CPL format.

KiCad emits  Ref,Val,Package,PosX,PosY,Rot,Side
JLCPCB wants Designator,Mid X,Mid Y,Layer,Rotation

Rotation is normalised into [0, 360) to match JLC's convention.

With --parts, the mapping decides what gets placed: a part is listed only when
it is assigned an LCSC number and not marked Assemble=no. Everything else -
hand-fitted connectors, unresolved footprints, mounting holes - is dropped.

JLC rejects an upload where a placed designator carries no part number, so the
CPL must list only parts that will actually be fitted. Through-hole parts marked
for assembly are included, which requires JLC's through-hole service.
"""
import argparse
import csv
import sys

from partmap import load_partmap, load_dnp, placement

SIDE = {"top": "Top", "bottom": "Bottom"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="KiCad position CSV (default: stdin)")
    ap.add_argument("output", nargs="?", help="CPL to write (default: stdout)")
    ap.add_argument("--parts", metavar="FILE",
                    help="lcsc-parts.csv; drop placements with no LCSC number")
    ap.add_argument("--dnp", metavar="FILE", nargs="?", const=True,
                    help="do-not-populate list; drop those designators")
    args = ap.parse_args()

    mapping = load_partmap(args.parts) if args.parts else {}
    dnp = load_dnp(None if args.dnp is True else args.dnp) if args.dnp else set()

    src = open(args.input, newline="") if args.input else sys.stdin
    dst = open(args.output, "w", newline="") if args.output else sys.stdout

    reader = csv.DictReader(src)
    missing = {"Ref", "Val", "Package", "PosX", "PosY", "Rot", "Side"} - set(reader.fieldnames or [])
    if missing:
        sys.exit(f"pos2jlc: input is missing column(s): {', '.join(sorted(missing))}")

    writer = csv.writer(dst, lineterminator="\r\n")
    writer.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])

    dropped = {}
    for row in reader:
        if row["Ref"] in dnp:
            dropped.setdefault("not populated", []).append(row["Ref"])
            continue
        if mapping:
            place, reason = placement(mapping, row["Val"], row["Package"])
            if not place:
                dropped.setdefault(reason, []).append(row["Ref"])
                continue
        side = row["Side"].strip().lower()
        writer.writerow([
            row["Ref"],
            f"{float(row['PosX']):.4f}",
            f"{float(row['PosY']):.4f}",
            SIDE.get(side, row["Side"]),
            f"{float(row['Rot']) % 360:.2f}",
        ])

    if dst is not sys.stdout:
        dst.close()
    if src is not sys.stdin:
        src.close()

    for reason, refs in sorted(dropped.items()):
        print(f"pos2jlc: not placed, {reason}: {', '.join(refs)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
