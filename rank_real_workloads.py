#!/usr/bin/env python3
"""
rank_real_workloads.py - signal rankings over REAL workloads only.

Writes three kinds of output:

  results/real_workload_signal_ranking.txt        the aggregate across all real
                                                  datasets (hand-maintained prose
                                                  + this script's Appendix A table)
  results/real_workloads/<wl>/<ds>_ranking.txt    one standalone ranking per
                                                  dataset, filed next to that
                                                  dataset's reactivity CSV
  results/real_workload_cold_to_hot_ranking.txt   promotion-only ranking, ALL
  results/real_workload_hot_to_cold_ranking.txt   demotion-only ranking, ALL
                                                  real datasets that have that
                                                  event type -- never averaged
                                                  with the other direction

Why the last two exist: the per-dataset and aggregate rankings average a
signal's cold_to_hot and hot_to_cold scores together.  A circularity check
(circularity_check.py) found that random_walk_index_high correlates with the
raw access rate at r=0.90-1.00 on several datasets and that a naive delta_rate
baseline beats it outright on hot_to_cold in 4 of 5 large datasets, while
rwi_high beats that same baseline by 25-35% on cold_to_hot.  Averaging the two
directions hides that split.  These files don't hide it.

Synthetic workloads are excluded on purpose: they are hand-built access patterns
measured with a different instrument (userfaultfd, every access counted) than the
real runs (PEBS sampling).  See the aggregate doc for what that changes.

Lead handling: reactivity_analysis.py reports lead = (onset - W)*step_ms.  A value
of exactly -W*step means |dev| never fell below half-peak anywhere in the window
-- no clean onset was found, NOT a real lead.  Those are excluded, not averaged.

Usage:  python3 rank_real_workloads.py
"""
import os
import glob
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
EXCLUDE = {"interval_access_rate", "ichimoku_chikou"}  # circular / code-duplicate
W = 5  # half-window used by reactivity_analysis.py

# Per-dataset interpretation notes.  These are findings, not decoration -- three
# of these datasets are easy to over-read from the numbers alone.
NOTES = {
    "gups_hotset_move_r1": """\
Paired with r2.  The two replicates agree to within 0.02 on every top-5 score
and produce an IDENTICAL top-5 ordering -- the study's cleanest reproducibility
evidence.  Hot set relocates at t=15s, so both event types are well populated.""",
    "gups_hotset_move_r2": """\
Paired with r1.  The two replicates agree to within 0.02 on every top-5 score
and produce an IDENTICAL top-5 ordering -- the study's cleanest reproducibility
evidence.  Hot set relocates at t=15s, so both event types are well populated.""",
    "gups_skewed_r1": """\
OUTLIER DATASET -- read the ordering with care.  rwi_low wins at a perfect 1.00
consistency, but so do five signals that rank nowhere else in the study, and
rwi_HIGH does not make the top 10.  Two structural causes: (a) the hot region is
static, so pages only ever cool -- one event type, a clean monotonic decay that
almost any trend indicator detects; (b) 96 events is the smallest real sample, so
scores are inflated relative to the large-N datasets.  This is "everything works
when the transition is easy", not a re-ranking.""",
    "gups_skewed_r2": """\
OUTLIER DATASET -- read the ordering with care.  rwi_low wins at a perfect 1.00
consistency, but so do five signals that rank nowhere else in the study, and
rwi_HIGH does not make the top 10.  Two structural causes: (a) the hot region is
static, so pages only ever cool -- one event type, a clean monotonic decay that
almost any trend indicator detects; (b) 96 events is the smallest real sample, so
scores are inflated relative to the large-N datasets.  This is "everything works
when the transition is easy", not a re-ranking.""",
    "gapbs_pr_twitter": """\
The only large-N dataset where rwi_LOW falls out of the top 3.  PageRank sweeps
the whole graph every iteration, so per-page rate changes are gradual and the
longer RWI window wins over the short one.  Also the only dataset anywhere in the
study where trix places in a top 10.""",
    "gapbs_bc_twitter": """\
*** DO NOT QUOTE THESE AMPLITUDES ***  11 events per type.  The 17.08 and the
wall of 1.00 consistencies are small-sample inflation, not a stronger effect --
with 11 events, "all 11 agreed" is roughly a coin-flip run away from chance, and
z-scoring against a near-flat series exaggerates deviation.  The ORDERING carries
some weight (rwi_high #1 again, same top four as PageRank); the magnitudes carry
none.  Directional support only.""",
    "xsbench": """\
THE MOST TRUSTWORTHY SINGLE DATASET: 129,411 events, ~6x the next largest, and
the only non-graph non-synthetic workload.  Its top 4 are exactly the four Tier-1
signals from the aggregate ranking, and the 2/3/4 spread is inside noise --
consistent with those three carrying complementary rather than competing
information.  The parabolic SAR family places here and only here.""",
    "liblinear": """\
rwi_high's strongest large-N showing anywhere (3.64, roughly 2x its median across
real datasets).  liblinear allocates 20+ separate regions rather than one large
array, so pages belong to distinct phases of the solver and transitions between
them are sharper than in a single-array workload.""",
}


def load():
    """Load every real-workload reactivity CSV, remembering its folder."""
    frames = []
    for f in sorted(glob.glob(os.path.join(RES, "real_workloads", "*",
                                           "*_reactivity.csv"))):
        ds = os.path.basename(f).replace("ml_dataset_", "").replace("_reactivity.csv", "")
        d = pd.read_csv(f)
        d = d[~d.signal.isin(EXCLUDE)].copy()
        d["dataset"] = ds
        d["folder"] = os.path.dirname(f)
        frames.append(d)
    df = pd.concat(frames, ignore_index=True)
    df["table"] = df.dataset + "/" + df.event

    # window-edge leads are "no onset found", not leads
    edge = df.groupby("table").lead_lag_ms.min().abs()
    is_edge = df.apply(lambda r: r.lead_lag_ms < 0
                       and abs(r.lead_lag_ms) >= edge[r.table] - 1e-6, axis=1)
    df["clean_lead"] = df.lead_lag_ms.where(~is_edge)
    df["rank"] = df.groupby("table")["score"].rank(ascending=False, method="min")
    return df


def write_per_workload(df):
    """One ranking file per dataset, written into that dataset's results folder."""
    for ds, sub in df.groupby("dataset"):
        events = sorted(sub.event.unique())
        counts = {e: int(sub[sub.event == e].n_events.iloc[0]) for e in events}

        # overall = mean across this dataset's event types
        per = sub.groupby("signal").agg(
            amp=("amplitude", "mean"), cons=("consistency", "mean"),
            score=("score", "mean"),
        ).sort_values("score", ascending=False)

        out = os.path.join(sub.folder.iloc[0], f"{ds}_ranking.txt")
        L = []
        L.append("=" * 78)
        L.append(f"SIGNAL RANKING -- {ds}")
        L.append("LDOS PageReplacement -- heuristics-analysis branch")
        L.append("Generated by rank_real_workloads.py")
        L.append("=" * 78)
        L.append("")
        L.append("Events scored:  " + "   ".join(f"{e}={n}" for e, n in counts.items()))
        L.append(f"Total:          {sum(counts.values())} events across "
                 f"{len(events)} event type(s)")
        L.append("")
        L.append("score = amplitude x consistency, averaged over this dataset's event")
        L.append("types.  amplitude is in std-dev units of the z-scored signal;")
        L.append("consistency is the fraction of individual events moving the SAME")
        L.append("direction.  interval_access_rate and ichimoku_chikou are excluded as")
        L.append("circular (they ARE the series events are defined from).")
        L.append("")
        if ds in NOTES:
            L.append("-" * 78)
            L.append("NOTE ON THIS DATASET")
            L.append("-" * 78)
            L.append(NOTES[ds])
            L.append("")
        L.append("-" * 78)
        L.append("FULL RANKING -- ALL SIGNALS")
        L.append("-" * 78)
        L.append(f"  {'#':<4}{'signal':<34}{'amp':>7}{'cons':>7}{'score':>8}")
        for i, (s, r) in enumerate(per.iterrows(), 1):
            L.append(f"  {i:<4}{s:<34}{r.amp:>7.2f}{r.cons:>7.2f}{r.score:>8.2f}")
        L.append("")

        # per-event-type breakdown -- the averaged view can hide a split
        L.append("-" * 78)
        L.append("BREAKDOWN BY EVENT TYPE (top 5 each)")
        L.append("-" * 78)
        for e in events:
            ev = sub[sub.event == e].sort_values("score", ascending=False)
            L.append(f"\n  {e}  ({counts[e]} events)")
            L.append(f"    {'#':<4}{'signal':<34}{'amp':>7}{'cons':>7}"
                     f"{'score':>8}{'lead_ms':>9}")
            for i, (_, r) in enumerate(ev.head(5).iterrows(), 1):
                lead = "n/a" if pd.isna(r.clean_lead) else f"{r.clean_lead:.0f}"
                L.append(f"    {i:<4}{r.signal:<34}{r.amplitude:>7.2f}"
                         f"{r.consistency:>7.2f}{r.score:>8.2f}{lead:>9}")
        L.append("")
        L.append("  lead_ms: negative = signal moved BEFORE the transition.  'n/a' means")
        L.append("  the onset search hit the window edge (+-5 bars), i.e. no clean onset")
        L.append("  was found -- NOT a lead of that size.")
        L.append("")
        L.append("=" * 78)
        L.append("Aggregate across all 8 real datasets:")
        L.append("  results/real_workload_signal_ranking.txt")
        L.append("=" * 78)

        with open(out, "w") as fh:
            fh.write("\n".join(L) + "\n")
        print(f"wrote {os.path.relpath(out, HERE)}")


EVENT_LABEL = {
    "cold_to_hot": ("COLD_TO_HOT (PROMOTION)", "results/real_workload_cold_to_hot_ranking.txt"),
    "hot_to_cold": ("HOT_TO_COLD (DEMOTION)", "results/real_workload_hot_to_cold_ranking.txt"),
}


def write_by_event_type(df):
    """One ranking per event type, pooled across every real dataset that has it.
    Never mixes cold_to_hot and hot_to_cold scores together."""
    for event, sub in df.groupby("event"):
        label, relpath = EVENT_LABEL.get(event, (event.upper(), f"results/real_{event}_ranking.txt"))
        datasets = sorted(sub.dataset.unique())
        n_tables = sub.table.nunique()

        agg = sub.groupby("signal").agg(
            top3=("rank", lambda s: int((s <= 3).sum())),
            top5=("rank", lambda s: int((s <= 5).sum())),
            med_amp=("amplitude", "median"),
            med_cons=("consistency", "median"),
            med_score=("score", "median"),
        ).sort_values(["top3", "top5", "med_score"], ascending=False)

        out = os.path.join(RES, os.path.basename(relpath))
        L = []
        L.append("=" * 78)
        L.append(f"{label} RANKING -- ALL REAL WORKLOADS")
        L.append("LDOS PageReplacement -- heuristics-analysis branch")
        L.append("Generated by rank_real_workloads.py")
        L.append("=" * 78)
        L.append("")
        L.append(f"{n_tables} datasets scored this event type (synthetic excluded,")
        L.append("hot_to_cold and cold_to_hot scores are NOT averaged together here):")
        L.append("")
        for ds in datasets:
            n = int(sub[sub.dataset == ds].n_events.iloc[0])
            L.append(f"  {ds:<26}{n:>8,} events")
        L.append("")
        L.append("score = amplitude x consistency.  top3/top5 = datasets out of "
                 f"{n_tables} where the")
        L.append("signal placed that high.  interval_access_rate and ichimoku_chikou")
        L.append("are excluded as circular (they ARE the series events are defined from).")
        L.append("")
        if event == "hot_to_cold":
            rwih_rank = list(agg.index).index("random_walk_index_high") + 1
            L.append("-" * 78)
            L.append("CIRCULARITY CAVEAT")
            L.append("-" * 78)
            L.append(f"random_walk_index_high (#{rwih_rank} below) correlates with the raw access")
            L.append("rate at r=0.90-1.00 on liblinear/BC/PageRank/xsbench, and a naive delta_rate")
            L.append("baseline (current rate minus previous rate, no normalization) beats it")
            L.append("outright on THIS event type in 4 of 5 large datasets tested (liblinear,")
            L.append("xsbench, gapbs_pr, gapbs_bc).  See circularity_check.py.  Read its placement")
            L.append("below as measuring closeness to the rate on this event type, not as a")
            L.append("finding independent of how the event is defined.")
            L.append("")
            L.append("random_walk_index_low, which ranks ABOVE it here, is NOT a rate copy --")
            L.append("median correlation with the raw rate is -0.80 to -0.04 across every real")
            L.append("dataset tested, so its ranking does not carry the same caveat.")
            L.append("")
        L.append("-" * 78)
        L.append("FULL RANKING -- ALL SIGNALS")
        L.append("-" * 78)
        L.append(f"  {'#':<4}{'signal':<34}{'top3':>5}{'top5':>6}{'amp':>8}"
                 f"{'cons':>7}{'score':>8}")
        for i, (s, r) in enumerate(agg.iterrows(), 1):
            L.append(f"  {i:<4}{s:<34}{r.top3:>5.0f}{r.top5:>6.0f}{r.med_amp:>8.2f}"
                     f"{r.med_cons:>7.2f}{r.med_score:>8.2f}")
        L.append("")

        L.append("-" * 78)
        L.append("PER-DATASET SCORES, TOP 8 SIGNALS")
        L.append("-" * 78)
        top8 = agg.head(8).index.tolist()
        L.append(f"  {'signal':<34}" + "".join(f"{ds[:10]:>12}" for ds in datasets))
        for s in top8:
            row = sub[sub.signal == s].set_index("dataset")["score"]
            L.append(f"  {s:<34}" + "".join(
                f"{row.get(ds, float('nan')):>12.2f}" for ds in datasets))
        L.append("")

        L.append("=" * 78)
        L.append("Companion file (the other event type):")
        other = "hot_to_cold" if event == "cold_to_hot" else "cold_to_hot"
        L.append(f"  {EVENT_LABEL[other][1]}")
        L.append("Averaged-across-both-directions view:")
        L.append("  results/real_workload_signal_ranking.txt")
        L.append("=" * 78)

        with open(out, "w") as fh:
            fh.write("\n".join(L) + "\n")
        print(f"wrote {os.path.relpath(out, HERE)}")


def print_aggregate(df):
    """Appendix A of the aggregate doc."""
    agg = df.groupby("signal").agg(
        top3=("rank", lambda s: int((s <= 3).sum())),
        top5=("rank", lambda s: int((s <= 5).sum())),
        med_amp=("amplitude", "median"),
        med_cons=("consistency", "median"),
        med_score=("score", "median"),
    ).sort_values(["top3", "top5", "med_score"], ascending=False)
    n = df.table.nunique()
    print(f"\nAGGREGATE -- {df.dataset.nunique()} real datasets, {n} tables\n")
    print(f"  {'signal':<34}{'top3':>5}{'top5':>6}{'amp':>8}{'cons':>7}{'score':>8}")
    for s, r in agg.iterrows():
        print(f"  {s:<34}{r.top3:>5.0f}{r.top5:>6.0f}{r.med_amp:>8.2f}"
              f"{r.med_cons:>7.2f}{r.med_score:>8.2f}")


if __name__ == "__main__":
    d = load()
    write_per_workload(d)
    write_by_event_type(d)
    print_aggregate(d)
