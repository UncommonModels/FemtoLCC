#!/usr/bin/env python3
"""Group a flat KiCad BOM export into a JLCPCB assembly BOM.

Input is a per-part CSV from `kicad-cli sch export bom` carrying the columns
Reference, Value, Footprint, LCSC, MPN and Manufacturer.

Parts are grouped when Value, Footprint, LCSC, MPN and Manufacturer all match,
so that two otherwise identical passives with different part numbers stay on
separate order lines. The library prefix is stripped from footprint names and
references are sorted naturally (C2 before C10).

tools/lcsc-parts.csv overrides whatever LCSC / MPN / Manufacturer the schematic
carries, matched on Value plus stripped Footprint. The schematic's own part
numbers are known to be wrong on several lines, so the mapping is the authority.

With --placements, references absent from the CPL are dropped and any line left
empty disappears. JLC rejects an upload whose BOM names a designator the CPL
does not place, so the assembly BOM must cover exactly the placed parts. Omit
the flag to get the full BOM, including through-hole parts fitted by hand.
"""
import argparse
import csv
import re
import sys

from partmap import load_partmap, load_dnp

COLUMNS = ["Comment", "Designator", "Footprint", "LCSC Part #", "Quantity", "MPN", "Manufacturer"]
IN_COLUMNS = ["Reference", "Value", "Footprint", "LCSC", "MPN", "Manufacturer"]


def refkey(ref):
    """Natural sort key: split a reference into its prefix and number."""
    m = re.match(r"^([^0-9]*)([0-9]*)", ref.strip())
    prefix, number = m.group(1), m.group(2)
    return (prefix, int(number) if number else 0, ref)


def load_placements(path):
    """Designators present in a JLCPCB CPL file."""
    with open(path, newline="") as fh:
        return {row["Designator"].strip() for row in csv.DictReader(fh)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="flat KiCad BOM CSV (default: stdin)")
    ap.add_argument("output", nargs="?", help="BOM to write (default: stdout)")
    ap.add_argument("--parts", metavar="FILE", help="lcsc-parts.csv overrides")
    ap.add_argument("--placements", metavar="FILE",
                    help="CPL file; keep only designators it places")
    ap.add_argument("--dnp", metavar="FILE", nargs="?", const=True,
                    help="do-not-populate list; drop those designators")
    args = ap.parse_args()

    placed = load_placements(args.placements) if args.placements else None
    dnp = load_dnp(None if args.dnp is True else args.dnp) if args.dnp else set()

    src = open(args.input, newline="") if args.input else sys.stdin
    dst = open(args.output, "w", newline="") if args.output else sys.stdout

    reader = csv.DictReader(src)
    missing = set(IN_COLUMNS) - set(reader.fieldnames or [])
    if missing:
        sys.exit(f"bom2jlc: input is missing column(s): {', '.join(sorted(missing))}")

    partmap = load_partmap(args.parts) if args.parts else load_partmap()

    parts, dropped, unpopulated = [], [], []
    for row in reader:
        ref = row["Reference"].strip()
        if not ref:
            continue
        if ref in dnp:
            unpopulated.append(ref)
            continue
        if placed is not None and ref not in placed:
            dropped.append(ref)
            continue
        # "Capacitor_SMD:C_0805_..." -> "C_0805_..."
        footprint = row["Footprint"].split(":", 1)[-1]
        value = row["Value"]
        entry = partmap.get((value, footprint))
        if entry:
            lcsc, mpn, mfr = entry["lcsc"], entry["mpn"], entry["manufacturer"]
        else:
            lcsc, mpn, mfr = row["LCSC"], row["MPN"], row["Manufacturer"]
        parts.append((ref, value, footprint, lcsc, mpn, mfr))

    groups = {}
    for ref, value, footprint, lcsc, mpn, mfr in sorted(parts, key=lambda p: refkey(p[0])):
        groups.setdefault((value, footprint, lcsc, mpn, mfr), []).append(ref)

    writer = csv.writer(dst, lineterminator="\r\n")
    writer.writerow(COLUMNS)
    for (value, footprint, lcsc, mpn, mfr), refs in groups.items():
        writer.writerow([value, ",".join(refs), footprint, lcsc, len(refs), mpn, mfr])

    if dst is not sys.stdout:
        dst.close()
    if src is not sys.stdin:
        src.close()

    if unpopulated:
        print(f"bom2jlc: not populated: {', '.join(sorted(unpopulated, key=refkey))}",
              file=sys.stderr)
    if dropped:
        print(f"bom2jlc: not assembled, absent from the CPL: "
              f"{', '.join(sorted(dropped, key=refkey))}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
