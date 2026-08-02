#!/usr/bin/env python3
"""
golden_profile_check.py - does a GUPS dataset physically LOOK like GUPS?

Every structural gate (rows, move position, scoreable windows, latency) passed
on r650 datasets whose hot set was never captured at all.  This is the check
none of them performed: compare the PHYSICAL access-concentration shape of a
dataset against the c220g2 originals the study is built on.

                        c220g2 golden      divisor-bug r650
    top-128 share       96.0% / 99.6%      9.7%
    max/median count    1720x / 1918x      2.8x
    samples per second  17,743 / 29,185    98

Bands are deliberately loose: platform differences are fine, a missing hot
set is not.  For GUPS-shaped (skewed hot set) workloads only -- XSBench and
graph workloads have flatter genuine profiles.

stdlib only; reads .csv or .csv.gz.  Non-zero exit if any file fails.

    python3 golden_profile_check.py data/real_workloads/r650_*/r1.csv.gz
"""
import sys
import csv
import gzip
import os
from collections import defaultdict

BANDS = {
    "top128 share":  (0.60,   "fraction of access mass in the top 128 pages"),
    "max/median":    (100.0,  "hottest page vs median page, by access_count"),
    "samples/sec":   (1000.0, "PEBS samples surviving into tracked pages"),
    "pages":         (300,    "distinct tracked pages"),
}
PERIOD = 5003  # PEBS sample period: access_count = samples * PERIOD


def profile(path):
    op = gzip.open if path.endswith(".gz") else open
    tot = defaultdict(float)
    t0 = t1 = None
    with op(path, "rt") as f:
        for r in csv.DictReader(f):
            try:
                c = float(r["access_count"] or 0)
                t = int(r["timestamp_ns"])
            except (KeyError, TypeError, ValueError):
                continue
            a = r["page_addr"]
            if c > tot[a]: tot[a] = c
            t0 = t if t0 is None or t < t0 else t0
            t1 = t if t1 is None or t > t1 else t1
    if not tot:
        return None
    v = sorted(tot.values(), reverse=True)
    dur = max((t1 - t0) / 1e9, 1e-9)
    return {
        "top128 share": sum(v[:128]) / sum(v),
        "max/median":   v[0] / max(v[len(v) // 2], 1e-9),
        "samples/sec":  sum(v) / PERIOD / dur,
        "pages":        len(v),
    }


def main(paths):
    bad = []
    for p in paths:
        prof = profile(p)
        name = os.path.basename(os.path.dirname(p)) or os.path.basename(p)
        if prof is None:
            print(f"{name}: EMPTY"); bad.append(p); continue
        fails = [k for k, (lo, _) in BANDS.items() if prof[k] < lo]
        tag = "FAIL" if fails else "pass"
        print(f"{tag}  {name:<28} "
              f"top128={prof['top128 share']*100:5.1f}%  "
              f"max/med={prof['max/median']:8.1f}x  "
              f"samp/s={prof['samples/sec']:9,.0f}  "
              f"pages={prof['pages']:,}"
              + (f"   <- below band: {', '.join(fails)}" if fails else ""))
        if fails:
            bad.append(p)
    if bad:
        print(f"\n{len(bad)}/{len(paths)} file(s) do not look like a GUPS hot-set "
              f"workload.  Do not analyze them as one.")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1:]))
