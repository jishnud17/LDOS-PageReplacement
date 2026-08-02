#!/usr/bin/env bash
# probe_regime.sh - is the pre-move phase under-sampled, independent of the move?
#
# probe_move_artifact.sh established the ~86x jump TRACKS the relocation
# (+0.4s at both a 25s and a 50s move).  Run lengths then separate two
# superimposed effects: with total work fixed, an earlier move gives a
# shorter run, implying the workload itself runs 2.72x faster after the
# move.  That is real.  It leaves ~32x unexplained -- a measurement effect.
#
# This removes the transition entirely and compares the two REGIMES:
#
#   never  GUPS_MOVE_AT_SEC huge  -> whole run in the pre-move regime
#   always GUPS_MOVE_AT_SEC=0     -> whole run in the post-move regime
#
# No transition in either, so nothing can be blamed on the event itself.
# Reports samples/sec and the workload's own GUPS throughput for each.
#
#   both regimes similar samples/sec  -> the jump needs the transition, and
#                                        is a transient (re-check bar 1-2)
#   'never' far below 'always'        -> the pre-move phase is intrinsically
#                                        under-sampled and cannot serve as a
#                                        baseline; the workload needs its
#                                        destination region pre-touched
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/regime_probe"
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n UPDATES=1500000000

export LDOS_PEBS_TELEMETRY_ONLY=1 LDOS_MIN_SAMPLES_TO_TRACK=10
export LDOS_PAGE_SAMPLE_DIVISOR=1 LDOS_SIGNAL_SAMPLE_MS=200

[[ -x "$GUPS" ]] || { echo "missing $GUPS"; exit 1; }
mkdir -p "$OUT"; rm -f "$OUT"/ml_dataset_* "$OUT"/*.stderr "$OUT"/*.stdout
cd "$MANAGER_DIR" || exit 1

run() {   # $1=label  $2=GUPS_MOVE_AT_SEC
    printf '\n\033[1;36m== %s (GUPS_MOVE_AT_SEC=%s)\033[0m\n' "$1" "$2"
    GUPS_MOVE_AT_SEC="$2" LDOS_CSV_LABEL="$1" LD_PRELOAD=./lib/libmmap_shim.so \
        "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
        >"$OUT/$1.stdout" 2>"$OUT/$1.stderr" || true
    [[ -f "ml_dataset_$1.csv" ]] && mv "ml_dataset_$1.csv" "$OUT/"
    grep -iE "gups|throughput|elapsed" "$OUT/$1.stdout" | tail -3
    python3 - "$OUT/ml_dataset_$1.csv" <<'PY'
import sys, os, csv
from collections import defaultdict
p = sys.argv[1]
if not os.path.exists(p):
    print("   no CSV"); raise SystemExit
tot = defaultdict(float); t0 = t1 = None
for r in csv.DictReader(open(p)):
    try:
        c = float(r["access_count"] or 0); t = int(r["timestamp_ns"])
    except (TypeError, ValueError, KeyError):
        continue
    a = r["page_addr"]
    if c > tot[a]: tot[a] = c
    t0 = t if t0 is None or t < t0 else t0
    t1 = t if t1 is None or t > t1 else t1
if not tot:
    print("   empty"); raise SystemExit
dur = max((t1 - t0) / 1e9, 1e-9)
s = sum(tot.values()) / 5003
print(f"   pages={len(tot):,}  dur={dur:.0f}s  samples={s:,.0f}  "
      f"samples/s={s/dur:,.0f}")
PY
}

run never 99999
run always 0

cat <<'EOF'

  Reference: the c220g2 datasets this study is built on sustained
  17,000-29,000 samples/s.  The relocation runs measured ~1,000/s before
  their move and ~80,000/s after.
EOF
