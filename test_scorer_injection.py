#!/usr/bin/env python3
"""
test_scorer_injection.py - end-to-end test of reactivity_analysis.py itself.

Collection has been validated hard; the scorer it feeds has been trusted
untested.  This builds a synthetic dataset where the right answer is KNOWN:

    10 pages hot->cold at bar 120, 10 pages cold->hot at bar 120, 10 flat
    sig_lead   steps 3 bars BEFORE every transition   -> lead_lag = -150ms
    sig_sync   steps exactly AT the transition        -> lead_lag =    0ms
    sig_lag    steps 3 bars AFTER                     -> lead_lag = +150ms
    sig_noise  iid gaussian                           -> low consistency

then runs the real reactivity_analysis.py on it and asserts the output:
event counts exact, lead/lag signs and magnitudes right, noise ranked last.
If this fails, no ranking the scorer has ever produced can be trusted.

    python3 test_scorer_injection.py        (exit 0 = scorer verified)
"""
import csv
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BARS, W, STEP_NS = 240, 5, 50_000_000
FLIP = 120                      # transition bar
HOT, COLD = 1e5, 300.0          # above / below detect_events' min_hot=1e4


def build(path):
    random.seed(7)
    cols = ["cycle", "timestamp_ns", "page_addr", "interval_access_rate",
            "migration_count", "sig_lead", "sig_sync", "sig_lag", "sig_noise"]
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for p in range(30):
            kind = ("h2c", "c2h", "flat")[p // 10]
            addr = f"0x7f{p:02x}0000000"
            for b in range(BARS):
                if kind == "h2c":
                    rate = HOT if b < FLIP else COLD
                elif kind == "c2h":
                    rate = COLD if b < FLIP else HOT
                else:
                    rate = COLD
                trans = kind != "flat"
                w.writerow([
                    b, 1_000_000_000_000 + b * STEP_NS, addr, rate, 0,
                    1.0 if trans and b >= FLIP - 3 else 0.0,   # leads by 3 bars
                    1.0 if trans and b >= FLIP else 0.0,       # coincident
                    1.0 if trans and b >= FLIP + 3 else 0.0,   # lags by 3 bars
                    random.gauss(0, 1),
                ])


def main():
    ok = True
    def check(cond, msg):
        nonlocal ok
        print(f"  {'ok  ' if cond else 'FAIL'} {msg}")
        ok = ok and cond

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "synthetic_injection.csv")
        build(src)
        r = subprocess.run(
            [sys.executable, os.path.join(HERE, "reactivity_analysis.py"),
             src, "--window", str(W)],
            cwd=td, capture_output=True, text=True)
        out = os.path.join(td, "synthetic_injection_reactivity.csv")
        if not os.path.exists(out):
            print(r.stdout[-2000:]); print(r.stderr[-2000:])
            sys.exit("FAIL: scorer produced no reactivity CSV")

        rows = list(csv.DictReader(open(out)))
        by = {}   # (event, signal) -> row
        for row in rows:
            by[(row["event"], row["signal"])] = row

        for et in ("hot_to_cold", "cold_to_hot"):
            n = {int(row["n_events"]) for (e, _), row in by.items() if e == et}
            check(n == {10}, f"{et}: exactly 10 events scored (got {n})")
            lead = by.get((et, "sig_lead")); sync = by.get((et, "sig_sync"))
            lag = by.get((et, "sig_lag"));  noise = by.get((et, "sig_noise"))
            check(all([lead, sync, lag, noise]), f"{et}: all 4 signals present")
            if not all([lead, sync, lag, noise]):
                continue
            check(float(lead["lead_lag_ms"]) <= -100,
                  f"{et}: sig_lead leads ({lead['lead_lag_ms']}ms, want -150)")
            check(abs(float(sync["lead_lag_ms"])) <= 50,
                  f"{et}: sig_sync coincident ({sync['lead_lag_ms']}ms, want 0)")
            check(float(lag["lead_lag_ms"]) >= 100,
                  f"{et}: sig_lag lags ({lag['lead_lag_ms']}ms, want +150)")
            for s in (lead, sync, lag):
                check(float(s["consistency"]) >= 0.9,
                      f"{et}: {s['signal']} consistency "
                      f"{float(s['consistency']):.2f} >= 0.9")
            floor = min(float(s["score"]) for s in (lead, sync, lag))
            check(float(noise["score"]) < floor,
                  f"{et}: noise scores below every true signal "
                  f"({float(noise['score']):.2f} < {floor:.2f})")

    print("\nSCORER " + ("VERIFIED" if ok else "BROKEN -- rankings untrustworthy"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
