#!/usr/bin/env python3
"""
pick_min_samples.py - choose LDOS_MIN_SAMPLES_TO_TRACK from measured data.

THE TENSION THIS RESOLVES
  divisor=128 bounded the tracked set to ~2,400 pages and held cadence
  (1.2-2.2x), but selected pages by ADDRESS -- a 512KB lattice that contained
  a hot page only when ASLR cooperated, so the hot set was usually invisible.

  divisor=1 makes the hot set fully visible (top-128 = 77% of access mass,
  max/median 8,400x, all 128 hot pages scoreable) but admits ~300,000 pages,
  and the signal thread cannot walk that many per bar: the 50ms run measured
  1,599ms (32x) and the 400ms run 4,881ms (12x).  The inventory already
  records this exact failure at 159K pages -- "rankings were cadence
  artifacts".  At 300K it is worse, and every requested cadence collapses
  into the same 1.6-4.9s band, so the sweep measures nothing.

  Address sampling cannot fix this: a divisor small enough to keep a useful
  number of hot pages (<=8, to keep >=16 of them) still admits >37K pages.

  MIN_SAMPLES_TO_TRACK selects by HEAT instead of address.  Hot pages carry
  ~31,000 samples each; background pages carry ~4.  A threshold between
  those keeps every hot page and drops the background tail -- exactly the
  pages inflating the walk -- with no ASLR dependence at all.

This reports the real trade-off curve from a collected CSV so the threshold
is measured, not guessed.  stdlib only; run on the node before gzipping.

    python3 pick_min_samples.py cloudlab_out/ml_dataset_gups_move_c200.csv
"""
import csv
import gzip
import sys
from collections import defaultdict

PERIOD = 5003          # access_count = samples * PERIOD
THRESHOLDS = [3, 5, 10, 20, 30, 50, 100, 200, 500]

# Measured page-count -> cadence-stretch observations from this study, used
# only to flag which thresholds are plausibly sustainable.
#   2,400 pages -> 1.2-2.2x ;  300,000 pages -> 12-32x
SUSTAINABLE = 10_000   # rough ceiling for near-requested cadence


def main(path):
    op = gzip.open if path.endswith(".gz") else open
    tot = defaultdict(float)
    with op(path, "rt") as f:
        for r in csv.DictReader(f):
            try:
                c = float(r["access_count"] or 0)
            except (KeyError, TypeError, ValueError):
                continue
            a = r["page_addr"]
            if c > tot[a]:
                tot[a] = c

    if not tot:
        sys.exit("no rows")

    samples = sorted((v / PERIOD for v in tot.values()), reverse=True)
    hot_cut = samples[127] if len(samples) > 128 else samples[-1]
    total_mass = sum(samples)

    print(f"{path}")
    print(f"  {len(samples):,} pages;  hottest {samples[0]:,.0f} samples, "
          f"128th {hot_cut:,.0f}, median {samples[len(samples)//2]:,.0f}\n")
    print(f"  {'min_samples':>12}{'pages kept':>12}{'top-128 kept':>14}"
          f"{'access mass':>13}   sustainable?")
    print("  " + "-" * 66)
    for t in THRESHOLDS:
        kept = [s for s in samples if s >= t]
        hot_kept = sum(1 for s in samples[:128] if s >= t)
        mass = sum(kept) / total_mass * 100 if total_mass else 0
        ok = ("yes" if len(kept) <= SUSTAINABLE else
              "NO -- too many pages to hold cadence")
        print(f"  {t:>12}{len(kept):>12,}{hot_kept:>10}/128"
              f"{mass:>12.1f}%   {ok}")

    print(f"""
  PICK the smallest threshold whose 'pages kept' is under ~{SUSTAINABLE:,}
  while 'top-128 kept' is still 128/128.  That keeps every ground-truth hot
  page and drops only the background tail that breaks the cadence.

  Then rerun with:  LDOS_MIN_SAMPLES_TO_TRACK=<pick> bash run_cloudlab_experiments.sh

  CAVEAT worth stating in the writeup: a higher threshold delays admission of
  pages that are cold early and hot later, so cold_to_hot events lose their
  pre-move history.  hot_to_cold (the relocation's 128 pages) is unaffected.""")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
