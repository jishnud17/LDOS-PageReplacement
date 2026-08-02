#!/usr/bin/env python3
"""
permutation_test.py - are the reactivity scores better than chance?

Null model: keep every detected event's page and the per-type event counts,
but reassign each event to a random bar of the same page (with a full +-W
window).  Re-score with the identical pipeline.  Repeat N times.

A signal whose real score does not clear the null's 95th percentile is
indistinguishable from autocorrelated noise around arbitrary time points --
whatever its rank in the headline table says.

    python3 permutation_test.py data/real_workloads/gups_skewed/r1.csv.gz
"""
import argparse
import sys

import numpy as np

import reactivity_analysis as ra


def scores_for(df, events, signal_cols, W, zmean, zstd, step_ms):
    """{event_type: {signal: score}} via the real scoring pipeline."""
    by_type = ra.event_triggered(df, events, signal_cols, W, zmean, zstd)
    out = {}
    for et, (nev, sig) in by_type.items():
        out[et] = {}
        for c, (mean_traj, mat) in sig.items():
            amp, agree, _, _ = ra.score(mean_traj, mat, W, step_ms)
            out[et][c] = amp * agree
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--n-perms", type=int, default=30)
    ap.add_argument("--window", type=int, default=5)
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--hot-frac", type=float, default=0.10)
    ap.add_argument("--min-hot", type=float, default=1e4)
    ap.add_argument("--smooth", type=int, default=3)
    ap.add_argument("--debounce", type=int, default=2)
    args = ap.parse_args()
    rng = np.random.default_rng(7)
    W = args.window

    df, signal_cols = ra.load_prepare(args.csv)
    label_col, circular = ra.label_column_and_exclusions("rate")
    signal_cols = [c for c in signal_cols if c not in circular]
    zmean = {c: df[c].mean() for c in signal_cols}
    zstd = {c: (df[c].std() or 1.0) for c in signal_cols}
    step_ms = ra.measure_cadence(df)

    events = ra.detect_events(df, args.hot_frac, args.smooth,
                              args.min_hot, args.debounce,
                              rate_col=label_col)
    if not events:
        sys.exit("no events detected -- nothing to test")

    real = scores_for(df, events, signal_cols, W, zmean, zstd, step_ms)

    # page boundaries, for same-page reassignment with full windows
    pages = df["page_addr"].to_numpy()
    starts = {}
    for i, p in enumerate(pages):
        starts.setdefault(p, [i, i])[1] = i
    ev_pages = [(pages[ri], et) for ri, et in events]

    null = {et: {c: [] for c in signal_cols} for et in real}
    for _ in range(args.n_perms):
        fake = []
        for p, et in ev_pages:
            lo, hi = starts[p]
            if hi - lo < 2 * W:
                continue
            fake.append((int(rng.integers(lo + W, hi - W + 1)), et))
        perm = scores_for(df, fake, signal_cols, W, zmean, zstd, step_ms)
        for et in null:
            for c in signal_cols:
                null[et][c].append(perm.get(et, {}).get(c, 0.0))

    for et, sig_scores in real.items():
        print(f"\n=== {et}  ({args.n_perms} permutations) ===")
        print(f"  {'signal':<26}{'real':>7}{'null95':>8}{'pctile':>8}  verdict")
        ranked = sorted(sig_scores, key=sig_scores.get, reverse=True)
        for c in ranked[:args.top]:
            n = np.array(null[et][c])
            n95 = np.percentile(n, 95) if len(n) else float("nan")
            pct = 100.0 * (sig_scores[c] > n).mean() if len(n) else float("nan")
            verdict = "SIGNAL" if sig_scores[c] > n95 else "~chance"
            print(f"  {c:<26}{sig_scores[c]:>7.2f}{n95:>8.2f}{pct:>7.0f}%  {verdict}")

    print("""
  'SIGNAL' = real score beats the 95th percentile of the permutation null.
  A '~chance' verdict on a headline signal means its ranking reflects
  autocorrelation, not reaction to the events.""")


if __name__ == "__main__":
    main()
