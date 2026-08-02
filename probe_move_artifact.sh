#!/usr/bin/env bash
# probe_move_artifact.sh - is the sampling jump caused by the MOVE, or by the CLOCK?
#
# At move_ns the measured access rate jumps ~86x for EVERY tracked page --
# including the 96 that provably did not relocate.  Pre-move the data
# captures ~1.7% of the accesses the workload's own update rate implies;
# post-move it matches.  So the pre-move baseline is starved and "detection"
# is the measurement recovering.
#
# Every run so far put the move at 60s, so two very different explanations
# fit the same data:
#
#   (a) the RELOCATION causes it -- something about touching the fresh
#       destination region unblocks sampling
#   (b) the CLOCK causes it -- khugepaged/THP promotion, an allocator
#       transition, or a warmup boundary that just happens to land near 60s
#
# They need opposite fixes, so this runs the SAME workload with the move at
# two different times and reports where the jump actually lands.
#
#   jump tracks the move        -> (a), relocation-caused
#   jump stays at a fixed time  -> (b), and the move is a coincidence
#
# ~3 minutes.
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/artifact_probe"
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n UPDATES=2500000000

export LDOS_PEBS_TELEMETRY_ONLY=1
export LDOS_MIN_SAMPLES_TO_TRACK=10
export LDOS_PAGE_SAMPLE_DIVISOR=1
export LDOS_SIGNAL_SAMPLE_MS=200

[[ -x "$GUPS" ]] || { echo "missing $GUPS"; exit 1; }
mkdir -p "$OUT"; rm -f "$OUT"/ml_dataset_* "$OUT"/*.stderr
cd "$MANAGER_DIR" || exit 1

for AT in 25 50; do
    printf '\n\033[1;36m== move at %ss\033[0m\n' "$AT"
    GUPS_MOVE_AT_SEC=$AT LDOS_CSV_LABEL="mv$AT" LD_PRELOAD=./lib/libmmap_shim.so \
        "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
        >"$OUT/mv$AT.stdout" 2>"$OUT/mv$AT.stderr" || true
    [[ -f "ml_dataset_mv$AT.csv" ]] && mv "ml_dataset_mv$AT.csv" "$OUT/"
    python3 - "$OUT/ml_dataset_mv$AT.csv" "$OUT/mv$AT.stderr" <<'PY'
import sys, os, re, csv
from collections import defaultdict
csvp, errp = sys.argv[1], sys.argv[2]
if not os.path.exists(csvp):
    print("   no CSV"); raise SystemExit
m = re.search(r"LDOS_GROUNDTRUTH move_ns=(\d+)", open(errp, errors="replace").read())
mv = int(m.group(1)) if m else None

per_bar = defaultdict(float)          # bar -> summed access_count (cumulative)
for r in csv.DictReader(open(csvp)):
    try:
        per_bar[int(r["timestamp_ns"])] += float(r["access_count"] or 0)
    except (TypeError, ValueError):
        pass
bars = sorted(per_bar)
if len(bars) < 4:
    print("   too few bars"); raise SystemExit
t0 = bars[0]

# per-bar DELTA of the cumulative total = work observed in that bar
deltas = [(bars[i], per_bar[bars[i]] - per_bar[bars[i-1]]) for i in range(1, len(bars))]
# biggest multiplicative step = where sampling changes regime
best, ratio = None, 0.0
for i in range(1, len(deltas)):
    prev, cur = deltas[i-1][1], deltas[i][1]
    if prev > 0 and cur / prev > ratio:
        ratio, best = cur / prev, deltas[i][0]

print(f"   run length      : {(bars[-1]-t0)/1e9:.0f}s over {len(bars)} bars")
if mv: print(f"   move logged at  : {(mv-t0)/1e9:.1f}s")
if best:
    print(f"   biggest jump at : {(best-t0)/1e9:.1f}s  ({ratio:.0f}x step)")
    if mv:
        d = (best - mv) / 1e9
        print(f"   jump - move     : {d:+.1f}s   "
              + ("<-- TRACKS THE MOVE" if abs(d) < 3 else "<-- does NOT track the move"))
PY
done

cat <<'EOF'

VERDICT
  If both runs show the jump within ~1 bar of their own move time, the
  relocation causes it and the pre-move baseline is a property of the
  workload's first phase.
  If both jump at about the same WALL-CLOCK second regardless of when the
  move happened, the move is a coincidence -- suspect THP/khugepaged or an
  allocator transition, and check /sys/kernel/mm/transparent_hugepage.
EOF
