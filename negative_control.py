#!/usr/bin/env python3
"""
negative_control.py - false-attribution floor for ground-truth relocation events.

Ground-truth scoring credits hot_to_cold / cold_to_hot events near move_ns to
the relocation.  But event detection also fires on workloads with NO move
(the inventory records 96 hot_to_cold events on static gups_skewed).  So: on
a no-move dataset, place FAKE move times and count events that land within
the attribution window of each -- events that would have been credited to a
relocation that never happened.

That count is the floor.  A real move dataset's event count near its true
move_ns must clear it by a wide margin, or the "ground truth" is noise.

    python3 negative_control.py data/real_workloads/gups_skewed/r1.csv.gz
"""
import argparse
import sys

import numpy as np

import reactivity_analysis as ra


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", help="a NO-MOVE dataset (e.g. gups_skewed r1)")
    ap.add_argument("--n-controls", type=int, default=20,
                    help="number of fake move times (evenly spaced)")
    ap.add_argument("--window", type=int, default=5,
                    help="attribution window, +-bars around the fake move")
    ap.add_argument("--hot-frac", type=float, default=0.10)
    ap.add_argument("--min-hot", type=float, default=1e4)
    ap.add_argument("--smooth", type=int, default=3)
    ap.add_argument("--debounce", type=int, default=2)
    args = ap.parse_args()

    df, _ = ra.load_prepare(args.csv)
    events = ra.detect_events(df, args.hot_frac, args.smooth,
                              args.min_hot, args.debounce)
    step_ms = ra.measure_cadence(df)
    win_ns = args.window * step_ms * 1e6
    ts = df["timestamp_ns"].to_numpy()
    t0, t1 = ts.min(), ts.max()
    dur_s = (t1 - t0) / 1e9

    types = sorted(set(t for _, t in events))
    ev_ts = {et: np.array([ts[ri] for ri, t in events if t == et])
             for et in types}

    print(f"{args.csv}")
    print(f"  {len(df):,} rows, {dur_s:.0f}s, measured bar {step_ms:.0f}ms")
    print(f"  events on this NO-MOVE dataset: "
          + ", ".join(f"{et}={len(v)}" for et, v in ev_ts.items()))
    print(f"  attribution window: +-{args.window} bars = +-{win_ns/1e6:.0f}ms")

    # fake moves in the middle 80% so the window never falls off the ends
    fakes = np.linspace(t0 + 0.1 * (t1 - t0), t1 - 0.1 * (t1 - t0),
                        args.n_controls)
    print(f"\n  events falsely attributable to {args.n_controls} fake moves:")
    worst = 0
    for et, v in ev_ts.items():
        hits = np.array([(np.abs(v - f) <= win_ns).sum() for f in fakes])
        worst = max(worst, int(hits.max()))
        print(f"    {et:<14} mean {hits.mean():5.1f}   median {int(np.median(hits)):3d}"
              f"   max {int(hits.max()):3d}")

    print(f"""
  FLOOR: up to {worst} events per type can appear "at the move" by chance.
  A ground-truth move dataset (128 relocated hot pages) must show event
  counts near its true move_ns well above this before its events can be
  called relocation-caused.""")


if __name__ == "__main__":
    main()
