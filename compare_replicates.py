#!/usr/bin/env python3
"""
compare_replicates.py - do two replicate runs agree?

The original study's credibility rested on r1/r2 agreeing within a few
percent on every headline number (rwi_high 2.12 vs 2.09, ...).  This makes
that check mechanical: point it at two *_reactivity.csv files produced by
reactivity_analysis.py from replicate collections of the same config.

Reports, per event type: Spearman rank correlation of scores across the
signals both runs ranked, and the top signals side by side with % deltas.

stdlib only.

    python3 compare_replicates.py A_reactivity.csv B_reactivity.csv
"""
import csv
import sys


def load(path):
    out = {}
    for r in csv.DictReader(open(path)):
        out[(r["event"], r["signal"])] = (float(r["score"]),
                                          float(r["lead_lag_ms"]))
    return out


def spearman(xs, ys):
    def ranks(v):
        order = sorted(range(len(v)), key=lambda i: v[i])
        rk = [0.0] * len(v)
        for pos, i in enumerate(order):
            rk[i] = float(pos)
        return rk
    rx, ry = ranks(xs), ranks(ys)
    n = len(xs)
    if n < 3:
        return float("nan")
    mx, my = sum(rx) / n, sum(ry) / n
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    dx = sum((a - mx) ** 2 for a in rx) ** 0.5
    dy = sum((b - my) ** 2 for b in ry) ** 0.5
    return num / (dx * dy) if dx and dy else float("nan")


def main(pa, pb):
    A, B = load(pa), load(pb)
    events = sorted({e for e, _ in A} & {e for e, _ in B})
    if not events:
        sys.exit("no common event types between the two files")

    for et in events:
        common = sorted({s for e, s in A if e == et} & {s for e, s in B if e == et})
        sa = [A[(et, s)][0] for s in common]
        sb = [B[(et, s)][0] for s in common]
        rho = spearman(sa, sb)
        print(f"\n=== {et}: {len(common)} common signals, "
              f"score rank correlation rho={rho:.3f} ===")
        flag = ("ok" if rho >= 0.8 else
                "WEAK -- replicates disagree on the ordering itself")
        print(f"    ({flag}; originals agreed within a few % on headline numbers)")
        top = sorted(common, key=lambda s: A[(et, s)][0], reverse=True)[:8]
        print(f"  {'signal':<26}{'A score':>9}{'B score':>9}{'delta%':>8}"
              f"{'A lead':>8}{'B lead':>8}")
        for s in top:
            a, la = A[(et, s)]; b, lb = B[(et, s)]
            d = 100.0 * abs(a - b) / max(abs(a), 1e-9)
            print(f"  {s:<26}{a:>9.2f}{b:>9.2f}{d:>7.0f}%{la:>8.0f}{lb:>8.0f}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
