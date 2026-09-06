#!/usr/bin/env python3
"""Check JLCPCB stock for the parts in the generated BOM, live.

Reads production/FemtoLCC-BOM.csv, queries the JLC parts index for every line
that carries an LCSC number, and reports whether stock covers the build. For any
line that is short, out of stock or unassigned, it searches for in-stock
alternatives in the same package and prints them ranked Basic > Preferred >
stock.

    python3 tools/jlcstock.py --boards 25

Exits non-zero if any line cannot cover the requested build, so it can gate a
release. Network failures are reported but do not fail the run.
"""
import argparse
import csv
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://jlcsearch.tscircuit.com"
UA = "Mozilla/5.0 (FemtoLCC BOM tooling; +https://github.com/UncommonModels/FemtoLCC)"

# Multipliers for the value suffixes KiCad uses in this project.
R_SUFFIX = {"r": 1, "k": 1e3, "m": 1e6}
C_SUFFIX = {"p": 1e-12, "n": 1e-9, "u": 1e-6}


def fetch(path, **params):
    url = f"{BASE}{path}?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=45) as r:
        return json.load(r)


def package_of(footprint):
    """Pull an imperial package code out of a KiCad footprint name."""
    m = re.search(r"_(\d{4})_\d+Metric", footprint)
    return m.group(1) if m else None


def resistance_of(value):
    """'R1k430' -> 1430.0, 'R2.2k' -> 2200.0, 'R2k 1W' -> 2000.0, 'R120' -> 120.0"""
    v = value.strip()
    if not v.upper().startswith("R"):
        return None
    v = re.split(r"\s", v[1:])[0].lower()          # drop a trailing ' 1W'
    m = re.fullmatch(r"(\d+)([rkm])(\d*)", v)      # 1k430, 4k7, 2k
    if m:
        whole, suffix, frac = m.groups()
        num = float(whole) + (float(f"0.{frac}") if frac else 0.0)
        return num * R_SUFFIX[suffix]
    m = re.fullmatch(r"([\d.]+)([rkm]?)", v)       # 2.2k, 120
    if m:
        num, suffix = m.groups()
        return float(num) * R_SUFFIX.get(suffix, 1)
    return None


def capacitance_of(value):
    """'C100n' -> 1e-7, 'C10u VM' -> 1e-5"""
    v = value.strip()
    if not v.upper().startswith("C"):
        return None
    v = re.split(r"\s", v[1:])[0].lower()          # drop a trailing ' VM'
    m = re.fullmatch(r"([\d.]+)([pnu])", v)
    if m:
        num, suffix = m.groups()
        return float(num) * C_SUFFIX[suffix]
    m = re.fullmatch(r"(\d+)([pnu])(\d+)", v)      # 4u7 style
    if m:
        whole, suffix, frac = m.groups()
        return (float(whole) + float(f"0.{frac}")) * C_SUFFIX[suffix]
    return None


def clean(x):
    """Trim binary-float noise so 10u queries as 1e-05, not 9.999...e-06."""
    return float(f"{x:.12g}")


def rank(part):
    return (not part.get("is_basic"), not part.get("is_preferred"), -(part.get("stock") or 0))


def label(part):
    if part.get("is_basic"):
        return "BASIC"
    if part.get("is_preferred"):
        return "PREF"
    return ""


def search_terms(mpn, value):
    """Progressively broader catalogue search terms for a non-passive part."""
    terms = []
    for candidate in (mpn, re.split(r"[/]", mpn)[0] if mpn else "",
                      re.sub(r"[A-Z]{1,4}$", "", re.split(r"[-/]", mpn)[0]) if mpn else "",
                      value):
        candidate = (candidate or "").strip()
        if len(candidate) >= 4 and candidate not in terms:
            terms.append(candidate)
    return terms


def alternatives(value, footprint, mpn, limit=4):
    """Find in-stock replacements for a line, best first."""
    package = package_of(footprint)
    try:
        ohms = resistance_of(value)
        if ohms is not None and package:
            rows = fetch("/resistors/list.json", package=package,
                         resistance=repr(clean(ohms)))["resistors"]
            return sorted([r for r in rows if r.get("in_stock")], key=rank)[:limit]

        farads = capacitance_of(value)
        if farads is not None and package:
            rows = fetch("/capacitors/list.json", package=package,
                         capacitance=repr(clean(farads)))["capacitors"]
            return sorted([c for c in rows if c.get("in_stock")], key=rank)[:limit]

        # Anything else: search the catalogue on the part number, widening the
        # term as we go so a package-suffixed MPN still turns up its siblings.
        found = {}
        for term in search_terms(mpn, value):
            try:
                rows = fetch("/components/list.json", search=term)["components"]
            except (urllib.error.URLError, urllib.error.HTTPError, KeyError, ValueError):
                continue
            for c in rows:
                if (c.get("stock") or 0) > 0:
                    found.setdefault(c["lcsc"], c)
            if len(found) >= limit * 2:
                break
        return sorted(found.values(), key=rank)[:limit]
    except (urllib.error.URLError, urllib.error.HTTPError, KeyError, ValueError):
        return []


def lookup(lcsc, mpn):
    """Current stock for an assigned LCSC number, matched through its MPN."""
    try:
        rows = fetch("/components/list.json", search=mpn)["components"]
    except (urllib.error.URLError, urllib.error.HTTPError, KeyError, ValueError) as exc:
        return None, f"lookup failed: {exc}"
    want = lcsc.lstrip("Cc")
    for c in rows:
        if str(c["lcsc"]) == want:
            return c, None
    return None, "LCSC number not found under that MPN"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bom", nargs="?", default="production/FemtoLCC-BOM.csv")
    ap.add_argument("--boards", type=int, default=10,
                    help="build quantity to check stock against (default: 10)")
    ap.add_argument("--suggest", action="store_true",
                    help="also list alternatives for lines that are fine")
    args = ap.parse_args()

    try:
        with open(args.bom, newline="") as fh:
            lines = list(csv.DictReader(fh))
    except FileNotFoundError:
        sys.exit(f"{args.bom} not found - run 'make bom' first")

    print(f"Checking JLC stock for {args.boards} boards against {args.bom}\n")

    short, thin, unassigned, errors = [], [], [], []
    for line in lines:
        value = line["Comment"]
        refs = line["Designator"]
        qty = int(line["Quantity"])
        need = qty * args.boards
        lcsc = line["LCSC Part #"].strip()
        mpn = line["MPN"].strip()
        footprint = line["Footprint"]

        if not lcsc:
            unassigned.append((value, refs, footprint, mpn, need))
            continue

        part, err = lookup(lcsc, mpn)
        if err:
            errors.append((value, lcsc, err))
            print(f"  ?  {value:<20} {lcsc:<10} {err}")
            continue

        stock = part.get("stock") or 0
        boards = stock // qty
        # Less than double the build is thin: stock moves between quote and order.
        if stock < need:
            mark = "!! "
        elif stock < need * 2:
            mark = "~~ "
        else:
            mark = "OK "
        print(f"  {mark}{value:<20} {lcsc:<10} {label(part):<5} "
              f"stock={stock:<9} need={need:<6} covers {boards} boards")
        if stock < need:
            short.append((value, refs, footprint, mpn, need, stock))
        elif stock < need * 2:
            thin.append((value, refs, footprint, mpn, need, stock))

    if short or thin:
        print("\n" + "=" * 72)
        print("SHORT / THIN - alternatives in the same package, best first")
        print("=" * 72)
        for value, refs, footprint, mpn, need, stock in short + thin:
            print(f"\n{value}  ({refs})  need {need}, have {stock}")
            alts = alternatives(value, footprint, mpn)
            if not alts:
                print("    no alternatives found")
            for a in alts:
                print(f"    C{a['lcsc']:<9} {a['mfr']:<26} {a.get('package', '') or '':<18} "
                      f"stock={a.get('stock', 0):<9} {label(a)}")

    if unassigned:
        print("\n" + "=" * 72)
        print("UNASSIGNED - no LCSC number in tools/lcsc-parts.csv")
        print("=" * 72)
        for value, refs, footprint, mpn, need in unassigned:
            print(f"\n{value}  ({refs})  {need} needed")
            if args.suggest:
                for a in alternatives(value, footprint, mpn):
                    print(f"    C{a['lcsc']:<9} {a['mfr']:<26} {a.get('package', '') or '':<18} "
                          f"stock={a.get('stock', 0):<9} {label(a)}")

    print(f"\n{len(lines)} lines: {len(short)} short, {len(thin)} thin, "
          f"{len(unassigned)} unassigned, {len(errors)} not looked up")
    return 1 if short else 0


if __name__ == "__main__":
    raise SystemExit(main())
