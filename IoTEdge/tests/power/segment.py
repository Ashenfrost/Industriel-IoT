#!/usr/bin/env python3
"""segment.py - find sessions / sleep phases from the current alone.

For recordings where the logic-port markers were not recorded. Works on the
binned CSV from bins.py (start_s, avg_mA, max_mA, digital_or).

  python segment.py sessions test2_b100ms.csv [--json out.json]
  python segment.py phases   test4_b100ms.csv [--json out.json]

sessions: a session is where the current stays clearly above the idle level
          (> --factor x idle median, default 2.5) for at least --min-s
          (0.5 s); gaps shorter than --gap-s (2 s) are bridged. Reports each
          session's duration, charge and average, and the idle current.
phases:   test 4 (power_sleep_test): cuts the curve into plateaus by level,
          then names them in the known order after each boot:
          boot -> P1 busy -> P2 idle -> P3 light sleep -> P4 light sleep +
          radio off -> P5 idle + radio off -> deep sleep.

Current-based detection is an estimate: a session's first/last few hundred
ms at near-idle current are counted as idle. Use markers when possible.
"""
import argparse
import csv
import json
import statistics as st


def load(path):
    t, i = [], []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            t.append(float(row["start_s"]))
            i.append(float(row["avg_mA"]))
    dt = (t[1] - t[0]) if len(t) > 1 else 1.0
    return t, i, dt


def runs_of(mask):
    """[(start_idx, end_idx_exclusive)] where mask is True."""
    out, start = [], None
    for k, m in enumerate(mask):
        if m and start is None:
            start = k
        elif not m and start is not None:
            out.append((start, k))
            start = None
    if start is not None:
        out.append((start, len(mask)))
    return out


def sessions(a):
    t, i, dt = load(a.csv)
    idle = st.median(i)
    # A fixed threshold between the idle and the active level works when
    # sessions take up most of the time (test 3: the median is no longer idle).
    thr = a.threshold if a.threshold is not None else a.factor * idle
    mask = [v > thr for v in i]
    rs = runs_of(mask)
    # bridge short gaps, then drop short blips
    merged = []
    for s, e in rs:
        if merged and (s - merged[-1][1]) * dt < a.gap_s:
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append((s, e))
    merged = [(s, e) for s, e in merged if (e - s) * dt >= a.min_s]
    # skip a session cut by the start/end of the recording
    merged = [(s, e) for s, e in merged if s > 0 and e < len(i)]

    in_s = [False] * len(i)
    rows = []
    for s, e in merged:
        for k in range(s, e):
            in_s[k] = True
        q = sum(i[s:e]) * dt                       # mA*s = mC
        rows.append({"start_s": t[s], "dur_s": (e - s) * dt, "q_mc": q,
                     "avg_ma": q / ((e - s) * dt), "peak_bin_ma": max(i[s:e])})
    idle_vals = [v for v, m in zip(i, in_s) if not m]
    idle_avg = sum(idle_vals) / len(idle_vals) if idle_vals else float("nan")
    total = sum(i) * dt / (len(i) * dt)

    print(f"# {a.csv}\n")
    print(f"recording median {idle:.1f} mA, session threshold {thr:.1f} mA, bins {dt * 1000:.0f} ms\n")
    print("| # | Start (s) | Duration (s) | Charge (mC) | Avg (mA) | Peak bin (mA) |")
    print("|---|---|---|---|---|---|")
    for k, r in enumerate(rows):
        print(f"| {k} | {r['start_s']:.1f} | {r['dur_s']:.1f} | {r['q_mc']:.1f} | {r['avg_ma']:.1f} | {r['peak_bin_ma']:.0f} |")

    def ms(key):
        v = [r[key] for r in rows]
        return (st.mean(v), st.stdev(v) if len(v) > 1 else 0.0) if v else (float("nan"), 0.0)

    gaps = [rows[k + 1]["start_s"] - rows[k]["start_s"] for k in range(len(rows) - 1)]
    print("\n| Summary | Value |\n|---|---|")
    print(f"| Recording average | {total:.2f} mA |")
    print(f"| Sessions found | {len(rows)} |")
    print(f"| Session duration | {ms('dur_s')[0]:.2f} s (sd {ms('dur_s')[1]:.2f}) |")
    print(f"| Session charge | {ms('q_mc')[0]:.1f} mC (sd {ms('q_mc')[1]:.1f}) |")
    print(f"| Session average | {ms('avg_ma')[0]:.1f} mA |")
    print(f"| Time between session starts | {st.mean(gaps):.1f} s |" if gaps else "| Time between session starts | - |")
    print(f"| **Idle (outside sessions)** | **{idle_avg:.2f} mA** |")
    summ = {"recording_avg_ma": total, "sessions": len(rows), "session_dur_s": ms("dur_s")[0],
            "session_q_mc": ms("q_mc")[0], "session_q_sd_mc": ms("q_mc")[1],
            "idle_ma": idle_avg, "period_s": st.mean(gaps) if gaps else None, "rows": rows}
    if a.json:
        json.dump(summ, open(a.json, "w"), indent=2)


# ---------------------------------------------------------------- phases

LEVELS = [            # (label, low mA, high mA); first match wins
    ("boot", 90.0, 1e9),
    ("hi", 53.3, 90.0),      # P1 busy (~55)
    ("mid", 44.0, 53.3),     # P2 idle (~48)
    ("p5", 33.0, 44.0),      # P5 idle + radio off (~40)
    ("sleep", 11.0, 33.0),   # P3 light sleep, modem on (~16-22)
    ("floor", 0.0, 11.0),    # P4 / deep sleep (~7.5)
]
ORDER = ["boot", "P1", "P2", "P3", "P4", "P5", "deep"]
NAMES = {"boot": "boot + modem attach", "P1": "P1 ESP busy, modem idle",
         "P2": "P2 ESP idle, modem idle", "P3": "P3 ESP light sleep, modem idle",
         "P4": "P4 ESP light sleep, radio off", "P5": "P5 ESP idle, radio off",
         "deep": "deep sleep, radio off"}


def classify(v):
    for name, lo, hi in LEVELS:
        if lo <= v < hi:
            return name
    return "?"


def phases(a):
    t, i, dt = load(a.csv)
    # 1 s rolling mean to classify plateaus (light sleep has short bursts)
    w = max(1, int(round(1.0 / dt)))
    sm = []
    acc = 0.0
    for k, v in enumerate(i):
        acc += v
        if k >= w:
            acc -= i[k - w]
        sm.append(acc / min(k + 1, w))
    lab = [classify(v) for v in sm]
    # runs of equal label, drop runs shorter than 3 s (transitions)
    runs = []
    s = 0
    for k in range(1, len(lab) + 1):
        if k == len(lab) or lab[k] != lab[s]:
            runs.append((lab[s], s, k))
            s = k
    runs = [r for r in runs if (r[2] - r[1]) * dt >= 3.0]
    # merge neighbours with the same label after dropping transitions
    m = []
    for r in runs:
        if m and m[-1][0] == r[0]:
            m[-1] = (r[0], m[-1][1], r[2])
        else:
            m.append(r)

    # name in order: after 'boot', hi=P1, mid=P2, sleep=P3, floor=P4, p5=P5, floor=deep
    expected = {"hi": "P1", "mid": "P2", "sleep": "P3", "p5": "P5"}
    named = []
    seen_p5 = False
    for label, s, e in m:
        if label == "boot":
            name, seen_p5 = "boot", False
        elif label == "floor":
            name = "deep" if seen_p5 else "P4"
        else:
            name = expected.get(label, label)
            if name == "P5":
                seen_p5 = True
        trim = int(round(1.0 / dt))
        ss, ee = (s + trim, e - trim) if e - s > 3 * trim else (s, e)
        avg = sum(i[ss:ee]) / (ee - ss)
        named.append({"phase": name, "start_s": t[s], "dur_s": (e - s) * dt, "avg_ma": avg,
                      "q_mc": sum(i[s:e]) * dt})

    print(f"# {a.csv}\n")
    print("| # | Phase | Start (s) | Duration (s) | Avg, 1 s trimmed (mA) | Charge (mC) |")
    print("|---|---|---|---|---|---|")
    for k, r in enumerate(named):
        print(f"| {k} | {NAMES.get(r['phase'], r['phase'])} | {r['start_s']:.1f} | {r['dur_s']:.1f} | {r['avg_ma']:.2f} | {r['q_mc']:.0f} |")
    print("\n| Phase | Runs | Avg current (mA) | sd | Typical duration (s) |")
    print("|---|---|---|---|---|")
    summ = {}
    for ph in ORDER:
        rs = [r for r in named if r["phase"] == ph]
        # first/last run can be cut by the recording edges: only full-ish runs
        if not rs:
            continue
        v = [r["avg_ma"] for r in rs]
        d = [r["dur_s"] for r in rs]
        q = [r["q_mc"] for r in rs]
        summ[ph] = {"name": NAMES[ph], "runs": len(rs), "avg_ma": st.mean(v),
                    "sd": st.stdev(v) if len(v) > 1 else 0.0, "dur_s": st.median(d),
                    "q_mc": st.median(q)}
        print(f"| {NAMES[ph]} | {len(rs)} | **{st.mean(v):.2f}** | {summ[ph]['sd']:.2f} | {st.median(d):.1f} |")
    if a.json:
        json.dump({"phases": summ, "runs": named}, open(a.json, "w"), indent=2)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sessions")
    s.add_argument("csv")
    s.add_argument("--factor", type=float, default=2.5, help="threshold = factor x idle median")
    s.add_argument("--threshold", type=float, help="fixed threshold in mA (overrides --factor)")
    s.add_argument("--min-s", type=float, default=0.5)
    s.add_argument("--gap-s", type=float, default=2.0)
    s.add_argument("--json")
    p = sub.add_parser("phases")
    p.add_argument("csv")
    p.add_argument("--json")
    a = ap.parse_args()
    sessions(a) if a.cmd == "sessions" else phases(a)


if __name__ == "__main__":
    main()
