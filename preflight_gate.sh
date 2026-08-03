#!/usr/bin/env bash
# preflight_gate.sh - abort-before-you-collect gate for the GUPS move sweep.
#
# ~1 minute.  Runs one short relocation and asserts everything a past failure
# has taught us to check, BEFORE the ~17-minute sweep spends node time:
#
#   1. move fired             (LDOS_GROUNDTRUTH in stderr)    [no-move bug]
#   2. geometry logged        (LDOS_GEOMETRY in stderr)
#   3. divisor/hot overlap    (deterministic, from geometry)  [ASLR coin-flip]
#   4. hot set visible        (top-128 pages >= 50% of mass)  [divisor bug]
#   5. hottest page IS hot    (top page inside a hot window)
#   6. data on both sides of the move
#   7. latency captured       (>5% of rows non-zero)          [event bug]
#   8. cadence held           (measured bar <=3x requested)    [overload bug]
#   9. write-touch channel    (soft-dirty writes on hot pages) [PEBS-independent
#                                                               label channel]
#
# Called from run_cloudlab_experiments.sh (skip with PREFLIGHT=0); also runs
# standalone.  Inherits the sweep's LDOS_* env, setting identical defaults
# when unset, so a standalone pass means the sweep's env passes too.
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/preflight_out"
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n

export LDOS_PEBS_TELEMETRY_ONLY="${LDOS_PEBS_TELEMETRY_ONLY:-1}"
export LDOS_MIN_SAMPLES_TO_TRACK="${LDOS_MIN_SAMPLES_TO_TRACK:-10}"
export LDOS_PAGE_SAMPLE_DIVISOR="${LDOS_PAGE_SAMPLE_DIVISOR:-1}"
export LDOS_SIGNAL_SAMPLE_MS="${LDOS_SIGNAL_SAMPLE_MS:-200}"
export LDOS_TOUCH_CHANNEL="${LDOS_TOUCH_CHANNEL:-1}"
export GUPS_MOVE_AT_SEC=15

[[ -x "$GUPS" ]] || { echo "PREFLIGHT FAIL: missing $GUPS -- run the sweep's STEP 0 (patch+build) first"; exit 1; }
mkdir -p "$OUT"; rm -f "$OUT"/ml_dataset_* "$OUT"/preflight.*
cd "$MANAGER_DIR" || exit 1

echo "   preflight: short GUPS run, move@15s, divisor=$LDOS_PAGE_SAMPLE_DIVISOR, ${LDOS_SIGNAL_SAMPLE_MS}ms bars"
LDOS_CSV_LABEL=preflight LD_PRELOAD=./lib/libmmap_shim.so \
    "$GUPS" "$THREADS" 1200000000 "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
    >"$OUT/preflight.stdout" 2>"$OUT/preflight.stderr" || true
[[ -f ml_dataset_preflight.csv ]] && mv ml_dataset_preflight.csv "$OUT/"

python3 - "$OUT" "$LDOS_PAGE_SAMPLE_DIVISOR" <<'PY'
import sys, os, re, csv
from collections import defaultdict

out, div = sys.argv[1], int(sys.argv[2])
fails = []
def check(ok, msg):
    print(f"   {'ok  ' if ok else 'FAIL'} {msg}")
    if not ok: fails.append(msg)

def warn(ok, msg):
    """Report, but do not block the sweep."""
    print(f"   {'ok  ' if ok else 'WARN'} {msg}")

txt = open(f"{out}/preflight.stderr", errors="replace").read()
mg = re.search(r"LDOS_GROUNDTRUTH move_ns=(\d+)", txt)
gg = re.search(r"LDOS_GEOMETRY region_base=(0x[0-9a-f]+) region_size=(\d+) "
               r"slice_bytes=(\d+) hot_bytes=(\d+)", txt)
check(bool(mg), "move fired (LDOS_GROUNDTRUTH logged)")
check(bool(gg), "geometry logged (LDOS_GEOMETRY)")

base = slice_b = hot = None
if gg:
    base = int(gg.group(1), 16); region = int(gg.group(2))
    slice_b = int(gg.group(3)); hot = int(gg.group(4))
    if div > 1:
        # The divisor tracks only pages with (vaddr>>12) % div == 0 -- a
        # (div*4KB)-period lattice.  Whether the hot window contains ANY
        # such page is decided by ASLR.  This is the check that would have
        # caught the flat r650 datasets before a single sweep ran.
        period = div * 4096
        per_slice = []
        for k in range(max(region // slice_b, 1)):
            lo = base + k * slice_b
            first = (lo + period - 1) // period * period
            per_slice.append(len(range(first, lo + hot, period)))
        check(min(per_slice) > 0,
              f"divisor={div}: hot window holds {per_slice} lattice page(s) "
              f"per slice -- 0 means the hot set CANNOT be tracked this run")

csvp = f"{out}/ml_dataset_preflight.csv"
if not os.path.exists(csvp):
    check(False, "collection produced a CSV"); sys.exit(1)

tot = defaultdict(float); rows = pre = post = lat = 0
last = {}; gaps = []; wp_pages = set()
mv = int(mg.group(1)) if mg else None
for r in csv.DictReader(open(csvp)):
    rows += 1
    a = int(r["page_addr"], 16)
    c = float(r["access_count"] or 0)
    if c > tot[a]: tot[a] = c
    t = int(r["timestamp_ns"])
    if a in last and t > last[a]: gaps.append(t - last[a])
    last[a] = t
    if mv is not None:
        pre += t < mv; post += t >= mv
    lat += float(r.get("interval_latency_cycles") or 0) > 0
    if float(r.get("touch_windows") or 0) > 0:
        wp_pages.add(a)

check(rows >= 1000, f"row volume ({rows:,} rows, need >=1000)")
if tot:
    v = sorted(tot.values(), reverse=True)
    share = sum(v[:128]) / sum(v)
    check(share >= 0.50,
          f"hot set visible: top-128 pages hold {share*100:.0f}% of access "
          f"mass (need >=50; c220g2 golden is 96, the divisor bug gave 9.7)")
    if base is not None:
        hottest = max(tot, key=tot.get)
        check((hottest - base) % slice_b < hot,
              "hottest tracked page lies inside a hot window")
if mv is not None:
    check(pre >= 100 and post >= 100,
          f"data on both sides of the move ({pre:,} pre / {post:,} post)")
if rows:
    # WARNING, not a gate.  Latency is derived from PERF_SAMPLE_WEIGHT, which
    # rides the same per-page sample crediting that was measured to be
    # instruction-mix dependent on this platform -- so it inherits the very
    # confound the touch channel exists to escape, and the study does not
    # depend on it.  It also proved node-dependent: identical CPU model
    # (Xeon Platinum 8360Y), identical code, 100% on one machine and 0% on
    # another.  See the precise_ip line in the run log.
    warn(lat / rows > 0.05,
         f"latency captured on {lat/rows*100:.0f}% of rows "
         f"(0 = no latency channel on this node; not required)")

# The gate this collection needed and did not have: correct physics with a
# broken clock.  ~300K tracked pages passed every other check while
# stretching a 50ms request to 1,599ms, collapsing the whole cadence sweep
# into one 1.6-4.9s band.
if gaps:
    gaps.sort()
    bar = gaps[len(gaps)//2] / 1e6
    req = float(os.environ.get("LDOS_SIGNAL_SAMPLE_MS", "200"))
    check(bar <= 3 * req,
          f"cadence held: {bar:.0f}ms measured vs {req:.0f}ms requested "
          f"({bar/req:.1f}x, need <=3x) at {len(tot):,} tracked pages")

if os.environ.get("LDOS_TOUCH_CHANNEL", "1") != "0":
    # The PEBS-independent label channel.  GUPS writes every hot page many
    # times per 50ms re-arm window, so with the channel alive virtually all
    # hot pages must show faults.  0 here usually means the kernel lacks
    # UFFD-WP or the column is missing (stale build).
    check(len(wp_pages) >= 64,
          f"write-touch channel: {len(wp_pages)} pages showed soft-dirty "
          f"writes (need >=64; the 128 hot pages are written every window)")

sys.exit(1 if fails else 0)
PY
rc=$?
if [[ $rc -eq 0 ]]; then
    echo "   PREFLIGHT PASS"
else
    echo "   PREFLIGHT FAIL -- do NOT run the sweep until this passes"
fi
exit $rc
