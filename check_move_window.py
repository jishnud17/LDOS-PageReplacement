#!/usr/bin/env python3
"""
check_move_window.py - can the ground-truth relocation actually be SCORED?

inspect_runs.py answers "is there data on both sides of the move".  That is
necessary but not sufficient.  reactivity_analysis.py scores an event by
comparing a window of W bars BEFORE it against W bars AFTER, so a page only
contributes if it was ALREADY BEING TRACKED W bars before the relocation.

Page admission is not instant: a page must accumulate LDOS_MIN_SAMPLES_TO_TRACK
samples before it appears at all, so the tracked-page count climbs steeply over
the first part of a run.  If the move lands during that climb, most pages have
no pre-move history and the event is unscoreable no matter how many rows the
run produced.

This reports, per dataset:
  - how the tracked-page count grows over the run (pick MOVE_AT after it flattens)
  - how many pages have >= W bars on BOTH sides of the move  <- the real answer

stdlib only.  Run on the node:
    python3 ~/LDOS-PageReplacement/check_move_window.py [outdir] [W]
"""
import os
import re
import sys
import csv
import glob
import statistics
from collections import defaultdict


def move_ns_of(p):
    try:
        m = re.search(r"LDOS_GROUNDTRUTH move_ns=(\d+)", open(p, errors="replace").read())
    except OSError:
        return None
    return int(m.group(1)) if m else None


def main(outdir, W):
    print(f"scoreability at W={W} bars per side\n")
    for f in sorted(glob.glob(os.path.join(outdir, "ml_dataset_gups_move*.csv"))):
        name = os.path.basename(f)[len("ml_dataset_"):-len(".csv")]
        mv = move_ns_of(os.path.join(outdir, f"{name}.stderr"))
        if mv is None:
            continue

        pre = defaultdict(set)      # page -> distinct pre-move timestamps
        post = defaultdict(set)
        first_seen = {}
        acc_pre = defaultdict(float)
        t0 = None
        with open(f, newline="") as fh:
            for row in csv.DictReader(fh):
                try:
                    t = int(row["timestamp_ns"])
                except (KeyError, TypeError, ValueError):
                    continue
                p = row["page_addr"]
                t0 = t if t0 is None or t < t0 else t0
                if p not in first_seen or t < first_seen[p]:
                    first_seen[p] = t
                if t < mv:
                    pre[p].add(t)
                    try:
                        acc_pre[p] += float(row.get("accesses") or 0)
                    except ValueError:
                        pass
                else:
                    post[p].add(t)

        if t0 is None:
            continue
        dur = (max(max(v) for v in list(pre.values()) + list(post.values()) if v) - t0) / 1e9

        # when does the tracked-page count flatten out?
        growth = []
        for frac in (0.1, 0.2, 0.3, 0.5, 0.7, 1.0):
            cut = t0 + frac * dur * 1e9
            growth.append((frac * dur, sum(1 for v in first_seen.values() if v <= cut)))

        allp = set(pre) | set(post)
        both = [p for p in allp if len(pre[p]) >= W and len(post[p]) >= W]
        # of the pages that were hottest going into the move -- the ones the
        # relocation is supposed to turn cold -- how many are scoreable?
        hot = sorted(acc_pre, key=acc_pre.get, reverse=True)[:128]
        hot_ok = sum(1 for p in hot if len(pre[p]) >= W and len(post[p]) >= W)

        print(f"{name}  ({dur:.0f}s, move at {(mv-t0)/1e9:.0f}s)")
        print("   pages tracked by t: " +
              "  ".join(f"{t:.0f}s={n}" for t, n in growth))
        print(f"   pages with >={W} bars pre-move : {sum(1 for p in allp if len(pre[p])>=W)}")
        print(f"   pages with >={W} bars BOTH sides: {len(both)}   <-- scoreable")
        print(f"   of the 128 hottest pre-move pages, scoreable: {hot_ok}")
        print()

    print("""READING THIS
  If 'scoreable' is near zero the run cannot demonstrate a transition, however
  many rows it has -- the move landed before page tracking filled in.  The fix
  is to move the relocation later (MOVE_AT), to a point after the
  'pages tracked by t' series stops climbing.""")


if __name__ == "__main__":
    a = [x for x in sys.argv[1:]]
    out = a[0] if a and not a[0].isdigit() else os.path.expanduser("~/LDOS-PageReplacement/cloudlab_out")
    W = int([x for x in a if x.isdigit()][0]) if any(x.isdigit() for x in a) else 5
    main(out, W)
