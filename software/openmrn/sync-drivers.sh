#!/bin/sh
#
# Refresh this firmware's copies of the hardware drivers from ../FemtoLCC, and
# say which ones differ.
#
# The two firmwares share the board, so they share the code that drives it:
# Channels, Occupancy, Turnouts, IoPins, Expander and the pin map. ../FemtoLCC
# is the original; the copies in main/ are byte-identical to it, which is the
# whole point — a plain `cmp` is then a trustworthy drift check.
#
# The copies compile unchanged because this project supplies, in main/:
#
#   Config.h        the setting enums and structs, without AOLCB's
#                   configuration-space layout or its <ConfigStorage.h>
#   Arduino.h       millis, pinMode, digitalWrite/Read, analogRead, ledc*
#   Wire.h          the I2C calls Expander makes
#
# Nothing in the copies is edited. If a copy ever has to diverge, that is a port
# problem to solve in one of those three files instead.
#
# Usage:
#   ./sync-drivers.sh            copy anything that changed, and report
#   ./sync-drivers.sh --check    report only; exit 1 if anything differs
#
#   Uncommon Models — https://uncommonmodels.com

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../FemtoLCC"
DST="$HERE/main"
MANIFEST="$HERE/drivers.manifest"

FILES="board.h \
       Channels.h Channels.cpp \
       Occupancy.h Occupancy.cpp \
       Turnouts.h Turnouts.cpp \
       IoPins.h IoPins.cpp \
       Expander.h Expander.cpp"

CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
elif [ $# -gt 0 ]; then
    echo "usage: $0 [--check]" >&2
    exit 2
fi

if [ ! -d "$SRC" ]; then
    echo "Cannot find the AOLCB firmware at $SRC" >&2
    exit 2
fi

differs=0
changed=0

for f in $FILES; do
    if [ ! -f "$SRC/$f" ]; then
        echo "MISSING  $f — gone from ../FemtoLCC"
        differs=$((differs + 1))
        continue
    fi

    if [ ! -f "$DST/$f" ]; then
        if [ "$CHECK_ONLY" -eq 1 ]; then
            echo "ABSENT   $f — no copy here"
            differs=$((differs + 1))
        else
            cp "$SRC/$f" "$DST/$f"
            echo "ADDED    $f"
            changed=$((changed + 1))
        fi
        continue
    fi

    if cmp -s "$SRC/$f" "$DST/$f"; then
        echo "same     $f"
        continue
    fi

    differs=$((differs + 1))
    if [ "$CHECK_ONLY" -eq 1 ]; then
        added=$(diff "$DST/$f" "$SRC/$f" | grep -c '^>' || true)
        removed=$(diff "$DST/$f" "$SRC/$f" | grep -c '^<' || true)
        echo "DIFFERS  $f — ../FemtoLCC has $added line(s) added, $removed removed"
    else
        cp "$SRC/$f" "$DST/$f"
        echo "updated  $f"
        changed=$((changed + 1))
    fi
done

echo

if [ "$CHECK_ONLY" -eq 1 ]; then
    if [ "$differs" -gt 0 ]; then
        echo "$differs file(s) differ from ../FemtoLCC."
        echo "Run ./sync-drivers.sh to copy them, then rebuild and check that"
        echo "main/Config.h still provides everything the copies need."
        exit 1
    fi
    echo "All driver copies match ../FemtoLCC."
    exit 0
fi

# Record what was copied, so a later check can say when the copies were taken
# even if both trees move on.
(
    echo "# Hardware driver copies, taken from ../FemtoLCC"
    echo "# Refreshed $(date -u '+%Y-%m-%d %H:%M:%S UTC') by sync-drivers.sh"
    cd "$SRC" && sha256sum $FILES
) > "$MANIFEST"

if [ "$changed" -gt 0 ]; then
    echo "$changed file(s) refreshed; rebuild with 'make build'."
else
    echo "Nothing to do; the copies already match ../FemtoLCC."
fi
