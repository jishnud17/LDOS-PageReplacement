#!/usr/bin/env bash
# analyze_oor.sh - name the OUT-OF-REGION addresses in the raw dumps.
#
# run_diagnosis.sh showed 76-97% of kernel-delivered samples point outside
# the workload's 2GB region, and that hot-window delivery differs 100x by
# regime.  The OOR addresses themselves say what is being sampled instead:
#   ~0x4xxxxx / matches gups symbols  -> the workload's own globals (BSS)
#   ~0x5555.. or near brk             -> heap: the shim's records/stats
#   ~0x7ffx..                         -> stacks
#   near the perf mmap pages          -> the collector sampling itself
#
# Reads the existing diagnosis/*.raw.csv -- no new runs.  Seconds.
set -uo pipefail

MANAGER_DIR="$HOME/LDOS-PageReplacement"
GUPS="$HOME/benchmarks/workloads/gups_hemem/gups-hotset-move"
OUT="$MANAGER_DIR/diagnosis"

echo "== gups binary symbols (for matching against sample addresses)"
readelf -sW "$GUPS" 2>/dev/null | grep -E "move_hotset1|thread_gups|hot_start|OBJECT.*GLOBAL" | head -12
echo
echo "== binary type (PIE symbols need the runtime base; non-PIE match directly)"
readelf -h "$GUPS" 2>/dev/null | grep Type

python3 - "$OUT" <<'PY'
import csv
import re
import sys
from collections import Counter

out = sys.argv[1]
PAGE = 4096

for name in ("never", "always"):
    txt = open(f"{out}/{name}.stderr", errors="replace").read()
    g = re.search(r"LDOS_GEOMETRY region_base=(0x[0-9a-f]+) region_size=(\d+)", txt)
    base, region = int(g.group(1), 16), int(g.group(2))

    pages = Counter()
    wr = Counter()
    n_oor = 0
    with open(f"{out}/{name}.raw.csv") as f:
        rdr = csv.reader(f); next(rdr, None)
        for row in rdr:
            try:
                w = int(row[1]); a = int(row[2], 16)
            except (ValueError, IndexError):
                continue
            if a == 0 or base <= a < base + region:
                continue
            n_oor += 1
            p = a & ~(PAGE - 1)
            pages[p] += 1
            wr[p] += w

    print(f"\n--- {name}: {n_oor:,} OOR samples over {len(pages):,} distinct pages ---")
    print(f"    region was [{hex(base)}, {hex(base+region)})")
    top = pages.most_common(20)
    covered = sum(c for _, c in top)
    print(f"    top 20 pages hold {covered/n_oor*100:.1f}% of OOR mass")
    print(f"    {'page':>16} {'samples':>12} {'%':>6} {'writes%':>8}   offset vs region")
    for p, c in top:
        rel = (p - base)
        where = (f"{rel/2**30:+.2f} GB" if abs(rel) < 1 << 44 else "far")
        print(f"    {hex(p):>16} {c:>12,} {c/n_oor*100:>5.1f}% "
              f"{wr[p]/c*100:>7.1f}%   {where}")

    # coarse map: how many distinct OOR pages per 256MB bucket
    buckets = Counter((p >> 28) for p in pages)
    print(f"    address-space spread: {len(buckets)} x 256MB buckets; "
          f"top: " + ", ".join(f"{hex(b << 28)}({c})"
                               for b, c in buckets.most_common(4)))
PY
