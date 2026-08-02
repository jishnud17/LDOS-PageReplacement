#!/usr/bin/env bash
# crosscheck_perf_mem.sh - independent check of the shim's page attribution.
#
# Runs the same GUPS config twice on the node:
#   1. under the shim         -> per-page CSV (our measurement)
#   2. under `perf mem record` -> kernel's own PEBS address stream
# then computes the SAME statistic from both: what fraction of access mass
# falls inside the workload's hot windows ((addr-base) mod slice < hot).
#
# The two runs have different ASLR bases, so shapes are compared, not
# addresses.  Agreement (both high) validates shim-side attribution with a
# tool that shares none of its code.  Divergence means one of them is
# misattributing -- and perf is the one with a decade of users.
#
# No relocation during the runs (move parked far out).
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/xcheck_out"
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n UPDATES=800000000

export LDOS_PEBS_TELEMETRY_ONLY=1 LDOS_MIN_SAMPLES_TO_TRACK=3
export LDOS_PAGE_SAMPLE_DIVISOR="${LDOS_PAGE_SAMPLE_DIVISOR:-1}"
export LDOS_SIGNAL_SAMPLE_MS=200 GUPS_MOVE_AT_SEC=99999

[[ -x "$GUPS" ]] || { echo "missing $GUPS (run the sweep's STEP 0 first)"; exit 1; }
command -v perf >/dev/null || { echo "perf not installed on this node"; exit 1; }
mkdir -p "$OUT"; rm -f "$OUT"/*
cd "$MANAGER_DIR" || exit 1

hot_share_py() {  # $1=stderr-with-geometry  stdin=hex addresses, one per line
    python3 - "$1" <<'PY'
import sys, re
txt = open(sys.argv[1], errors="replace").read()
g = re.search(r"LDOS_GEOMETRY region_base=(0x[0-9a-f]+) region_size=(\d+) "
              r"slice_bytes=(\d+) hot_bytes=(\d+)", txt)
if not g:
    print("no LDOS_GEOMETRY in stderr"); sys.exit(1)
base, region, slice_b, hot = (int(g.group(1), 16), int(g.group(2)),
                              int(g.group(3)), int(g.group(4)))
inside = hot_n = total = 0
for line in sys.stdin:
    try: a = int(line.strip(), 16)
    except ValueError: continue
    if base <= a < base + region:
        inside += 1
        if (a - base) % slice_b < hot: hot_n += 1
    total += 1
if inside == 0:
    print(f"0 of {total:,} samples landed in the region -- cannot compare")
    sys.exit(1)
print(f"in-region samples: {inside:,}   hot-window share: {hot_n/inside*100:.1f}%")
PY
}

echo "== run 1: shim attribution"
LDOS_CSV_LABEL=xcheck LD_PRELOAD=./lib/libmmap_shim.so \
    "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
    >"$OUT/shim.stdout" 2>"$OUT/shim.stderr" || true
[[ -f ml_dataset_xcheck.csv ]] && mv ml_dataset_xcheck.csv "$OUT/"
# weight each page by its sample count (access_count / period), emit one
# address line per sample so both sides compute the same statistic
python3 - "$OUT/ml_dataset_xcheck.csv" <<'PY' | hot_share_py "$OUT/shim.stderr"
import sys, csv
from collections import defaultdict
tot = defaultdict(float)
for r in csv.DictReader(open(sys.argv[1])):
    c = float(r["access_count"] or 0)
    if c > tot[r["page_addr"]]: tot[r["page_addr"]] = c
for a, c in tot.items():
    for _ in range(max(int(c / 5003), 1)):
        print(a)
PY

echo "== run 2: perf mem attribution (no shim)"
perf mem record -o "$OUT/perf.data" -- \
    "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
    >"$OUT/perf.stdout" 2>"$OUT/perf.stderr" || {
        echo "perf mem record failed -- check kernel.perf_event_paranoid (needs <=1):"
        sysctl kernel.perf_event_paranoid 2>/dev/null || true
        exit 1
    }
perf script -i "$OUT/perf.data" -F addr 2>/dev/null | grep -oE '0x[0-9a-f]+' \
    | hot_share_py "$OUT/perf.stderr"

echo
echo "Both hot-window shares should be high (~90% for the 90/10 GUPS skew)"
echo "and within ~10 points of each other.  A low shim share with a high"
echo "perf share means the shim is misattributing addresses."
