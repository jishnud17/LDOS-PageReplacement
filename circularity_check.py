#!/usr/bin/env python3
"""
circularity_check.py - is the top of the signal ranking a real finding?

Events are DEFINED by the access rate crossing a threshold.  The two top-ranked
signals are both normalized derivatives of that same access rate:

    swing_index            = (v[n-1] - v[n-2]) / mean|dv|
    random_walk_index_high = max_k (v[n-1] - v[n-k]) / (ATR_k * sqrt(k))

So they are guaranteed to move when an event fires.  Two tests separate "these
signals earn their rank" from "any rate derivative would score the same":

  TEST 1  Correlation with the raw rate, across the whole per-page series.
          High |r| means the signal is close to a relabeled copy of the rate.

  TEST 2  Head-to-head against naive baselines scored through the SAME
          event-triggered pipeline, as extra signals:
            delta_rate      = v[n-1] - v[n-2]           (plain first difference)
            delta_rate_norm = delta_rate / mean|dv|     (= swing_index / 50)
            abs_delta_rate  = |delta_rate|
          If delta_rate ranks alongside rwi_high, the sophistication buys
          nothing.  If rwi_high clearly beats it, the volatility normalization
          and the multi-window search are doing real work.

Usage:  python3 circularity_check.py [dataset.csv.gz ...]
        (no args = every real-workload dataset it can find)
"""
import os
import sys
import glob
import numpy as np
import pandas as pd

import reactivity_analysis as ra

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = "interval_access_rate"
TOP = ["random_walk_index_high", "random_walk_index_low", "swing_index",
       "recency_weighted_frequency"]
BASELINES = ["delta_rate", "delta_rate_norm", "abs_delta_rate"]


class Args:
    """Same defaults reactivity_analysis.py uses."""
    window = 5
    hot_frac = 0.10
    min_hot = 1e4
    smooth = 3
    debounce = 2


def add_baselines(df):
    """Naive rate derivatives, computed per page exactly as the C code would."""
    g = df.groupby("page_addr", sort=False)[RATE]
    d = g.diff()
    df["delta_rate"] = d.fillna(0.0)
    df["abs_delta_rate"] = df["delta_rate"].abs()
    # normalize by each page's mean |dv| -- the swing_index denominator
    scale = df.groupby("page_addr", sort=False)["abs_delta_rate"].transform("mean")
    df["delta_rate_norm"] = np.where(scale > 1e-12, df["delta_rate"] / scale, 0.0)
    return df


def test1_correlation(df, name):
    """Per-page Pearson r between each top signal and the raw rate."""
    print(f"\n  TEST 1 -- correlation with {RATE} (per page, then median)")
    print(f"    {'signal':<30}{'median r':>10}{'|r|>0.9':>10}{'pages':>8}")
    rows = []
    for c in TOP + BASELINES:
        if c not in df:
            continue
        rs = []
        for _, grp in df.groupby("page_addr", sort=False):
            a, b = grp[c].to_numpy(float), grp[RATE].to_numpy(float)
            if len(a) < 5 or np.std(a) < 1e-12 or np.std(b) < 1e-12:
                continue
            rs.append(np.corrcoef(a, b)[0, 1])
        if not rs:
            continue
        rs = np.array(rs)
        med = float(np.nanmedian(rs))
        frac = float(np.mean(np.abs(rs) > 0.9))
        rows.append((c, med, frac, len(rs)))
        print(f"    {c:<30}{med:>10.2f}{frac:>10.1%}{len(rs):>8}")
    return rows


def test2_baseline(df, name):
    """Score baselines through the real event-triggered pipeline."""
    signal_cols = [c for c in df.columns
                   if c not in ra.META_COLS and pd.api.types.is_numeric_dtype(df[c])]
    signal_cols = [c for c in signal_cols if df[c].std(skipna=True) > 1e-12]

    a = Args()
    events = ra.detect_events(df, a.hot_frac, a.smooth, a.min_hot, a.debounce)
    if not events:
        print("    no events -- skipped")
        return []
    step_ms = ra.measure_cadence(df)
    zmean = {c: df[c].mean() for c in signal_cols}
    zstd = {c: (df[c].std() or 1.0) for c in signal_cols}
    by_type = ra.event_triggered(df, events, signal_cols, a.window, zmean, zstd)

    out = []
    for et, (nev, sig) in by_type.items():
        rows = []
        for c, (mean_traj, mat) in sig.items():
            amp, agree, lead, _ = ra.score(mean_traj, mat, a.window, step_ms)
            rows.append((c, amp, agree, amp * agree))
        rank = pd.DataFrame(rows, columns=["signal", "amp", "cons", "score"])
        rank = rank[~rank.signal.isin({RATE, "ichimoku_chikou"})]
        rank = rank.sort_values("score", ascending=False).reset_index(drop=True)
        rank["rank"] = rank.index + 1

        print(f"\n  TEST 2 -- {et}  (n={nev}, baselines scored as extra signals)")
        print(f"    {'rank':<6}{'signal':<30}{'amp':>7}{'cons':>7}{'score':>8}")
        show = rank[rank.signal.isin(TOP + BASELINES)]
        for _, r in show.iterrows():
            mark = "  <-- baseline" if r.signal in BASELINES else ""
            print(f"    {int(r['rank']):<6}{r.signal:<30}{r.amp:>7.2f}"
                  f"{r.cons:>7.2f}{r.score:>8.2f}{mark}")
        out.append((et, nev, rank))
    return out


def main(paths):
    for p in paths:
        name = os.path.basename(os.path.dirname(p)) or os.path.basename(p)
        print(f"\n{'='*74}\n{name}\n{'='*74}")
        df = pd.read_csv(p)
        df = df.rename(columns=ra.COLUMN_RENAMES)
        df = df.sort_values(["page_addr", "cycle"]).reset_index(drop=True)
        if RATE not in df:
            print("  no interval_access_rate column -- skipped")
            continue
        df = add_baselines(df)
        print(f"  {len(df):,} rows, {df.page_addr.nunique():,} pages")
        test1_correlation(df, name)
        test2_baseline(df, name)


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        args = sorted(glob.glob(os.path.join(HERE, "data", "real_workloads",
                                             "*", "*.csv.gz")))
    main(args)
