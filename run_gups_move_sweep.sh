#!/usr/bin/env bash
#
# run_gups_move_sweep.sh - collect the GUPS hot-set relocation the study never got,
#                          at several signal cadences.
#
# WHY THIS EXISTS
# ---------------
# Two separate problems with the existing gups_hotset_move datasets:
#
#   1. NO RELOCATION.  Stock gups-hotset-move.c moves the hot set at tic>=150
#      (150 wall-clock seconds).  Our runs end at ~35s, so the move never fired
#      and the dataset is behaviourally identical to gups_skewed.  Details in
#      results/gups_hotset_move_never_moved.txt.  Fixed by the patch in
#      patches/gups-hotset-move-configurable.diff, which makes the move time
#      configurable (GUPS_MOVE_AT_SEC) and logs the exact relocation timestamp
#      on CLOCK_MONOTONIC plus the region geometry -- so hot/cold ground truth
#      is computable without reference to any measured access rate.
#
#   2. CADENCE IS AN UNCONTROLLED VARIABLE.  The exporter asks for a 50 ms bar,
#      but the policy thread cannot hold that once many pages are tracked, so
#      the MEASURED cadence lands wherever it lands -- 143 ms on gups_skewed,
#      186 ms on xsbench, 357 ms on PageRank (1.9x-7.1x nominal).  Signal
#      rankings are known to be cadence-sensitive (ADX and linreg ranked #1 at
#      a 30x-stretched clock and collapsed at measured cadence), so a variable
#      that differs per dataset and is never set on purpose is a confound.
#      LDOS_SIGNAL_SAMPLE_MS now sets it explicitly; this script sweeps it.
#
# WHAT IT PRODUCES
#   ml_dataset_gups_move_c{50,100,200,400}.csv  -- same workload, 4 cadences
#   gups_move_c<N>.stderr                       -- LDOS_GEOMETRY + LDOS_GROUNDTRUTH
#                                                  lines needed for ground truth
#
# PREREQS on the CloudLab node
#   bash ~/LDOS-PageReplacement/setup_cloudlab.sh          # env + build
#   cd ~/benchmarks/workloads/gups_hemem
#   git apply ~/LDOS-PageReplacement/patches/gups-hotset-move-configurable.diff
#   make
#   cd ~/LDOS-PageReplacement && make                      # picks up the cadence change
#
# Usage:  bash run_gups_move_sweep.sh [updates_per_thread] [move_at_sec]
#
set -euo pipefail

UPDATES="${1:-1000000000}"     # per thread; tune so the run lasts >2x MOVE_AT
MOVE_AT="${2:-12}"             # seconds into the timed phase

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/sweep_out"

# Geometry args, matching the original runs:
#   4 threads, 2^31 B table, 8 B elements, 2^19 B hot set, no hugetlb
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n

[[ -x "$GUPS" ]] || { echo "missing $GUPS -- apply the patch and run make"; exit 1; }
mkdir -p "$OUT"
cd "$MANAGER_DIR"

echo "GUPS hot-set relocation sweep"
echo "  updates/thread : $UPDATES"
echo "  move at        : ${MOVE_AT}s  (GUPS_MOVE_AT_SEC)"
echo "  cadences       : 50 100 200 400 ms"
echo

for MS in 50 100 200 400; do
    LABEL="gups_move_c${MS}"
    echo "=== ${MS}ms cadence -> ml_dataset_${LABEL}.csv ==="

    LDOS_CSV_LABEL="$LABEL" \
    LDOS_PEBS_TELEMETRY_ONLY=1 \
    LDOS_MIN_SAMPLES_TO_TRACK=3 \
    LDOS_PAGE_SAMPLE_DIVISOR=128 \
    LDOS_SIGNAL_SAMPLE_MS="$MS" \
    GUPS_MOVE_AT_SEC="$MOVE_AT" \
    LD_PRELOAD=./lib/libmmap_shim.so \
        "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
        2> "$OUT/${LABEL}.stderr" || true

    # The two lines the ground-truth relabeling needs.
    if grep -q LDOS_GROUNDTRUTH "$OUT/${LABEL}.stderr"; then
        grep -h 'LDOS_GEOMETRY\|LDOS_GROUNDTRUTH' "$OUT/${LABEL}.stderr"
    else
        echo "  *** WARNING: no LDOS_GROUNDTRUTH line -- the move did NOT fire."
        echo "  *** The run ended before ${MOVE_AT}s.  Raise updates_per_thread"
        echo "  *** or lower move_at_sec, and rerun.  This is exactly the failure"
        echo "  *** that produced the original no-move datasets."
    fi

    if [[ -f "ml_dataset_${LABEL}.csv" ]]; then
        mv "ml_dataset_${LABEL}.csv" "$OUT/"
        rows=$(grep -c . "$OUT/ml_dataset_${LABEL}.csv" || echo 0)
        echo "  rows: $rows"
    else
        echo "  *** WARNING: no CSV produced"
    fi
    echo
done

echo "Done.  Outputs in $OUT"
echo
echo "Sanity-check each run BEFORE trusting it:"
echo "  1. every .stderr has an LDOS_GROUNDTRUTH line (the move fired)"
echo "  2. run duration is comfortably more than 2x ${MOVE_AT}s, so there is"
echo "     pre- AND post-move data"
echo "  3. measured cadence is near the requested one -- if 50ms still measures"
echo "     ~200ms, the thread could not hold it and that run is the old problem"
echo "     again, which is itself the result the sweep is meant to surface"
echo
echo "Then gzip and copy back to data/real_workloads/gups_move_sweep/"
