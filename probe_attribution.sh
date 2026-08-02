#!/usr/bin/env bash
# probe_attribution.sh - where does the shim think the samples went?
#
# Established so far:
#   - PEBS CAPTURE is identical in both regimes (3.80M vs 3.43M samples by
#     cycle 400, same throttle count, same 128 tracked pages)
#   - workload throughput is identical (GUPS 0.2636 vs 0.2696)
#   - yet ATTRIBUTION to those 128 hot pages differs 107x
#
# So ~99% of captured samples are being credited somewhere other than the
# hot pages in the pre-move regime.  The CSV cannot show where, because
# LDOS_MIN_SAMPLES_TO_TRACK=10 hides every page that never reaches 10.
#
# This sets the threshold to 1 so EVERY sampled page appears, then bins the
# access mass by offset within each thread's slice.  90% of this workload's
# accesses go to the hot window by construction, so:
#
#   hot-window share ~90% in both   -> attribution is fine; the bug is in
#                                      admission or in the per-bar export
#   ~90% in 'always', low in 'never'-> the shim credits hot addresses to the
#                                      wrong pages in one regime; a
#                                      layout-dependent attribution bug that
#                                      would affect every dataset
#
# Short runs with coarse bars: we need the address distribution, not cadence.
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/attrib_probe"
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n UPDATES=400000000

export LDOS_PEBS_TELEMETRY_ONLY=1
export LDOS_MIN_SAMPLES_TO_TRACK=1     # <-- show every sampled page
export LDOS_PAGE_SAMPLE_DIVISOR=1
export LDOS_SIGNAL_SAMPLE_MS=2000      # coarse: few bars, small CSV

[[ -x "$GUPS" ]] || { echo "missing $GUPS"; exit 1; }
mkdir -p "$OUT"; rm -f "$OUT"/ml_dataset_* "$OUT"/*.stderr "$OUT"/*.stdout
cd "$MANAGER_DIR" || exit 1

for spec in "never 99999" "always 0"; do
    set -- $spec
    printf '\n\033[1;36m== %s regime (GUPS_MOVE_AT_SEC=%s)\033[0m\n' "$1" "$2"
    GUPS_MOVE_AT_SEC="$2" LDOS_CSV_LABEL="$1" LD_PRELOAD=./lib/libmmap_shim.so \
        "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
        >"$OUT/$1.stdout" 2>"$OUT/$1.stderr" || true
    [[ -f "ml_dataset_$1.csv" ]] && mv "ml_dataset_$1.csv" "$OUT/"
    grep -o "pebs\[samples=[0-9]*" "$OUT/$1.stderr" | tail -1
    python3 - "$OUT/ml_dataset_$1.csv" "$OUT/$1.stderr" <<'PY'
import sys, os, re, csv
from collections import defaultdict
csvp, errp = sys.argv[1], sys.argv[2]
if not os.path.exists(csvp):
    print("   no CSV"); raise SystemExit
g = re.search(r"LDOS_GEOMETRY region_base=(0x[0-9a-f]+) region_size=(\d+) "
              r"slice_bytes=(\d+) hot_bytes=(\d+)",
              open(errp, errors="replace").read())
if not g:
    print("   no geometry"); raise SystemExit
base, region = int(g.group(1), 16), int(g.group(2))
slice_b, hot = int(g.group(3)), int(g.group(4))

tot = defaultdict(float)
for r in csv.DictReader(open(csvp)):
    try: c = float(r["access_count"] or 0)
    except (TypeError, ValueError): continue
    a = r["page_addr"]
    if c > tot[a]: tot[a] = c

mass = defaultdict(float); pages = defaultdict(int)
for a, c in tot.items():
    off = int(a, 16) - base
    if off < 0 or off >= region:
        k = "outside region"
    else:
        w = off % slice_b
        k = ("hot window [0,128K)" if w < hot else
             "destination [128K,160K)" if w < hot + hot // 4 else
             "background")
    mass[k] += c; pages[k] += 1
T = sum(mass.values()) or 1.0
print(f"   {len(tot):,} pages tracked (threshold=1)")
for k in ("hot window [0,128K)", "destination [128K,160K)",
          "background", "outside region"):
    if pages[k]:
        print(f"     {k:<26} {mass[k]/T*100:>6.2f}% of access mass  "
              f"({pages[k]:,} pages)")
PY
done

cat <<'EOF'

  The workload sends 90% of its accesses to the hot window by construction
  (hot_num < 90 in do_gups).  Any regime where the hot window holds far
  less than that is not measuring what the workload is doing.
EOF
