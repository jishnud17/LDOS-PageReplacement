#!/usr/bin/env bash
#
# run_cloudlab_experiments.sh - ONE script, all outstanding CloudLab collection.
#
# Run this after setup_cloudlab.sh.  It applies the GUPS patch, rebuilds, and
# collects three things:
#
#   STEP 1  LATENCY PROBE      is PERF_SAMPLE_WEIGHT actually populated here?
#   STEP 2  GUPS MOVE SWEEP    the hot-set relocation, at 4 signal cadences
#   STEP 3  LATENCY RUNS       hot/cold labeled from latency, not from rate
#
# ---------------------------------------------------------------------------
# WHY EACH STEP EXISTS
# ---------------------------------------------------------------------------
# STEP 2 -- The existing gups_hotset_move datasets contain NO relocation.  Stock
#   gups-hotset-move.c moves the hot set at tic>=150 (150 wall-clock seconds)
#   and our runs ended at ~35s, so it never fired; the data is behaviourally
#   identical to gups_skewed and its 1,506 "events" are threshold crossings on
#   fluctuating rates, not the designed transition.  Evidence in
#   results/gups_hotset_move_never_moved.txt.  The patch makes the move time
#   configurable and logs the exact relocation timestamp (CLOCK_MONOTONIC, the
#   same clock as timestamp_ns) plus the region geometry -- so hot/cold ground
#   truth is computable with NO reference to any measured access rate.
#
#   The cadence sweep is folded in because signal cadence has been an
#   uncontrolled variable: the exporter asks for 50ms bars but the policy
#   thread cannot hold that under load, so measured cadence landed wherever it
#   landed per dataset (143-357ms, 1.9x-7.1x nominal).  Rankings are known to
#   be cadence-sensitive -- ADX and linreg ranked #1 at a 30x-stretched clock
#   and collapsed at measured cadence -- so a variable that moves rankings was
#   varying by accident.  Same workload, same hardware, only cadence differs.
#
# STEP 1/3 -- Every signal in the study is a transform of interval_access_rate,
#   and events are DEFINED by that same rate crossing a threshold, which makes
#   the top of the ranking partly circular (see circularity_check.py).  Latency
#   (cycles per access, from PEBS weight) is a physically different measurement,
#   so labeling hot/cold from it breaks the circularity.
#
#   MEASURED ON r650 (2026-08-02), correcting the original assumption here:
#   the DEFAULT event 0x81d0 DOES populate PERF_SAMPLE_WEIGHT on Ice Lake --
#   33-71% of rows carried non-zero latency -- because adaptive PEBS can attach
#   the memory-info group to any precise event.  The dedicated load-latency
#   event 0x1cd gave 0.0% on GUPS: it samples far fewer LOADS and GUPS is
#   store-dominated, so nearly nothing with a weight is captured.  It did work
#   on XSBench (39%), which is load-heavy.  Latency runs therefore use the
#   default event; LDOS_PEBS_LOAD_EVENT is kept for experimentation.
#
#   WHERE TO PUT THE MOVE (measured on r650, 2.5e9 updates / ~70s runs):
#   the tracked-page count does NOT saturate.  It climbs roughly LINEARLY at
#   ~30 pages/s for the whole run -- 11 pages at 7s, 145 at 21s, 819 at 50s,
#   1535 at 72s -- because background pages over a 2GB region keep being
#   discovered.  So "wait for the curve to flatten" is not available; the move
#   simply has to go as late as the post-move window allows.
#
#   With MOVE_AT=20 only 45-84 pages had the +-5 bars on BOTH sides that
#   reactivity_analysis.py needs, out of ~1530 tracked.  Notably that count
#   exactly equalled the number of scoreable pages among the 128 hottest: at
#   20s the ONLY pages with pre-move history were hot ones, leaving no cold
#   pages to act as negatives.  MOVE_AT is now 60 in a ~100s run.
#
#   Latency capture is NOT fully reliable: gups_move_w0p5 read 0.0% while
#   w0p25, identical apart from an unrelated window-scale setting, read 55.4%.
#   Treat a zero-latency run as a collection failure to retry, not as evidence.
#
# ---------------------------------------------------------------------------
# Usage:  bash run_cloudlab_experiments.sh [updates_per_thread] [move_at_sec]
#
set -uo pipefail

UPDATES="${1:-3500000000}"
MOVE_AT="${2:-60}"

MANAGER_DIR="$HOME/LDOS-PageReplacement"
WORKLOADS="$HOME/benchmarks/workloads"
GUPS="$WORKLOADS/gups_hemem/gups-hotset-move"
XSBENCH="$WORKLOADS/XSBench/openmp-threading/XSBench"
OUT="$MANAGER_DIR/cloudlab_out"
PATCH="$MANAGER_DIR/patches/patch_gups.py"

# Geometry matching the original runs: 4 threads, 2^31 B, 8 B elts, 2^19 hot
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n

# Common telemetry env for every run
export LDOS_PEBS_TELEMETRY_ONLY=1
export LDOS_MIN_SAMPLES_TO_TRACK=3
export LDOS_PAGE_SAMPLE_DIVISOR=128

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m   !! %s\033[0m\n' "$*"; }

mkdir -p "$OUT"
# Clear stale CSVs: the run set changes between revisions of this script, and
# a leftover from an earlier parameterisation is indistinguishable from a
# fresh one once copied back.  Two 0x1cd runs (lat_c1000/lat_c2000) survived
# exactly this way and reappeared in a later summary as zero-latency results.
rm -f "$OUT"/ml_dataset_*.csv "$OUT"/*.stderr
cd "$MANAGER_DIR"

# --------------------------------------------------------------------------
say "STEP 0  patch + build"

if [[ -f "$PATCH" ]]; then
    # String-replacement patcher, not a unified diff: diffs depend on exact
    # line numbers and context, and the earlier hand-written .diff was corrupt.
    # This verifies every edit landed and is idempotent.  It exits non-zero if
    # the upstream file differs from what it expects -- do NOT continue past
    # that, because an unpatched binary silently yields another no-move dataset.
    if ! python3 "$PATCH" "$WORKLOADS/gups_hemem/gups-hotset-move.c"; then
        echo "GUPS patch FAILED -- aborting rather than collecting a"
        echo "dataset whose hot set never moves."
        exit 1
    fi
    make -C "$WORKLOADS/gups_hemem" >/dev/null 2>&1 || warn "gups make reported errors"
else
    warn "no patcher at $PATCH"; exit 1
fi

make >/dev/null 2>&1 || { echo "manager build FAILED -- run 'make' to see errors"; exit 1; }
echo "   manager built"

[[ -x "$GUPS" ]] || { echo "missing $GUPS"; exit 1; }

# helper: run one collection.  $1=label  $2..=command
collect() {
    local label="$1"; shift
    LDOS_CSV_LABEL="$label" LD_PRELOAD=./lib/libmmap_shim.so \
        "$@" >"$OUT/${label}.stdout" 2>"$OUT/${label}.stderr" || true
    if [[ -f "ml_dataset_${label}.csv" ]]; then
        mv "ml_dataset_${label}.csv" "$OUT/"
        echo "   rows: $(( $(grep -c . "$OUT/ml_dataset_${label}.csv") - 1 ))"
    else
        warn "no CSV produced for $label"
    fi
}

# helper: are the two latency columns non-zero anywhere?
latency_alive() {
    python3 - "$1" <<'PY'
import sys, csv
p = sys.argv[1]
try:
    with open(p) as f:
        r = csv.DictReader(f)
        if "interval_latency_cycles" not in (r.fieldnames or []):
            print("NOCOL"); sys.exit()
        n = 0
        for i, row in enumerate(r):
            if i > 200000: break
            try:
                if float(row["interval_latency_cycles"]) > 0: n += 1
            except (ValueError, TypeError): pass
        print("ALIVE" if n else "ZERO", n)
except FileNotFoundError:
    print("NOFILE")
PY
}

# --------------------------------------------------------------------------
say "STEP 1  latency probe -- is PERF_SAMPLE_WEIGHT populated on this CPU?"
echo "   short GUPS run, DEFAULT load event, 200ms bars"
# Measured on r650: the default 0x81d0 populates weight on 33-71% of rows
# (Ice Lake adaptive PEBS carries the memory-info group on any precise event),
# while the dedicated load-latency event 0x1cd yielded 0.0% on GUPS -- it
# samples far too few LOADS, and GUPS is store-dominated, so almost nothing
# with a weight is captured.  0x1cd is therefore the wrong tool here despite
# being the textbook answer.  Left switchable via LDOS_PEBS_LOAD_EVENT.
LDOS_SIGNAL_SAMPLE_MS=200 \
    collect lat_probe "$GUPS" "$THREADS" 200000000 "$EXPT" "$ELT" "$LOGHOT" "$HUGE"

PROBE=$(latency_alive "$OUT/ml_dataset_lat_probe.csv")
echo "   probe result: $PROBE"
LAT_OK=0
case "$PROBE" in
    ALIVE*) LAT_OK=1; echo "   latency IS captured -- STEP 3 will run" ;;
    ZERO*)  warn "latency all zero -- capture is flaky; retry before concluding." ;;
    NOCOL*) warn "no latency column -- stale binary? rebuild the manager." ;;
    *)      warn "probe produced no CSV." ;;
esac
[[ "$LAT_OK" == "1" ]] || warn "SKIPPING STEP 3.  Latency-labeled analysis is not possible on this hardware/config; report that as the finding rather than collecting zeros."

# --------------------------------------------------------------------------
say "STEP 2  GUPS hot-set relocation, cadence sweep (default rate event)"
echo "   updates/thread=$UPDATES  move_at=${MOVE_AT}s  cadences: 50 100 200 400 ms"

for MS in 50 100 200 400; do
    L="gups_move_c${MS}"
    echo "-- ${MS}ms -> $L"
    LDOS_SIGNAL_SAMPLE_MS="$MS" GUPS_MOVE_AT_SEC="$MOVE_AT" \
        collect "$L" "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE"

    if grep -q LDOS_GROUNDTRUTH "$OUT/${L}.stderr" 2>/dev/null; then
        grep -h 'LDOS_GEOMETRY\|LDOS_GROUNDTRUTH' "$OUT/${L}.stderr"
    else
        warn "NO relocation -- run ended before ${MOVE_AT}s."
        warn "Raise updates_per_thread or lower move_at_sec. This is exactly"
        warn "the failure that produced the original no-move datasets."
    fi
done

# --------------------------------------------------------------------------
say "STEP 2b  indicator window-length sweep"
echo "   Aroon(25) Ichimoku(9/26/52) VHF(28) linreg(25) RWI(14) carry textbook"
echo "   daily-chart lookbacks.  At a 50-350ms bar those span seconds to a"
echo "   minute -- 100-1000x slower than the transitions being detected, which"
echo "   is the leading explanation for why the long-window signals sit at the"
echo "   bottom of every ranking.  That is a claim about TUNING, and this tests"
echo "   it: same workload, same cadence, only the window scale differs."

for SC in 0.25 0.5; do
    L="gups_move_w${SC/./p}"
    echo "-- window scale ${SC} -> $L"
    LDOS_SIGNAL_WINDOW_SCALE="$SC" LDOS_SIGNAL_SAMPLE_MS=200 \
    GUPS_MOVE_AT_SEC="$MOVE_AT" \
        collect "$L" "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE"
    grep -q LDOS_GROUNDTRUTH "$OUT/${L}.stderr" 2>/dev/null \
        || warn "no relocation in $L"
done
echo "   compare against gups_move_c200 (same cadence, scale 1.0)"

# --------------------------------------------------------------------------
if [[ "$LAT_OK" == "1" ]]; then
    say "STEP 3  latency-labeled runs (default event)"
    # Default event -- see STEP 1.  0x1cd produced 0.0% latency on GUPS.
    for MS in 200 400; do
        L="gups_move_lat_c${MS}"
        echo "-- GUPS move, ${MS}ms -> $L"
        LDOS_SIGNAL_SAMPLE_MS="$MS" GUPS_MOVE_AT_SEC="$MOVE_AT" \
            collect "$L" "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE"
        grep -h LDOS_GROUNDTRUTH "$OUT/${L}.stderr" 2>/dev/null || warn "no relocation in $L"
        echo "   latency: $(latency_alive "$OUT/ml_dataset_${L}.csv")"
    done

    if [[ -x "$XSBENCH" ]]; then
        echo "-- XSBench, 400ms -> xsbench_lat"
        LDOS_SIGNAL_SAMPLE_MS=400 \
            collect xsbench_lat "$XSBENCH" -g 130000 -p 20000000 -t 16
        echo "   latency: $(latency_alive "$OUT/ml_dataset_xsbench_lat.csv")"
    else
        warn "XSBench not built -- skipping (rerun setup_cloudlab.sh)"
    fi
fi

# --------------------------------------------------------------------------
say "SUMMARY"
for f in "$OUT"/ml_dataset_*.csv; do
    [[ -e "$f" ]] || continue
    b=$(basename "$f" .csv)
    printf '  %-34s %10s rows   latency=%s\n' \
        "${b#ml_dataset_}" "$(( $(grep -c . "$f") - 1 ))" "$(latency_alive "$f")"
done

cat <<EOF

CHECK BEFORE TRUSTING ANY OF THIS
  1. every gups_move_* stderr has an LDOS_GROUNDTRUTH line (the move fired)
  2. run duration comfortably exceeds 2x ${MOVE_AT}s, so there is pre- AND
     post-move data
  3. MEASURED cadence is near the requested one.  If the 50ms run still
     measures ~200ms the thread could not hold it -- that is itself the
     result the sweep exists to surface, not a failed run.

THEN
  gzip $OUT/*.csv
  and copy back into data/real_workloads/ (or data/large_local/ if >100MB)
EOF
