#!/usr/bin/env bash
# probe_touch_window.sh - how short must a soft-dirty window be to discriminate?
#
# THE PROBLEM THIS SIZES
#   The soft-dirty write-touch channel works mechanically (161/161 pages
#   report writes) but SATURATES at the 200ms default: post-move, pages the
#   program stopped touching read exactly the same as pages that stayed hot
#   (h2c/stays = 1.00 at every cadence collected).
#
#   Not a bug -- dynamic range.  A binary "was written this window" bit
#   discriminates only if cold pages are idle for a WHOLE window.  In GUPS
#   90/10 over 2GB, the 10% uniform traffic still gives every page ~5
#   writes/sec, so at 200ms every page is dirty and the bit is pinned at 1.
#   PEBS rate sees the same pages cool 16x, because a rate has range where a
#   bit does not.
#
#   Required window is set by the background rate: with ~5 writes/page/sec,
#   a 200ms window expects ~1 write (always dirty), 20ms expects 0.1 (dirty
#   ~10% of the time), which is the contrast the labels need.  The cost is
#   clear_refs frequency -- it resets soft-dirty process-wide and
#   write-protects every resident PTE, so shorter windows slow the workload.
#
#   This measures both sides -- discrimination AND slowdown -- per window
#   size, so the choice is made from data.
#
# ~6 minutes.
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/touchwin_probe"
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n UPDATES=900000000

export LDOS_PEBS_TELEMETRY_ONLY=1 LDOS_MIN_SAMPLES_TO_TRACK=10
export LDOS_PAGE_SAMPLE_DIVISOR=1 LDOS_SIGNAL_SAMPLE_MS=200
export GUPS_MOVE_AT_SEC=15

[[ -x "$GUPS" ]] || { echo "missing $GUPS"; exit 1; }
mkdir -p "$OUT"; rm -f "$OUT"/*
cd "$MANAGER_DIR" || exit 1

for W in 1 2 5 20; do
    printf '\n\033[1;36m== touch window %s cycles (%s ms)\033[0m\n' "$W" "$((W*10))"
    LDOS_TOUCH_WINDOW_CYCLES="$W" LDOS_CSV_LABEL="tw$W" \
        LD_PRELOAD=./lib/libmmap_shim.so \
        "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
        >"$OUT/tw$W.stdout" 2>"$OUT/tw$W.stderr" || true
    [[ -f "ml_dataset_tw$W.csv" ]] && mv "ml_dataset_tw$W.csv" "$OUT/"
    grep -E "^Elapsed|^GUPS" "$OUT/tw$W.stdout" | tr '\n' '  '; echo
    python3 - "$OUT/ml_dataset_tw$W.csv" "$OUT/tw$W.stderr" <<'PY'
import sys, os, csv, re
sys.path.insert(0, os.path.expanduser("~/LDOS-PageReplacement"))
from collections import defaultdict
from groundtruth_labels import read_geometry, classify
csvp, errp = sys.argv[1], sys.argv[2]
if not os.path.exists(csvp):
    print("   no CSV"); raise SystemExit
geo = read_geometry(errp)
if not geo:
    print("   no ground truth"); raise SystemExit
mv = geo["move_ns"]
pre = defaultdict(list); post = defaultdict(list); last = {}
for r in csv.DictReader(open(csvp)):
    a = int(r["page_addr"], 16)
    try:
        tw = float(r["touch_windows"] or 0); t = int(r["timestamp_ns"])
    except (KeyError, TypeError, ValueError):
        continue
    d = tw - last.get(a, tw); last[a] = tw
    (pre if t < mv else post)[classify(a, geo)].append(d)
def m(dd, k):
    v = dd.get(k, []); return sum(v)/len(v) if v else 0.0
h_pre, s_pre = m(pre, "hot_to_cold"), m(pre, "stays_hot")
h, s, b = m(post,"hot_to_cold"), m(post,"stays_hot"), m(post,"background")
print(f"   post-move touch/bar:  h2c={h:.3f}  stays={s:.3f}  bkgd={b:.3f}")
print(f"   h2c/stays = {h/s if s else 0:.2f}   "
      f"(1.00 = saturated, ~0 = clean separation)")
print(f"   h2c drop across move  = {h/h_pre if h_pre else 0:.2f}x")
PY
done

cat <<'EOF'

READING THIS
  Pick the LONGEST window whose h2c/stays is clearly below ~0.6 -- that is
  the coarsest setting that still separates relocated pages from ones that
  stayed hot.  Then weigh it against 'Elapsed' above: shorter windows clear
  soft-dirty more often and slow the workload, and a run that is 5x slower
  is a different experiment.

  If NO window separates them, the limit is the workload, not the channel:
  GUPS's 10% uniform traffic keeps every page warm.  The fix is then to
  dilute the background -- a larger region (EXPT=34/35) at the same update
  rate cuts per-page background traffic proportionally -- rather than to
  keep shortening the window.
EOF
