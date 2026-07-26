#!/bin/sh
# Measure the flash cost of every ENABLE_* build flag.
#
# Builds the current default image as a baseline, then rebuilds once per flag
# with that flag flipped away from its default, and reports the difference.
# A full clean build is ~6 s, so the whole sweep takes ~7 minutes.
#
#   sh utils/flag_cost_sweep.sh [output.tsv]
#
# Columns: flag, default value, size when flipped, cost in bytes, note.
# "cost" is always "what this feature costs you": for a flag that is ON by
# default it is what you reclaim by turning it off, for one that is OFF it is
# what you pay to turn it on. A negative cost means the feature makes the image
# *smaller* than the alternative (the custom menu layout does).
#
# Read the results with three caveats:
#   - LTO makes deltas non-additive. Cutting two 500-byte features rarely frees
#     1000 bytes, and several flags measure 0 because LTO absorbs them.
#   - Rows marked "does not fit" overflowed the 61440-byte program area, so the
#     size is derived from the linker's overflow figure, not a real binary.
#   - Some flag combinations do not compile at all; those are reported as
#     "build error" with the first error line, and are not regressions from
#     this sweep — most are upstream configurations nobody builds.
set -u
cd "$(dirname "$0")/.." || exit 1

OUT="${1:-flagcost.tsv}"
: > "$OUT"

echo "building baseline..." >&2
make clean >/dev/null 2>&1
if ! make -j8 >/tmp/flagcost.log 2>&1; then
    echo "baseline build failed; see /tmp/flagcost.log" >&2
    exit 1
fi
BASE=$(stat -f%z f4hwn.bin 2>/dev/null || stat -c%s f4hwn.bin)
echo "baseline: $BASE bytes" >&2
printf '# baseline\t\t%s\t\t default make\n' "$BASE" >> "$OUT"

flags=$(grep -oE '^ENABLE_[A-Z_0-9]+[[:space:]]*\?=' Makefile | sed -E 's/[[:space:]]*\?=//' | sort -u)

for f in $flags; do
    [ "$f" = "ENABLE_CLANG" ] && continue    # picks a different compiler, not a feature
    cur=$(grep -E "^$f[[:space:]]*\?=" Makefile | sed -E 's/.*\?=[[:space:]]*//' | tr -d '[:space:]')
    if [ "$cur" = "1" ]; then new=0; else new=1; fi

    echo "  $f: $cur -> $new" >&2
    make clean >/dev/null 2>&1
    if make -j8 "$f=$new" >/tmp/flagcost.log 2>&1; then
        s=$(stat -f%z f4hwn.bin 2>/dev/null || stat -c%s f4hwn.bin)
        if [ "$cur" = "1" ]; then cost=$((BASE - s)); else cost=$((s - BASE)); fi
        printf '%s\t%s\t%s\t%s\t\n' "$f" "$cur" "$s" "$cost" >> "$OUT"
    else
        ov=$(grep -oE 'overflowed by [0-9]+ byte' /tmp/flagcost.log | head -1 | grep -oE '[0-9]+')
        if [ -n "$ov" ]; then
            printf '%s\t%s\t%s\t%s\tdoes not fit\n' \
                "$f" "$cur" "$((61440 + ov))" "$((61440 + ov - BASE))" >> "$OUT"
        else
            err=$(grep -m1 -E 'error:' /tmp/flagcost.log | cut -c1-90)
            printf '%s\t%s\t-\t-\tbuild error: %s\n' "$f" "$cur" "$err" >> "$OUT"
        fi
    fi
done

# ENABLE_APRS_ACOUSTIC forces the flashlight and the RSSI bar off, so its plain
# delta is net of those two cuts and badly understates the decoder. Measure it
# again with both held on to get the gross figure.
echo "  ENABLE_APRS_ACOUSTIC (auto-cuts held off)" >&2
make clean >/dev/null 2>&1
if make -j8 ENABLE_APRS_ACOUSTIC=1 ENABLE_FLASHLIGHT=1 ENABLE_RSSI_BAR=1 >/tmp/flagcost.log 2>&1; then
    s=$(stat -f%z f4hwn.bin 2>/dev/null || stat -c%s f4hwn.bin)
    printf 'ENABLE_APRS_ACOUSTIC (gross)\t0\t%s\t%s\t\n' "$s" "$((s - BASE))" >> "$OUT"
else
    ov=$(grep -oE 'overflowed by [0-9]+ byte' /tmp/flagcost.log | head -1 | grep -oE '[0-9]+')
    printf 'ENABLE_APRS_ACOUSTIC (gross)\t0\t%s\t%s\tdoes not fit\n' \
        "$((61440 + ov))" "$((61440 + ov - BASE))" >> "$OUT"
fi

echo "SWEEP DONE" >> "$OUT"
make clean >/dev/null 2>&1 && make -j8 >/dev/null 2>&1   # leave the default build in the tree
echo "done -> $OUT" >&2
