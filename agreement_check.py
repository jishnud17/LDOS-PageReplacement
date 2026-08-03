#!/usr/bin/env python3
"""
agreement_check.py - does the heuristic detector agree with ground truth?

Every ranking in results/ rests on reactivity_analysis.detect_events(), a
threshold heuristic on measured access rate.  Nothing has ever checked it
against a known answer, because until the GUPS patch landed there was no
known answer.  There is now.

For each dataset this asks three questions:

  RECALL       of the 32 pages the program actually relocated, how many did
               the detector flag as hot_to_cold near move_ns?
  FALSE ALARMS of the 96 pages that stayed hot -- same signal regime, same
               tracking history, access rate UNCHANGED by the move -- how
               many did it flag anyway?  These are the controls that matter;
               a cold background page cannot test this.
  LATENCY      for the true detections, how long after move_ns did the
               event fire?  This is the detection-latency number the study
               has been reporting from inference rather than measurement.

A detector that scores high recall AND high false alarms is firing on
everything.  One with low false alarms and low recall is asleep.  Only the
combination means anything.

    python3 agreement_check.py data/real_workloads/r650b_gups_move_*
"""
import argparse
import os
import sys

import numpy as np

import reactivity_analysis as ra
from groundtruth_labels import (read_geometry, classify,
                                HOT_TO_COLD, STAYS_HOT, COLD_TO_HOT, BACKGROUND)


def analyze(dsdir, args):
    geo = read_geometry(os.path.join(dsdir, "r1.stderr"))
    if geo is None:
        print(f"{os.path.basename(dsdir)}: no ground truth -- skipped")
        return None

    df, _ = ra.load_prepare(os.path.join(dsdir, "r1.csv.gz"))

    if args.label_by == "touch":
        # touch_windows is cumulative; its per-bar delta is the touch RATE --
        # how many 10ms windows in this bar saw a write.  This is the
        # PEBS-independent channel: no sampling, no attribution, read
        # straight from soft-dirty PTE bits.
        if "touch_windows" not in df.columns:
            print(f"{os.path.basename(dsdir)}: no touch_windows column")
            return None
        df["touch_rate"] = (df.groupby("page_addr", sort=False)["touch_windows"]
                              .diff().fillna(0.0).clip(lower=0.0))
        rate_col = "touch_rate"
    else:
        rate_col = "interval_access_rate"

    step_ms = ra.measure_cadence(df)
    tol_ns = args.tol_bars * step_ms * 1e6
    mv = geo["move_ns"]

    addr = np.array([int(a, 16) for a in df["page_addr"]])
    label = {a: classify(a, geo) for a in set(addr.tolist())}
    ts = df["timestamp_ns"].to_numpy()

    events = ra.detect_events(df, args.hot_frac, args.smooth,
                              args.min_hot, args.debounce, rate_col=rate_col)

    # page -> events of each type, as (time, type)
    per_page = {}
    for ri, et in events:
        per_page.setdefault(addr[ri], []).append((ts[ri], et))

    pages_of = {}
    for a, lb in label.items():
        pages_of.setdefault(lb, []).append(a)

    name = os.path.basename(dsdir)
    print(f"\n{'='*74}\n{name}   bar {step_ms:.0f}ms   "
          f"tolerance +-{args.tol_bars} bars ({tol_ns/1e6:.0f}ms)")

    out = {"dataset": name, "bar_ms": step_ms}
    lats = []
    for lb, want in ((HOT_TO_COLD, "hot_to_cold"),
                     (STAYS_HOT, None),
                     (COLD_TO_HOT, "cold_to_hot"),
                     (BACKGROUND, None)):
        pages = pages_of.get(lb, [])
        if not pages:
            continue
        near = far = 0
        for a in pages:
            evs = per_page.get(a, [])
            # "correct" = the expected transition type, when one is expected
            hit = [t for t, et in evs
                   if abs(t - mv) <= tol_ns and (want is None or et == want)]
            oth = [t for t, et in evs if abs(t - mv) > tol_ns]
            if hit:
                near += 1
                if want is not None:
                    lats.append((min(hit, key=lambda t: abs(t - mv)) - mv) / 1e6)
            if oth:
                far += 1
        pct = 100.0 * near / len(pages)
        role = ("RECALL" if want else "FALSE ALARM")
        print(f"  {lb:<14} n={len(pages):<5} fired at move: {near:>4}/{len(pages):<5}"
              f" ({pct:>5.1f}%)  {role:<12} elsewhere: {far}")
        out[f"{lb}_at_move_pct"] = pct
        out[f"{lb}_n"] = len(pages)

    if lats:
        lats = np.array(lats)
        print(f"  detection latency (hot_to_cold, relative to move_ns): "
              f"median {np.median(lats):+.0f}ms   "
              f"p10 {np.percentile(lats,10):+.0f}  p90 {np.percentile(lats,90):+.0f}"
              f"   [= {np.median(lats)/step_ms:+.1f} bars]")
        out["latency_ms"] = float(np.median(lats))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("datasets", nargs="+")
    ap.add_argument("--label-by", choices=["rate", "touch"], default="rate",
                    help="which hotness channel defines the events")
    ap.add_argument("--tol-bars", type=float, default=5.0,
                    help="how close to move_ns counts as 'at the move'")
    ap.add_argument("--hot-frac", type=float, default=0.10)
    ap.add_argument("--min-hot", type=float, default=1e4)
    ap.add_argument("--smooth", type=int, default=3)
    ap.add_argument("--debounce", type=int, default=2)
    args = ap.parse_args()

    # The touch channel lives on a different scale from access rate (windows
    # per bar, ~0-40, vs ~1e5), so its thresholds differ.  hot_frac must be
    # high enough that a page falling to 17% of its former touch rate reads
    # as a transition: at 0.10 the default would call 17% "still hot".
    if args.label_by == "touch":
        if args.min_hot == 1e4: args.min_hot = 1.0
        if args.hot_frac == 0.10: args.hot_frac = 0.40

    print(f"### channel = {args.label_by}  "
          f"(hot_frac={args.hot_frac}, min_hot={args.min_hot})")
    rows = [r for d in args.datasets if (r := analyze(d, args))]
    if len(rows) > 1:
        print(f"\n{'='*74}\nSUMMARY")
        print(f"  {'dataset':<28}{'bar':>7}{'recall':>9}{'false':>8}{'latency':>10}")
        for r in rows:
            print(f"  {r['dataset']:<28}{r['bar_ms']:>6.0f}ms"
                  f"{r.get('hot_to_cold_at_move_pct',0):>8.0f}%"
                  f"{r.get('stays_hot_at_move_pct',0):>7.0f}%"
                  f"{r.get('latency_ms',float('nan')):>9.0f}ms")
        print("""
  recall  = of 32 truly relocated pages, % detected at the move
  false   = of 96 pages that stayed hot, % flagged anyway
  latency = median delay from the program's relocation to detection""")


if __name__ == "__main__":
    main()
