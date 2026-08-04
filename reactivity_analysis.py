#!/usr/bin/env python3
"""
reactivity_analysis.py - Phase-1 signal reactivity discovery (LDOS PageReplacement)

Goal: find which per-page signals *react* when a page changes state (hot<->cold),
and whether they react EARLY (lead the transition) or LATE (lag it).  This is the
exploratory step that shortlists candidate features for the phase-2 predictor:
a signal that is reactive AND leading is the kind worth modeling on.

Method (event-aligned / peri-event analysis -- no model, no training):
  1. Reconstruct each page's time series (rows are 50 ms apart).
  2. Detect state-change EVENTS per page:
       - cold->hot : access rate rises across a threshold
       - hot->cold : access rate falls across a threshold
       - migration : the manager moved the page (migration_count increases)
     The rate is median-smoothed first so throttle dips don't fake a transition.
  3. For every signal, average its (z-scored) trajectory in a window around each
     event -> the "event-triggered average".  This is the shape you look at.
  4. Score each signal's reactivity:
       - amplitude   : how far it moves around the event (in std units)
       - consistency : does it move the SAME direction across events (signal vs noise)
       - lead_lag_ms : when it moves relative to t=0  (negative = leads = predictive)
       - score       : amplitude * consistency

Outputs (per input CSV):
  <name>_reactivity.csv     ranked reactivity table
  <name>_trajectories.csv   event-triggered average per signal (for plotting anywhere)
  PNG trajectory grids       only if matplotlib is installed
  terminal: ranked table + ASCII sparklines of the top signals

Usage:
  python3 reactivity_analysis.py ml_dataset_*.csv
  python3 reactivity_analysis.py --window 5 --hot-frac 0.10 ml_dataset_hot_cold.csv
"""

import argparse
import os
import sys
import numpy as np
import pandas as pd

# Columns that are identifiers/raw counters, not candidate "signals" to rank.
META_COLS = {
    "cycle", "timestamp_ns", "page_addr", "current_tier",
    "access_count", "read_count", "write_count", "migration_count",
}
STEP_MS = 50  # one exported row per 5 policy cycles = 50 ms

# Phase-1 CSVs used abbreviated signal names; the exporter now writes
# descriptive ones.  Normalize old files on load so rankings and the final
# old-vs-new comparison line up on identical names.
COLUMN_RENAMES = {
    "si":               "swing_index",
    "asi":              "accum_swing_index",
    "aroon_osc":        "aroon_oscillator",
    "adx":              "avg_directional_index",
    "plus_di":          "plus_directional_indicator",
    "minus_di":         "minus_directional_indicator",
    "gapo":             "gopalakrishnan_range_index",
    "ich_tenkan":       "ichimoku_tenkan",
    "ich_kijun":        "ichimoku_kijun",
    "ich_senkou_a":     "ichimoku_senkou_a",
    "ich_senkou_b":     "ichimoku_senkou_b",
    "ich_chikou":       "ichimoku_chikou",
    "linreg_slope":     "linear_reg_slope",
    "linreg_intercept": "linear_reg_intercept",
    "psar":             "parabolic_sar",
    "psar_dir":         "parabolic_sar_direction",
    "rwi_high":         "random_walk_index_high",
    "rwi_low":          "random_walk_index_low",
    "ravi":             "range_action_verification_index",
    "stc":              "schaff_trend_cycle",
    "stc_signal":       "schaff_trend_cycle_signal",
    "supertrend_dir":   "supertrend_direction",
    "sqn":              "system_quality_number",
    "trix":             "triple_exp_rate_of_change",
    "vhf":              "vertical_horizontal_filter",
    "recency_weighted_freq": "recency_weighted_frequency",
}

SPARK = "▁▂▃▄▅▆▇█"


def ascii_spark(traj, center_idx):
    """Render a trajectory as a unicode sparkline; '|' marks the event (t=0)."""
    lo, hi = np.min(traj), np.max(traj)
    if hi - lo < 1e-12:
        cells = ["▁"] * len(traj)
    else:
        cells = [SPARK[int((v - lo) / (hi - lo) * (len(SPARK) - 1))] for v in traj]
    cells[center_idx] = "┃"  # event marker
    return "".join(cells)


def load_prepare(path):
    """Load one CSV, sort by page then cycle, return df + list of signal columns."""
    df = pd.read_csv(path)
    df = df.rename(columns=COLUMN_RENAMES)   # normalize phase-1 abbreviated names
    df = df.sort_values(["page_addr", "cycle"]).reset_index(drop=True)
    if "touch_windows" in df.columns:
        # Cumulative counter -> per-bar rate: how many soft-dirty windows in
        # this bar saw a write.  Derived here so --label-by touch has a
        # column to threshold on, and so it is excluded as circular when it
        # IS the label (see label_column_and_exclusions).
        df["touch_rate"] = (df.groupby("page_addr", sort=False)["touch_windows"]
                              .diff().fillna(0.0).clip(lower=0.0))
    signal_cols = [c for c in df.columns
                   if c not in META_COLS and pd.api.types.is_numeric_dtype(df[c])]
    # Drop constant columns (no variation -> nothing to react).
    signal_cols = [c for c in signal_cols if df[c].std(skipna=True) > 1e-12]
    return df, signal_cols


def label_column_and_exclusions(label_by):
    """
    Which column defines events, and which columns must therefore be excluded
    from the ranking as circular.

    Events are defined by a column crossing a threshold, so THAT column and any
    near-copy of it wins by construction -- it is restating the label, not
    detecting it.  interval_access_rate has always been excluded for this
    reason (ichimoku_chikou with it, being a code-duplicate).  The same trap
    applies to latency-based labeling: if events come from
    interval_latency_cycles, then the latency columns must be excluded, not
    scored.  Getting this wrong reproduces exactly the circularity that
    circularity_check.py was written to find.
    """
    if label_by == "latency":
        return "interval_latency_cycles", {
            "interval_latency_cycles", "mean_latency_cycles"}
    if label_by == "touch":
        # Soft-dirty write-touch labels.  Only the touch columns are
        # circular here -- and critically, interval_access_rate is NOT, so
        # for the first time the rate signal can be RANKED rather than
        # assumed.  That matters: measured against ground truth, rate labels
        # flagged 100% of the pages that provably never moved, while touch
        # labels flagged 0% (results/hotness_channel_comparison.txt).
        return "touch_rate", {"touch_rate", "touch_windows"}
    return "interval_access_rate", {
        "interval_access_rate", "ichimoku_chikou"}


def detect_events(df, hot_frac, smooth_win, min_hot, debounce, rate_col=None):
    """
    Return a list of (row_index, event_type) tuples.
    Rate transitions are detected per page on a median-smoothed, per-page-normalized
    rate; migrations are detected from increases in migration_count.
    """
    events = []
    if rate_col is None:
        rate_col = "interval_access_rate"
    for _, grp in df.groupby("page_addr", sort=False):
        idx = grp.index.to_numpy()
        n = len(idx)
        if n < 2 * debounce + 2:
            continue

        # --- rate transitions ---
        if rate_col in grp:
            rate = grp[rate_col].to_numpy(dtype=float)
            page_max = rate.max()
            if page_max >= min_hot:
                # median smooth to kill single-window throttle dips
                sm = pd.Series(rate).rolling(smooth_win, center=True,
                                             min_periods=1).median().to_numpy()
                thresh = hot_frac * page_max
                active = sm > thresh
                # require `debounce` sustained samples on each side of an edge
                for i in range(1, n):
                    if active[i] and not active[i - 1]:
                        if (not active[max(0, i - debounce):i].any()
                                and active[i:i + debounce].all()):
                            events.append((idx[i], "cold_to_hot"))
                    elif active[i - 1] and not active[i]:
                        if (active[max(0, i - debounce):i].all()
                                and not active[i:i + debounce].any()):
                            events.append((idx[i], "hot_to_cold"))

        # --- migration events ---
        if "migration_count" in grp:
            mc = grp["migration_count"].to_numpy(dtype=float)
            d = np.diff(mc, prepend=mc[0])
            for i in range(n):
                if d[i] > 0:
                    events.append((idx[i], "migration"))
    return events


def zscore_arrays(df, signal_cols, znorm):
    """
    Per-signal z-scored arrays.

    znorm='global' (historical): one mean/std per signal over ALL pages and
    bars.  This systematically penalises signals with large CROSS-PAGE
    spread relative to their per-page response.  Measured on
    touch10_gups_move_c200: interval_access_rate drops cleanly at the
    relocation (median per-page jump 5,716) but the global std is 334,421 --
    inflated by the PEBS crediting artifact making control pages read 142x
    hotter -- so the response registers as 0.017 std units and the signal
    ranks #24 of 34.  Bounded oscillators (RWI, Schaff, Aroon) have
    per-page jumps comparable to their global spread and score ~1.1-1.3.
    The ranking therefore partly measures BOUNDEDNESS, not reactivity.

    znorm='page': each page normalised by its OWN mean/std, so amplitude is
    measured in units of that page's variability.  This is the standard
    convention for event-triggered averaging and removes the bias.
    """
    out = {}
    for c in signal_cols:
        v = df[c].to_numpy(dtype=float)
        if znorm == "page":
            g = df.groupby("page_addr", sort=False)[c]
            m = g.transform("mean").to_numpy(dtype=float)
            sd = g.transform("std").to_numpy(dtype=float)
            # a page with no variance in this signal contributes nothing;
            # guard the divide rather than emit inf.
            sd = np.where(~np.isfinite(sd) | (sd < 1e-12), np.nan, sd)
            out[c] = (v - m) / sd
        else:
            sd = df[c].std() or 1.0
            out[c] = (v - df[c].mean()) / sd
    return out


def event_triggered(df, events, signal_cols, W, zmean, zstd, zarr=None):
    """
    Build event-triggered z-scored trajectories.
    Returns: {event_type: {signal: (mean_traj[2W+1], per_event_matrix)}}
    """
    # position of each row within its page's series, for edge checks
    pos = df.groupby("page_addr", sort=False).cumcount().to_numpy()
    page_len = df.groupby("page_addr", sort=False)["cycle"].transform("size").to_numpy()

    by_type = {}
    for et in sorted(set(t for _, t in events)):
        ev_idx = [ri for ri, t in events if t == et]
        # keep only events with a full window on both sides
        ev_idx = [ri for ri in ev_idx
                  if pos[ri] >= W and pos[ri] <= page_len[ri] - 1 - W]
        if not ev_idx:
            continue
        sig = {}
        for c in signal_cols:
            z = (zarr[c] if zarr is not None
                 else (df[c].to_numpy(dtype=float) - zmean[c]) / zstd[c])
            mat = np.empty((len(ev_idx), 2 * W + 1))
            for r, ri in enumerate(ev_idx):
                mat[r] = z[ri - W: ri + W + 1]
            sig[c] = (np.nanmean(mat, axis=0), mat)
        by_type[et] = (len(ev_idx), sig)
    return by_type


def score(mean_traj, mat, W, step_ms=STEP_MS):
    """amplitude (std units), consistency (dir agreement), lead/lag (ms)."""
    pre = mean_traj[:W]
    baseline = np.nanmean(pre) if len(pre) else 0.0
    dev = mean_traj - baseline
    peak = int(np.nanargmax(np.abs(dev)))
    amplitude = abs(dev[peak])
    direction = np.sign(dev[peak]) or 1.0

    # consistency: across events, fraction whose move at the peak agrees in sign
    per_event_dev = mat[:, peak] - np.nanmean(mat[:, :W], axis=1)
    agree = np.mean(np.sign(per_event_dev) == direction) if len(per_event_dev) else 0.0

    # lead/lag: first offset reaching 50% of peak deviation, relative to t=0
    half = 0.5 * amplitude
    crossings = np.where(np.abs(dev) >= half)[0]
    onset = crossings[0] if len(crossings) else peak
    lead_lag_ms = (onset - W) * step_ms
    return amplitude, agree, lead_lag_ms, peak


def measure_cadence(df):
    """
    Measure the ACTUAL bar spacing from row timestamps.  Bars are nominally
    STEP_MS apart, but an overloaded policy thread (too many tracked pages)
    stretches them -- which silently invalidates fast-vs-slow signal rankings
    (every signal fires on the same coarse bar).  This happened on the first
    GUPS runs (~30x stretch); never trust a ranking without this check.
    """
    dt = df.groupby("page_addr", sort=False)["timestamp_ns"].diff().dropna()
    if len(dt) == 0:
        return float(STEP_MS)
    return float(dt.median()) / 1e6


def analyze_file(path, args, accum):
    name = os.path.splitext(os.path.basename(path))[0]
    df, signal_cols = load_prepare(path)

    label_col, circular = label_column_and_exclusions(args.label_by)
    if label_col not in df.columns:
        print(f"\n{'='*70}\n{name}: --label-by={args.label_by} needs column "
              f"'{label_col}', which this CSV does not have -- skipped.")
        if args.label_by == "latency":
            print("  (measured on Ice Lake r650: the DEFAULT ALL_LOADS event "
                  "populates PERF_SAMPLE_WEIGHT at 33-99%; 0x1cd starved "
                  "store-heavy GUPS to zero rows -- recollect with defaults)")
        return
    if args.label_by == "latency" and df[label_col].abs().max() <= 0:
        print(f"\n{'='*70}\n{name}: '{label_col}' is all zero -- the run did "
              f"not capture latency. Skipped rather than scored as noise.")
        return

    # Whatever defines the events cannot also be ranked against them.
    signal_cols = [c for c in signal_cols if c not in circular]

    zmean = {c: df[c].mean() for c in signal_cols}
    zstd = {c: (df[c].std() or 1.0) for c in signal_cols}

    events = detect_events(df, args.hot_frac, args.smooth, args.min_hot,
                           args.debounce, rate_col=label_col)
    counts = pd.Series([t for _, t in events]).value_counts().to_dict()
    print(f"\n{'='*70}\n{name}  ({len(df):,} rows, {df['page_addr'].nunique()} pages)")

    step_ms = measure_cadence(df)
    stretch = step_ms / STEP_MS
    print(f"bar cadence: measured {step_ms:.0f} ms/bar (nominal {STEP_MS} ms)")
    if stretch > 1.5:
        print(f"  *** WARNING: cadence stretched {stretch:.1f}x -- the policy thread")
        print(f"  *** could not hold {STEP_MS}ms.  Fast-vs-slow signal rankings on")
        print(f"  *** this dataset are NOT trustworthy; lead/lag uses measured bars.")

    print(f"events detected: {counts if counts else 'none'}")
    if not events:
        print("  no events -- try lowering --hot-frac or --min-hot")
        return

    zarr = zscore_arrays(df, signal_cols, args.znorm)
    by_type = event_triggered(df, events, signal_cols, args.window, zmean, zstd,
                              zarr=zarr)

    if not by_type:
        # Events were found, but every one sat too close to the start or end
        # of its page's series to fit a full +-W window, so none could be
        # scored.  Common when pages are admitted to tracking only once they
        # go hot: the cold_to_hot transition then lands in the page's first
        # few bars.  Report it rather than crashing in the writer below.
        print(f"  {len(events)} events found, but none had a full "
              f"+-{args.window}-bar window around them -- nothing to score.")
        print("  (Pages likely enter the data already transitioning; try a "
              "longer run, or --window smaller than the shortest lead-in.)")
        return

    traj_rows = []
    for et, (nev, sig) in by_type.items():
        rows = []
        for c, (mean_traj, mat) in sig.items():
            amp, agree, lead, peak = score(mean_traj, mat, args.window, step_ms)
            rows.append((c, amp, agree, lead, amp * agree))
            for k, v in enumerate(mean_traj):
                traj_rows.append((et, c, (k - args.window) * step_ms, v))
        rank = pd.DataFrame(rows, columns=["signal", "amplitude",
                                           "consistency", "lead_lag_ms", "score"])
        rank = rank.sort_values("score", ascending=False).reset_index(drop=True)
        rank.insert(0, "event", et)
        rank.insert(1, "n_events", nev)
        accum.append((name, rank))

        # terminal view: top signals for this event type
        print(f"\n  --- {et}  (n={nev}) ---  [lead<0 = reacts early]")
        print(f"  {'signal':<24}{'ampl':>6}{'consist':>9}{'lead_ms':>9}  trajectory(-{args.window*step_ms:.0f}..+{args.window*step_ms:.0f}ms)")
        for _, r in rank.head(args.top).iterrows():
            mt = sig[r["signal"]][0]
            print(f"  {r['signal']:<24}{r['amplitude']:>6.2f}{r['consistency']:>9.2f}"
                  f"{int(r['lead_lag_ms']):>9}  {ascii_spark(mt, args.window)}")

    # write per-file outputs
    out_rank = f"{name}_reactivity.csv"
    pd.concat([r for _, r in accum if _ == name]).to_csv(out_rank, index=False)
    out_traj = f"{name}_trajectories.csv"
    pd.DataFrame(traj_rows, columns=["event", "signal", "t_ms", "mean_z"]).to_csv(out_traj, index=False)
    print(f"\n  wrote {out_rank} and {out_traj}")

    # optional plots
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        for et, (nev, sig) in by_type.items():
            order = [r["signal"] for _, r in
                     accum[-1][1].query("event==@et").iterrows()]
            ncol = 4
            nrow = int(np.ceil(len(order) / ncol))
            fig, axes = plt.subplots(nrow, ncol, figsize=(3 * ncol, 2 * nrow),
                                     squeeze=False)
            t = (np.arange(2 * args.window + 1) - args.window) * step_ms
            for ax, c in zip(axes.flat, order):
                ax.plot(t, sig[c][0])
                ax.axvline(0, color="r", lw=0.8, ls="--")
                ax.set_title(c, fontsize=8)
                ax.tick_params(labelsize=6)
            for ax in axes.flat[len(order):]:
                ax.axis("off")
            fig.suptitle(f"{name}: {et} (n={nev})", fontsize=10)
            fig.tight_layout()
            fig.savefig(f"{name}_{et}.png", dpi=110)
            plt.close(fig)
        print(f"  wrote PNG trajectory grids ({name}_*.png)")
    except ImportError:
        print("  (matplotlib not installed -> skipped PNGs; use the *_trajectories.csv)")


def main():
    ap = argparse.ArgumentParser(description="Phase-1 signal reactivity analysis")
    ap.add_argument("csvs", nargs="+", help="ml_dataset_*.csv files")
    ap.add_argument("--window", type=int, default=5,
                    help="half-window in 50ms steps around each event (default 5 = +-250ms)")
    ap.add_argument("--label-by", choices=["rate", "latency", "touch"],
                    default="rate",
                    help="quantity whose threshold crossing DEFINES an event. "
                         "'touch' uses soft-dirty write windows and is the "
                         "only channel validated against ground truth "
                         "(100%% recall / 0%% false alarms vs 100%% false "
                         "alarms for rate -- see "
                         "results/hotness_channel_comparison.txt). The "
                         "labeling column and its siblings are auto-excluded.")
    ap.add_argument("--znorm", choices=["global", "page"], default="global",
                    help="normalise each signal by its global spread "
                         "(historical) or by each page's own spread. "
                         "'global' penalises unbounded signals: see "
                         "zscore_arrays().")
    ap.add_argument("--hot-frac", type=float, default=0.10,
                    help="active threshold as fraction of a page's peak rate (default 0.10)")
    ap.add_argument("--min-hot", type=float, default=1e4,
                    help="ignore pages whose peak rate is below this (default 1e4/s)")
    ap.add_argument("--smooth", type=int, default=3,
                    help="median-smoothing window (samples) to debounce throttle dips")
    ap.add_argument("--debounce", type=int, default=2,
                    help="samples a transition must be sustained to count (default 2)")
    ap.add_argument("--top", type=int, default=12,
                    help="how many signals to print per event type (default 12)")
    args = ap.parse_args()

    # Per-channel thresholds.  touch_rate lives on a windows-per-bar scale
    # (~0-40), not an accesses-per-second one (~1e5), so the rate defaults
    # would never fire: at hot_frac=0.10 a page falling to 17% of its former
    # touch rate still reads as "active".
    if args.label_by == "touch":
        if args.min_hot == 1e4:  args.min_hot = 1.0
        if args.hot_frac == 0.10: args.hot_frac = 0.40

    accum = []
    for path in args.csvs:
        if not os.path.exists(path):
            print(f"skip (not found): {path}", file=sys.stderr)
            continue
        analyze_file(path, args, accum)

    if accum:
        print(f"\n{'='*70}\nDONE. Per-file *_reactivity.csv ranked by score (amplitude*consistency).")
        print("Look for signals with high score AND negative lead_lag_ms -- those react")
        print("early and consistently, i.e. the best candidates for the phase-2 model.")


if __name__ == "__main__":
    main()
