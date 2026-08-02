#!/usr/bin/env python3
"""
inspect_runs.py - sanity-check freshly collected runs BEFORE analyzing them.

Reports, per dataset: duration, pages, MEASURED cadence vs requested, how much
data sits on each side of the ground-truth relocation, and whether the latency
columns actually carry values.

Run on the CloudLab node right after run_cloudlab_experiments.sh:
    python3 ~/LDOS-PageReplacement/inspect_runs.py ~/LDOS-PageReplacement/cloudlab_out
"""
import os
import re
import sys
import glob
import pandas as pd


def move_ns_of(stderr_path):
    """Relocation timestamp (CLOCK_MONOTONIC ns) from the workload's log."""
    try:
        txt = open(stderr_path, errors="replace").read()
    except OSError:
        return None
    m = re.search(r"LDOS_GROUNDTRUTH move_ns=(\d+)", txt)
    return int(m.group(1)) if m else None


def requested_cadence(name):
    m = re.search(r"_c(\d+)", name)
    return int(m.group(1)) if m else None


def main(outdir):
    rows = []
    for f in sorted(glob.glob(os.path.join(outdir, "ml_dataset_*.csv"))):
        name = os.path.basename(f)[len("ml_dataset_"):-len(".csv")]
        try:
            d = pd.read_csv(f)
        except Exception as e:
            print(f"{name}: unreadable ({e})")
            continue
        if not len(d) or "timestamp_ns" not in d:
            print(f"{name}: empty")
            continue

        t0, t1 = d.timestamp_ns.min(), d.timestamp_ns.max()
        dur = (t1 - t0) / 1e9
        pages = d.page_addr.nunique()
        # measured bar = median per-page consecutive-row gap
        dt = d.sort_values(["page_addr", "cycle"]).groupby(
            "page_addr", sort=False).timestamp_ns.diff().dropna()
        bar = dt.median() / 1e6 if len(dt) else float("nan")

        mv = move_ns_of(os.path.join(outdir, f"{name}.stderr"))
        if mv is not None and t0 <= mv <= t1:
            pre = int((d.timestamp_ns < mv).sum())
            post = int((d.timestamp_ns >= mv).sum())
            split = f"{pre:,} / {post:,}"
            mv_s = f"{(mv - t0)/1e9:.1f}s"
        elif mv is not None:
            split = "MOVE OUTSIDE DATA"
            mv_s = f"{(mv - t0)/1e9:+.1f}s"
        else:
            split, mv_s = "no move logged", "-"

        lat_i = lat_m = "n/a"
        if "interval_latency_cycles" in d:
            lat_i = f"{(d.interval_latency_cycles > 0).mean()*100:.1f}%"
        if "mean_latency_cycles" in d:
            lat_m = f"{(d.mean_latency_cycles > 0).mean()*100:.1f}%"

        req = requested_cadence(name)
        stretch = f"{bar/req:.1f}x" if req else "-"
        rows.append((name, f"{dur:.1f}s", f"{pages:,}", f"{bar:.0f}ms",
                     stretch, mv_s, split, lat_i, lat_m))

    hdr = ("dataset", "dur", "pages", "bar", "vs req", "move@",
           "rows pre/post move", "lat_int>0", "lat_mean>0")
    w = [max(len(str(r[i])) for r in rows + [hdr]) for i in range(len(hdr))]
    line = "  ".join(h.ljust(w[i]) for i, h in enumerate(hdr))
    print(line)
    print("-" * len(line))
    for r in rows:
        print("  ".join(str(c).ljust(w[i]) for i, c in enumerate(r)))

    print("""
WHAT TO LOOK FOR
  bar / vs req   'vs req' near 1.0x means the thread held the requested
                 cadence.  Much above 1.0 means it could not -- the same
                 uncontrolled-cadence problem the sweep exists to measure.
  rows pre/post  BOTH sides must be substantial.  A relocation with almost
                 no post-move data cannot show a transition.
  lat_int>0      percent of rows with non-zero interval latency.  If this is
                 0% the run carries no usable latency regardless of what the
                 collection script reported.""")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1
         else os.path.expanduser("~/LDOS-PageReplacement/cloudlab_out"))
