#!/usr/bin/env python3
"""Shared reader for tools/lcsc-parts.csv, the LCSC part assignments.

Both the BOM and the CPL generator consult this so the two files agree on
exactly which parts JLC is expected to assemble.

Each row carries an optional Assemble column:

    (blank)  assemble it, provided an LCSC number is assigned
    no       do not place - fitted by hand, or not fitted at all

A part is placed in the CPL only when the mapping assigns it an LCSC number and
does not mark it "no". Parts absent from the mapping are never placed: anything
JLC assembles needs a part number, so an unknown part is a gap to fix, not a
default to assume.
"""
import csv
import os

PARTMAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lcsc-parts.csv")
DNP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "do-not-populate.txt")


def load_dnp(path=None):
    """Read do-not-populate.txt into a set of designators."""
    path = path or DNP
    if not os.path.exists(path):
        return set()
    designators = set()
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                designators.add(line.split()[0])
    return designators


def load_partmap(path=None):
    """Read the mapping into {(value, footprint): {lcsc, mpn, manufacturer, assemble}}."""
    path = path or PARTMAP
    if not os.path.exists(path):
        return {}
    with open(path, newline="") as fh:
        # Leading "#" lines carry provenance notes, not data.
        rows = [line for line in fh if not line.startswith("#")]
    mapping = {}
    for row in csv.DictReader(rows):
        mapping[(row["Value"], row["Footprint"])] = {
            "lcsc": (row.get("LCSC") or "").strip(),
            "mpn": (row.get("MPN") or "").strip(),
            "manufacturer": (row.get("Manufacturer") or "").strip(),
            "assemble": (row.get("Assemble") or "").strip().lower(),
            "note": (row.get("Note") or "").strip(),
        }
    return mapping


def placement(mapping, value, footprint):
    """Decide whether a placement belongs in the CPL.

    Returns (place, reason) where reason explains a refusal.
    """
    entry = mapping.get((value, footprint))
    if entry is None:
        return False, "not in the part mapping"
    if not entry["lcsc"]:
        return False, "no part assigned"
    if entry["assemble"] == "no":
        return False, "fitted by hand"
    return True, ""
