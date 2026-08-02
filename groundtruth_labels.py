#!/usr/bin/env python3
"""
groundtruth_labels.py - label GUPS pages from the PROGRAM's geometry.

No access rate, no threshold, no signal is consulted.  Labels come from the
workload's own arithmetic, read out of the LDOS_GROUNDTRUTH stderr line.

WHAT THE WORKLOAD DOES (gups-hotset-move.c, the hot-index draw):

    index1 = hot_start + (lfsr % hotsize);
    if (move_hotset1 && index1 < hotsize / 4)
        index1 += hotsize;          // only the FIRST QUARTER relocates

So within each thread's slice, in ELEMENT space:

    [0,          hotsize/4)   hot before, cold after   -> HOT_TO_COLD
    [hotsize/4,  hotsize)     hot before, hot after    -> STAYS_HOT
    [hotsize,    hotsize*5/4) cold before, hot after   -> COLD_TO_HOT
    everything else                                       BACKGROUND

With the r650 geometry (hot_bytes=131072, elt_size=8, 4 threads, 4KB pages)
that is 32 / 96 / 32 pages respectively.

STAYS_HOT is the control that matters: those pages are hot throughout, are
tracked from the first second, and their access rate is UNCHANGED by the
move -- a detector that fires on them at move_ns is producing false
positives in the same signal regime as the true events, which no cold
background page can test.

    python3 groundtruth_labels.py data/real_workloads/r650b_gups_move_c200
"""
import gzip
import os
import re
import sys

PAGE = 4096
HOT_TO_COLD, STAYS_HOT, COLD_TO_HOT, BACKGROUND = (
    "hot_to_cold", "stays_hot", "cold_to_hot", "background")


def read_geometry(stderr_path):
    """(move_ns, base, region, slice_bytes, hot_bytes, elt_size) or None."""
    txt = open(stderr_path, errors="replace").read()
    m = re.search(r"LDOS_GROUNDTRUTH move_ns=(\d+) region_base=(0x[0-9a-f]+) "
                  r"region_size=(\d+) slice_bytes=(\d+) hot_bytes=(\d+)", txt)
    if not m:
        return None
    g = re.search(r"LDOS_GEOMETRY .*elt_size=(\d+)", txt)
    return dict(move_ns=int(m.group(1)), base=int(m.group(2), 16),
                region=int(m.group(3)), slice_bytes=int(m.group(4)),
                hot_bytes=int(m.group(5)),
                elt_size=int(g.group(1)) if g else 8)


def classify(addr, geo):
    """Label one page address from geometry alone."""
    off = addr - geo["base"]
    if off < 0 or off >= geo["region"]:
        return BACKGROUND
    within = off % geo["slice_bytes"]          # byte offset inside the slice
    hot = geo["hot_bytes"]                     # bytes covered by the hot set
    q = hot // 4                               # the relocating first quarter
    # A page is 4KB; classify by the byte range it covers, so a page counts
    # as belonging to a band if it OVERLAPS it (bands are 32KB, page-aligned
    # here, so overlap and containment agree -- but do not assume that).
    lo, hi = within, within + PAGE
    if lo < q:
        return HOT_TO_COLD
    if lo < hot:
        return STAYS_HOT
    if lo < hot + q:
        return COLD_TO_HOT
    return BACKGROUND


def label_dataset(dsdir):
    """{page_addr_int: label}, plus geometry.  Reads only the CSV's addresses."""
    geo = read_geometry(os.path.join(dsdir, "r1.stderr"))
    if geo is None:
        return None, None
    import csv
    labels = {}
    with gzip.open(os.path.join(dsdir, "r1.csv.gz"), "rt") as f:
        for r in csv.DictReader(f):
            a = int(r["page_addr"], 16)
            if a not in labels:
                labels[a] = classify(a, geo)
    return labels, geo


def main(dsdir):
    labels, geo = label_dataset(dsdir)
    if labels is None:
        sys.exit(f"no LDOS_GROUNDTRUTH in {dsdir}/r1.stderr")
    counts = {}
    for v in labels.values():
        counts[v] = counts.get(v, 0) + 1
    hot = geo["hot_bytes"]
    print(f"{dsdir}")
    print(f"  base={hex(geo['base'])}  slice={geo['slice_bytes']:,}B  "
          f"hot={hot:,}B  elt={geo['elt_size']}B")
    print(f"  bands per slice: hot_to_cold [0,{hot//4:,})  "
          f"stays_hot [{hot//4:,},{hot:,})  "
          f"cold_to_hot [{hot:,},{hot + hot//4:,})")
    print(f"  expected pages across {geo['region']//geo['slice_bytes']} slices: "
          f"{hot//4//PAGE*4} / {(hot-hot//4)//PAGE*4} / {hot//4//PAGE*4}")
    print(f"  TRACKED pages by label:")
    for k in (HOT_TO_COLD, STAYS_HOT, COLD_TO_HOT, BACKGROUND):
        print(f"    {k:<14} {counts.get(k, 0):>6}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
