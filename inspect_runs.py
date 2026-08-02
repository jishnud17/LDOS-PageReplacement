#!/usr/bin/env python3
"""
inspect_runs.py - sanity-check freshly collected runs BEFORE analyzing them.

Reports, per dataset: duration, pages, MEASURED cadence vs requested, how much
data sits on each side of the ground-truth relocation, and whether the latency
columns actually carry values.

stdlib only -- CloudLab nodes do not have pandas by default, and this needs to
run on the node right after collection, before anything is copied back.

    python3 ~/LDOS-PageReplacement/inspect_runs.py [outdir]
"""
import os
import re
import sys
import csv
import glob
import statistics


def move_ns_of(stderr_path):
    """Relocation timestamp (CLOCK_MONOTONIC ns) from the workload's log."""
    try:
        txt = open(stderr_path, errors="replace").read()
    except OSError:
        return None
    m = re.search(r"LDOS_GROUNDTRUTH move_ns=(\d+)", txt)
    return int(m.group(1)) if m else None


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def scan(path):
    """One pass over the CSV, collecting only what the report needs."""
    ts_min = ts_max = None
    pages = set()
    last_ts = {}          # page -> previous timestamp, for per-page bar gaps
    gaps = []
    rows = 0
    lat_i = lat_m = 0
    has_i = has_m = False
    ts_all = []

    with open(path, newline="") as fh:
        r = csv.DictReader(fh)
        cols = r.fieldnames or []
        has_i = "interval_latency_cycles" in cols
        has_m = "mean_latency_cycles" in cols
        for row in r:
            t = fnum(row.get("timestamp_ns"))
            if t is None:
                continue
            rows += 1
            ts_all.append(t)
            ts_min = t if ts_min is None or t < ts_min else ts_min
            ts_max = t if ts_max is None or t > ts_max else ts_max
            p = row.get("page_addr")
            pages.add(p)
            if p in last_ts and t > last_ts[p]:
                gaps.append(t - last_ts[p])
            last_ts[p] = t
            if has_i and (fnum(row.get("interval_latency_cycles")) or 0) > 0:
                lat_i += 1
            if has_m and (fnum(row.get("mean_latency_cycles")) or 0) > 0:
                lat_m += 1

    return dict(rows=rows, ts_min=ts_min, ts_max=ts_max, pages=len(pages),
                gaps=gaps, lat_i=lat_i, lat_m=lat_m,
                has_i=has_i, has_m=has_m, ts_all=ts_all)


def main(outdir):
    out = []
    for f in sorted(glob.glob(os.path.join(outdir, "ml_dataset_*.csv"))):
        name = os.path.basename(f)[len("ml_dataset_"):-len(".csv")]
        try:
            s = scan(f)
        except Exception as e:
            print(f"{name}: unreadable ({e})")
            continue
        if not s["rows"]:
            print(f"{name}: empty")
            continue

        dur = (s["ts_max"] - s["ts_min"]) / 1e9
        bar = statistics.median(s["gaps"]) / 1e6 if s["gaps"] else float("nan")

        req = None
        m = re.search(r"_c(\d+)", name)
        if m:
            req = int(m.group(1))
        stretch = f"{bar/req:.1f}x" if req and bar == bar else "-"

        mv = move_ns_of(os.path.join(outdir, f"{name}.stderr"))
        if mv is not None and s["ts_min"] <= mv <= s["ts_max"]:
            pre = sum(1 for t in s["ts_all"] if t < mv)
            split = f"{pre:,} / {s['rows']-pre:,}"
            mv_s = f"{(mv - s['ts_min'])/1e9:.1f}s"
        elif mv is not None:
            split = "*** MOVE OUTSIDE DATA ***"
            mv_s = f"{(mv - s['ts_min'])/1e9:+.1f}s"
        else:
            split, mv_s = "no move logged", "-"

        li = f"{100.0*s['lat_i']/s['rows']:.1f}%" if s["has_i"] else "n/a"
        lm = f"{100.0*s['lat_m']/s['rows']:.1f}%" if s["has_m"] else "n/a"

        out.append((name, f"{dur:.1f}s", f"{s['rows']:,}", f"{s['pages']:,}",
                    f"{bar:.0f}ms", stretch, mv_s, split, li, lm))

    if not out:
        print(f"no ml_dataset_*.csv under {outdir}")
        return

    hdr = ("dataset", "dur", "rows", "pages", "bar", "vs req", "move@",
           "rows pre/post", "lat_int>0", "lat_mean>0")
    w = [max(len(str(r[i])) for r in out + [hdr]) for i in range(len(hdr))]
    line = "  ".join(h.ljust(w[i]) for i, h in enumerate(hdr))
    print(line)
    print("-" * len(line))
    for r in out:
        print("  ".join(str(c).ljust(w[i]) for i, c in enumerate(r)))

    print("""
WHAT TO LOOK FOR
  vs req         near 1.0x means the thread held the requested cadence.
                 Well above 1.0 means it could not -- the uncontrolled
                 cadence the sweep exists to measure.
  rows pre/post  BOTH sides must be substantial.  A relocation with almost
                 no post-move data cannot show a transition.
  lat_int>0      percent of rows with non-zero INTERVAL latency -- the
                 useful one.  lat_mean>0 can be non-zero while this is 0,
                 which means latency was captured but never twice within
                 one bar, so no per-bar value could be formed.""")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1
         else os.path.expanduser("~/LDOS-PageReplacement/cloudlab_out"))
