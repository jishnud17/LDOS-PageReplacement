#!/usr/bin/env bash
# run_diagnosis.sh - ONE command that closes the attribution investigation.
#
# Established: capture-side counters, workload throughput, and tracked-page
# counts are identical across regimes, yet per-page credit to the SAME hot
# addresses differs ~100x depending only on which quarter of the hot set the
# workload touches.  Every aggregated view sits downstream of a filter, so
# none of them can say where the un-credited samples went.
#
# This uses the new LDOS_PEBS_RAW_DUMP (every drained sample, logged before
# any filter) to get the kernel's actual address stream in both regimes,
# bins it against the workload geometry, and prints a verdict.  Also runs
# probe_attribution.sh for the exported-CSV view of the same runs.
#
# ~6 minutes total.  Paste the whole output back.
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/diagnosis"
THREADS=4 EXPT=31 ELT=8 LOGHOT=19 HUGE=n UPDATES=1200000000

export LDOS_PEBS_TELEMETRY_ONLY=1 LDOS_MIN_SAMPLES_TO_TRACK=10
export LDOS_PAGE_SAMPLE_DIVISOR=1 LDOS_SIGNAL_SAMPLE_MS=500

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }

say "STEP 0  rebuild with raw-dump + lost-record support"
cd "$MANAGER_DIR" || exit 1
make 2>&1 | tail -5
[[ -f lib/libmmap_shim.so ]] || { echo "BUILD FAILED -- paste the errors above"; exit 1; }
[[ -x "$GUPS" ]] || { echo "missing $GUPS"; exit 1; }
mkdir -p "$OUT"; rm -f "$OUT"/*

for spec in "never 99999" "always 0"; do
    set -- $spec
    say "STEP 1  $1 regime (GUPS_MOVE_AT_SEC=$2), raw dump on"
    GUPS_MOVE_AT_SEC="$2" LDOS_CSV_LABEL="$1" \
        LDOS_PEBS_RAW_DUMP="$OUT/$1.raw.csv" \
        LD_PRELOAD=./lib/libmmap_shim.so \
        "$GUPS" "$THREADS" "$UPDATES" "$EXPT" "$ELT" "$LOGHOT" "$HUGE" \
        >"$OUT/$1.stdout" 2>"$OUT/$1.stderr" || true
    [[ -f "ml_dataset_$1.csv" ]] && mv "ml_dataset_$1.csv" "$OUT/"
    grep -o "pebs\[[^]]*\]" "$OUT/$1.stderr" | tail -1
done

say "STEP 2  where did the kernel's samples actually land?"
python3 - "$OUT" <<'PY'
import csv
import os
import re
import sys
from collections import defaultdict

out = sys.argv[1]
PAGE = 4096

for name in ("never", "always"):
    txt = open(f"{out}/{name}.stderr", errors="replace").read()
    g = re.search(r"LDOS_GEOMETRY region_base=(0x[0-9a-f]+) region_size=(\d+) "
                  r"slice_bytes=(\d+) hot_bytes=(\d+)", txt)
    if not g:
        print(f"{name}: no geometry"); continue
    base, region = int(g.group(1), 16), int(g.group(2))
    slice_b, hot = int(g.group(3)), int(g.group(4))
    q = hot // 4

    bands = defaultdict(int)
    rw = defaultdict(int)
    n = 0
    with open(f"{out}/{name}.raw.csv") as f:
        rdr = csv.reader(f)
        next(rdr, None)
        for row in rdr:
            try:
                w = int(row[1]); a = int(row[2], 16)
            except (ValueError, IndexError):
                continue
            n += 1
            rw["w" if w else "r"] += 1
            if a == 0:
                k = "addr==0"
            else:
                off = a - base
                if off < 0 or off >= region:
                    k = "OUT OF REGION"
                else:
                    x = off % slice_b
                    k = ("hot q1   [0,32K)" if x < q else
                         "hot rest [32K,128K)" if x < hot else
                         "dest     [128K,160K)" if x < hot + q else
                         "background")
            bands[k] += 1

    # exported-CSV credit for the same run
    cred = defaultdict(float)
    csvp = f"{out}/ml_dataset_{name}.csv"
    if os.path.exists(csvp):
        for r in csv.DictReader(open(csvp)):
            try:
                c = float(r["access_count"] or 0)
            except (TypeError, ValueError):
                continue
            a = r["page_addr"]
            if c > cred[a]: cred[a] = c
    credited = sum(cred.values()) / 5003

    print(f"\n--- {name}: {n:,} raw samples "
          f"(r={rw['r']:,} w={rw['w']:,})   CSV credited {credited:,.0f} ---")
    for k in ("hot q1   [0,32K)", "hot rest [32K,128K)", "dest     [128K,160K)",
              "background", "OUT OF REGION", "addr==0"):
        if bands.get(k):
            print(f"   {k:<22} {bands[k]:>10,}  ({bands[k]/n*100:5.1f}%)")

print("""
READING THIS
  The workload sends 90% of accesses to its hot window in BOTH regimes.
  - If 'never' shows the hot bands holding ~90% of RAW samples while its
    CSV credit is ~100x lower than 'always': the kernel delivered the
    right addresses and the loss is INSIDE the shim (merge/export path).
  - If 'never' shows hot-band raw samples themselves missing (mass in
    OUT-OF-REGION or addr==0): the kernel/PEBS delivered wrong or no
    DataLA for those accesses, and the OUT-OF-REGION addresses printed
    above say what it reported instead.
  - lost= in the pebs[...] lines above is the kernel's own drop count,
    now visible for the first time.""")
PY

say "STEP 3  exported-view probe (threshold=1) for the same comparison"
bash "$MANAGER_DIR/probe_attribution.sh"

say "DONE -- paste everything above"
