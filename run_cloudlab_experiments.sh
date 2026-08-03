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
#   MOVE PLACEMENT: MOVE_AT=60 in a ~100s run.  (The earlier observation
#   that tracked pages "never saturate" was an artifact of the divisor=128
#   lattice -- see the divisor comment below; with divisor=1 hot pages are
#   admitted within seconds.  60s is kept anyway: it costs little and
#   guarantees a deep pre-move history plus plentiful background negatives.)
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
# Bound the tracked set by HEAT.  Measured on r650 c200: the sample-count
# distribution has a cliff -- threshold 5 keeps 95,266 pages, threshold 10
# keeps 718, and BOTH keep all 128 hot pages (background pages cluster at
# 3-9 samples; hot pages carry ~30,000).  718 pages matches the c220g2
# golden datasets (967-3,031) and restores the cadence that divisor=1
# destroyed.  The originals used 3, which sufficed on Haswell because store
# attribution saw ~3K pages; Ice Lake load attribution sees ~300K.
export LDOS_MIN_SAMPLES_TO_TRACK="${LDOS_MIN_SAMPLES_TO_TRACK:-10}"
# DIVISOR=1 matches the c220g2 originals (they bounded tracking with
# MIN_SAMPLES_TO_TRACK, not the divisor -- their tracked pages are NOT
# 512KB-aligned).  The first three r650 collections ran with divisor=128,
# which tracks only pages where (vaddr>>12)%128==0: a 512KB lattice that
# contains a hot page only if ASLR is kind (25% per run).  Verified against
# the collected data: 100% of r650 tracked pages sat on the lattice, and in
# the 3-of-8 runs whose base got lucky, the lattice hot pages were ranks
# #1-4 of ~2,350 and collapsed exactly at move_ns.  Fallback if divisor=1
# overloads the cadence: 8 (32KB lattice = 4 hot pages per slice,
# deterministically, no coin flip).  preflight_gate.sh checks this.
export LDOS_PAGE_SAMPLE_DIVISOR="${LDOS_PAGE_SAMPLE_DIVISOR:-1}"
# Soft-dirty write-touch channel, exported as touch_windows.  Shares nothing
# with PEBS -- no sampling, no DataLA attribution -- so it escapes the
# instruction-mix crediting artifact that makes the rate columns
# phase-dependent here (0.2% vs 20.7% sample share for identical page heat).
export LDOS_TOUCH_CHANNEL="${LDOS_TOUCH_CHANNEL:-1}"
# 10ms windows.  MEASURED by probe_touch_window.sh: a binary "written this
# window" bit separates hot from cold only if cold pages are idle for the
# WHOLE window, and GUPS's 10% uniform traffic keeps every page warm.
#   window   h2c/stays   GUPS rate
#    200ms      1.00       0.197    <- saturated; labels nothing
#     50ms      0.82       0.065
#     20ms      0.33       0.039
#     10ms      0.15       0.030    <- clean separation
# The 6.4x slowdown vs 200ms is the method's price, not a bug: clear_refs
# write-protects every resident PTE, so shorter windows cost more.  Both
# channels observe the same slowed workload within a run, so rate-vs-touch
# stays a fair comparison.  Runs now hit the workload stop flag
# (move_at+100s) before exhausting their updates -- fine, the time series
# is the deliverable, not the update total.
export LDOS_TOUCH_WINDOW_CYCLES="${LDOS_TOUCH_WINDOW_CYCLES:-1}"

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m   !! %s\033[0m\n' "$*"; }

mkdir -p "$OUT"
# Clear stale CSVs: the run set changes between revisions of this script, and
# a leftover from an earlier parameterisation is indistinguishable from a
# fresh one once copied back.  Two 0x1cd runs (lat_c1000/lat_c2000) survived
# exactly this way and reappeared in a later summary as zero-latency results.
# Also clears *.csv.gz: the archives from a previous collection survive an
# uncompressed-CSV wipe, and gzip then prompts "already exists" per file at
# the very end of a 17-minute run.  Answer 'n' to any of those and a stale
# archive from an older parameterisation gets copied back as if it were
# fresh -- the same trap the lat_c1000/lat_c2000 leftovers set earlier.
[[ -z "${REPLICATE_TAG:-}" ]] && rm -f "$OUT"/ml_dataset_*.csv \
    "$OUT"/ml_dataset_*.csv.gz "$OUT"/*.stderr
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

# --------------------------------------------------------------------------
if [[ "${PREFLIGHT:-1}" == "1" ]]; then
    say "STEP 0.5  preflight gate (~1 min; PREFLIGHT=0 skips)"
    bash "$MANAGER_DIR/preflight_gate.sh" || {
        echo "aborting the sweep: fix the preflight failure first."
        exit 1
    }
fi

# Provenance: enough to reconstruct WHAT ran WHERE -- the c220g2-vs-r650
# confusion was only resolvable because the inventory recorded platforms.
{
    date; hostname; uname -r
    lscpu 2>/dev/null | grep -E "Model name|Socket|Core|L3" || true
    git -C "$MANAGER_DIR" rev-parse HEAD 2>/dev/null || true
    env | grep -E "^LDOS_|^GUPS_" | sort
    echo "--- gups move semantics (where the hot set goes at the move) ---"
    grep -n -B2 -A8 "move_hotset1" \
        "$WORKLOADS/gups_hemem/gups-hotset-move.c" 2>/dev/null || true
} > "$OUT/provenance.txt" 2>&1

# helper: run one collection.  $1=label  $2..=command
# REPLICATE_TAG=_r2 appends to every dataset label so a second full pass
# coexists with the first (compare with compare_replicates.py afterwards).
collect() {
    local label="$1${REPLICATE_TAG:-}"; shift
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
        # Scan is capped for speed, so report a PERCENTAGE of what was
        # scanned.  Printing the raw count instead made every fast-cadence
        # run read "ALIVE 199xxx" -- the cap, not a property of the data --
        # which looked like a hardware ceiling on weighted samples.
        n = seen = 0
        for row in r:
            if seen >= 200000: break
            seen += 1
            try:
                if float(row["interval_latency_cycles"]) > 0: n += 1
            except (ValueError, TypeError): pass
        pct = (100.0 * n / seen) if seen else 0.0
        print("ALIVE" if n else "ZERO", f"{pct:.1f}% of {seen} rows scanned")
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
  python3 ~/LDOS-PageReplacement/inspect_runs.py
  python3 ~/LDOS-PageReplacement/check_move_window.py
  python3 ~/LDOS-PageReplacement/golden_profile_check.py $OUT/ml_dataset_gups_move*.csv
  gzip $OUT/*.csv
  and copy back into data/real_workloads/ (or data/large_local/ if >100MB)
EOF
