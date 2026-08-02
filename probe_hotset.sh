#!/usr/bin/env bash
# probe_hotset.sh - find a GUPS hot-set size whose hot pages are VISIBLE to
# PEBS on this machine.  Short runs, one per candidate size.
#
# WHY THIS EXISTS
#   The original GUPS datasets were collected on c220g2 (Haswell), which the
#   manager samples by STORE attribution.  r650 is Ice Lake and uses LOAD
#   attribution.  GUPS at the original 2^19 = 512KB hot set is entirely
#   cache-resident, so on Ice Lake its hot accesses never reach the sampled
#   memory path: the r650 runs captured ~100 samples/s against ~18,000/s on
#   c220g2, and the tracked pages were background traffic scattered over the
#   whole 2GB region -- top-128 pages held 9.7% of accesses instead of 96%.
#   The hot set was invisible, so there was no hot/cold ground truth to label.
#
#   The fix is a hot set larger than last-level cache (r650: ~54MB L3), so hot
#   accesses actually go to DRAM.  This probes candidate sizes cheaply rather
#   than spending ~17 minutes on a full sweep to find out.
#
# Usage:  bash probe_hotset.sh [loghot ...]        # default: 19 24 26 28
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/hotset_probe"
THREADS=4 EXPT=31 ELT=8 HUGE=n
UPDATES=400000000          # short -- we only need the concentration profile

export LDOS_PEBS_TELEMETRY_ONLY=1
export LDOS_MIN_SAMPLES_TO_TRACK=3
export LDOS_PAGE_SAMPLE_DIVISOR=128
export LDOS_SIGNAL_SAMPLE_MS=200
export GUPS_MOVE_AT_SEC=99999          # no relocation during a probe

cd "$MANAGER_DIR" || exit 1
mkdir -p "$OUT"; rm -f "$OUT"/ml_dataset_*.csv

for LH in "${@:-19 24 26 28}"; do
    printf '\n\033[1;36m== loghot=%s  (%s MB total hot set)\033[0m\n' \
        "$LH" "$(( (1 << LH) / 1048576 ))"
    LDOS_CSV_LABEL="hot$LH" LD_PRELOAD=./lib/libmmap_shim.so \
        "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LH" "$HUGE" \
        >"$OUT/hot$LH.stdout" 2>"$OUT/hot$LH.stderr" || true
    [[ -f "ml_dataset_hot$LH.csv" ]] && mv "ml_dataset_hot$LH.csv" "$OUT/"
    python3 - "$OUT/ml_dataset_hot$LH.csv" <<'PY'
import sys, csv, os
from collections import defaultdict
p = sys.argv[1]
if not os.path.exists(p):
    print("   no CSV produced"); raise SystemExit
tot = defaultdict(float); t0 = t1 = None
with open(p) as f:
    for r in csv.DictReader(f):
        a = r["page_addr"]
        try: c = float(r["access_count"] or 0)
        except ValueError: continue
        if c > tot[a]: tot[a] = c
        t = int(r["timestamp_ns"])
        t0 = t if t0 is None or t < t0 else t0
        t1 = t if t1 is None or t > t1 else t1
if not tot:
    print("   empty"); raise SystemExit
v = sorted(tot.values(), reverse=True); s = sum(v)
dur = max((t1 - t0) / 1e9, 1e-9)
print(f"   pages={len(v):,}  samples={s/5003:,.0f}  samples/s={s/5003/dur:,.0f}")
print(f"   top32={sum(v[:32])/s*100:.1f}%  top128={sum(v[:128])/s*100:.1f}%  "
      f"max/median={v[0]/v[len(v)//2]:.1f}x")
PY
done

cat <<'EOF'

READING THIS
  The c220g2 runs this study is built on showed top128 ~= 96% and ~18,000
  samples/s.  A hot set is VISIBLE when top128 is far above ~10% and
  max/median is in the hundreds or thousands -- not ~2.8x, which is what an
  invisible (cache-resident) hot set looks like: uniform background traffic
  only.  Use the smallest loghot that clears that bar.
EOF
