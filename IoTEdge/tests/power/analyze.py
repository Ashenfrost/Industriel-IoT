#!/usr/bin/env python3
"""analyze.py - turn PPK2 recordings into the numbers for the power tests.

Reads a CSV exported from the Power Profiler app (nRF Connect for Desktop):
current plus the digital channels D0-D7. The file is streamed, so long
recordings (millions of rows) are fine. Standard library only.

Usage
  python analyze.py markers  rec.csv [--json out.json]     # tests 1-3
  python analyze.py phases   rec.csv [--json out.json]     # test 4
  python analyze.py model    --session test2.json [--rest-ma 1.2] [--battery-mah 2000]
  python analyze.py selftest                               # checks the maths

Markers (firmware APP_POWER_MARKERS, see README.md):
  tests 1-3: D0 SESSION, D1 PUBLISH, D2 SAMPLE, D3 MODBUS (high while active)
  test 4:    phase number in binary on D0 (bit 0), D1 (bit 1), D2 (bit 2)

Units: current in mA, charge in mC (= mA*s), time in s.

CSV format: the script looks for a time column ("Timestamp(ms)" or similar,
in ms), a current column ("Current(uA)" in uA, or "Current(mA)") and the
digital channels, either as one column "D0-D7" with a bit string like
"10000000" or as separate columns "D0".."D7". The bit order of a "D0-D7"
string is set with --bits (default: first character = D0). Check it once
with a real recording: the summary lists how many pulses each channel had,
and D2 (SAMPLE) must pulse every 10 s.
"""
import argparse
import csv
import io
import json
import math
import os
import random
import sys
import tempfile

N_CH = 8
MARKER_NAMES = {0: "SESSION (D0)", 1: "PUBLISH (D1)", 2: "SAMPLE (D2)", 3: "MODBUS (D3)"}
PHASE_NAMES = {
    0: "deep sleep / boot / switch (all low)",
    1: "ESP busy, modem idle",
    2: "ESP idle, modem idle",
    3: "ESP light sleep, modem idle",
    4: "ESP light sleep, modem radio off",
    5: "ESP idle, modem radio off",
}


# ------------------------------------------------------------------ input

def open_rows(path, bit_order):
    """Yields (t_ms, current_uA, bits) from a Power Profiler CSV."""
    f = open(path, newline="", encoding="utf-8-sig")
    reader = csv.reader(f)
    header = [h.strip() for h in next(reader)]
    low = [h.lower() for h in header]

    def find(pred, what):
        for i, h in enumerate(low):
            if pred(h):
                return i
        sys.exit(f"no {what} column in {path}; header was: {header}")

    t_col = find(lambda h: h.startswith("timestamp") or h.startswith("time"), "time")
    i_col = find(lambda h: h.startswith("current"), "current")
    i_scale = 1000.0 if "(ma)" in low[i_col] else 1.0          # -> uA
    t_scale = 1000.0 if low[t_col].endswith("(s)") else 1.0    # -> ms

    combined = next((i for i, h in enumerate(low) if h.replace(" ", "") in ("d0-d7", "digital")), None)
    separate = {int(h[1]): i for i, h in enumerate(low) if len(h) == 2 and h[0] == "d" and h[1].isdigit()}

    def bits_of(row):
        if combined is not None:
            s = row[combined].strip()
            v = 0
            for k, c in enumerate(s[:N_CH]):
                if c == "1":
                    v |= 1 << (k if bit_order == "d0first" else (len(s) - 1 - k))
            return v
        v = 0
        for ch, col in separate.items():
            if row[col].strip() in ("1", "1.0", "true", "True"):
                v |= 1 << ch
        return v

    if combined is None and not separate:
        print("warning: no digital channels in the file; markers/phases will be empty",
              file=sys.stderr)

    def gen():
        with f:
            for row in reader:
                if not row or not row[t_col].strip():
                    continue
                try:
                    t = float(row[t_col]) * t_scale
                    i = float(row[i_col]) * i_scale
                except ValueError:
                    continue
                yield t, i, bits_of(row)
    return gen()


# ------------------------------------------------------------ core maths

class Acc:
    """Charge/time/peak accumulator. Charge in nC (uA * ms)."""
    __slots__ = ("q", "t", "peak")

    def __init__(self):
        self.q = 0.0
        self.t = 0.0
        self.peak = 0.0

    def add(self, i_ua, dt_ms):
        self.q += i_ua * dt_ms
        self.t += dt_ms
        if i_ua > self.peak:
            self.peak = i_ua

    def avg_ma(self):
        return (self.q / self.t) / 1000.0 if self.t > 0 else float("nan")


def to_mc(q_nc):
    return q_nc / 1e6


def stats(values):
    if not values:
        return {"n": 0}
    m = sum(values) / len(values)
    sd = math.sqrt(sum((v - m) ** 2 for v in values) / (len(values) - 1)) if len(values) > 1 else 0.0
    return {"n": len(values), "mean": m, "min": min(values), "max": max(values), "sd": sd}


def analyze_markers(rows):
    """Per-channel high intervals + overall + 'D0 low' (between sessions)."""
    total, d0_low = Acc(), Acc()
    open_iv = [None] * N_CH            # running Acc while channel is high
    intervals = {ch: [] for ch in range(N_CH)}
    prev_t = None
    start_t = end_t = None

    for t, i, bits in rows:
        if prev_t is None:
            prev_t, start_t = t, t
            prev_bits = bits
            continue
        dt = t - prev_t
        if dt <= 0:
            prev_t = t
            continue
        # the sample's current/bits are taken to hold over [prev_t, t)
        total.add(i, dt)
        if not prev_bits & 1:
            d0_low.add(i, dt)
        for ch in range(N_CH):
            high = bool(prev_bits >> ch & 1)
            if high:
                if open_iv[ch] is None:
                    open_iv[ch] = Acc()
                open_iv[ch].add(i, dt)
            elif open_iv[ch] is not None:
                intervals[ch].append(open_iv[ch])
                open_iv[ch] = None
        prev_t, prev_bits, end_t = t, bits, t

    # a channel still high at the end is an incomplete interval: dropped
    return {"total": total, "d0_low": d0_low, "intervals": intervals,
            "start_ms": start_t, "end_ms": end_t}


def analyze_phases(rows, trim_s, checkpoint_ms=50.0):
    """Splits the recording into runs of equal phase number (bits & 7)."""
    segs = []                          # dicts with start, end, cum checkpoints
    cur = None
    prev_t = None
    q_cum = 0.0

    for t, i, bits in rows:
        phase = bits & 7
        if prev_t is not None:
            dt = t - prev_t
            if dt > 0:
                q_cum += prev_i * dt
                if cur is not None:
                    cur["peak"] = max(cur["peak"], prev_i)
                    if t - cur["cp"][-1][0] >= checkpoint_ms:
                        cur["cp"].append((t, q_cum))
        if cur is None or phase != cur["phase"]:
            if cur is not None:
                cur["end"] = t
                cur["cp"].append((t, q_cum))
                segs.append(cur)
            cur = {"phase": phase, "start": t, "cp": [(t, q_cum)], "peak": 0.0}
        prev_t, prev_i = t, i
    if cur is not None and prev_t is not None:
        cur["end"] = prev_t
        cur["cp"].append((prev_t, q_cum))
        segs.append(cur)

    def q_at(cp, when):
        """Cumulative charge at time `when` (linear between checkpoints)."""
        lo, hi = 0, len(cp) - 1
        while lo < hi - 1:
            mid = (lo + hi) // 2
            if cp[mid][0] <= when:
                lo = mid
            else:
                hi = mid
        (t0, q0), (t1, q1) = cp[lo], cp[hi]
        if t1 == t0:
            return q0
        return q0 + (q1 - q0) * (when - t0) / (t1 - t0)

    out = []
    trim = trim_s * 1000.0
    for s in segs:
        dur = s["end"] - s["start"]
        a, b = s["start"] + trim, s["end"] - trim
        if b - a > 0:
            q = q_at(s["cp"], b) - q_at(s["cp"], a)
            avg_ma = (q / (b - a)) / 1000.0
        else:
            avg_ma = float("nan")
        q_full = s["cp"][-1][1] - s["cp"][0][1]
        out.append({"phase": s["phase"], "start_s": s["start"] / 1000.0, "dur_s": dur / 1000.0,
                    "avg_ma_trimmed": avg_ma, "q_mc": to_mc(q_full), "peak_ma": s["peak"] / 1000.0})
    return out


# ---------------------------------------------------------------- output

def fmt(v, d=3):
    return "-" if v is None or (isinstance(v, float) and math.isnan(v)) else f"{v:.{d}f}"


def report_markers(res, path):
    tot, low = res["total"], res["d0_low"]
    dur_s = tot.t / 1000.0
    print(f"# {os.path.basename(path)}\n")
    print("| Overall | Value |\n|---|---|")
    print(f"| Duration | {dur_s:.1f} s |")
    print(f"| **Average current** | **{fmt(tot.avg_ma())} mA** |")
    print(f"| Peak | {fmt(tot.peak / 1000.0)} mA |")
    print(f"| Charge | {fmt(to_mc(tot.q), 2)} mC |")
    print(f"| Average with D0 low (outside sessions) | {fmt(low.avg_ma())} mA ({low.t / 1000.0:.1f} s) |")
    print()
    print("| Marker | Count | Duration mean (min-max) | Charge mean (sd) | Avg current | Peak |")
    print("|---|---|---|---|---|---|")
    summary = {"duration_s": dur_s, "avg_ma": tot.avg_ma(), "peak_ma": tot.peak / 1000.0,
               "charge_mc": to_mc(tot.q), "avg_ma_d0_low": low.avg_ma(), "markers": {}}
    for ch in range(N_CH):
        ivs = res["intervals"][ch]
        if not ivs:
            continue
        d = stats([iv.t / 1000.0 for iv in ivs])
        q = stats([to_mc(iv.q) for iv in ivs])
        avg = sum(iv.q for iv in ivs) / sum(iv.t for iv in ivs) / 1000.0
        peak = max(iv.peak for iv in ivs) / 1000.0
        name = MARKER_NAMES.get(ch, f"D{ch}")
        print(f"| {name} | {d['n']} | {fmt(d['mean'])} s ({fmt(d['min'])}-{fmt(d['max'])}) | "
              f"{fmt(q['mean'], 4)} mC ({fmt(q['sd'], 4)}) | {fmt(avg)} mA | {fmt(peak)} mA |")
        summary["markers"][f"D{ch}"] = {"name": name, "count": d["n"], "dur_s": d, "charge_mc": q,
                                        "avg_ma": avg, "peak_ma": peak}
    # derived numbers for the model: what a session costs besides its publishes
    m = summary["markers"]
    if "D0" in m and "D1" in m and m["D0"]["count"]:
        pubs_per_session = m["D1"]["count"] / m["D0"]["count"]
        q_conn = m["D0"]["charge_mc"]["mean"] - pubs_per_session * m["D1"]["charge_mc"]["mean"]
        summary["derived"] = {"publishes_per_session": pubs_per_session,
                              "q_session_mc": m["D0"]["charge_mc"]["mean"],
                              "t_session_s": m["D0"]["dur_s"]["mean"],
                              "q_publish_mc": m["D1"]["charge_mc"]["mean"],
                              "q_connect_mc": q_conn}
        print()
        print(f"Derived: {pubs_per_session:.1f} publishes/session, "
              f"connect/overhead per session = {fmt(q_conn, 3)} mC "
              f"(session charge minus its publishes)")
    print("\nNote: averages per marker include everything else running at the same time "
          "(Wi-Fi, other threads, the modem).")
    return summary


def report_phases(segs, path, min_dur_s):
    print(f"# {os.path.basename(path)}\n")
    print("| # | Phase | Start (s) | Duration (s) | Avg, trimmed (mA) | Charge (mC) | Peak (mA) |")
    print("|---|---|---|---|---|---|---|")
    for k, s in enumerate(segs):
        if s["dur_s"] < min_dur_s:
            continue
        print(f"| {k} | {s['phase']}: {PHASE_NAMES.get(s['phase'], '?')} | {s['start_s']:.1f} | "
              f"{s['dur_s']:.1f} | {fmt(s['avg_ma_trimmed'])} | {fmt(s['q_mc'], 3)} | {fmt(s['peak_ma'])} |")
    # summary per phase number over all rounds
    by = {}
    for s in segs:
        if s["dur_s"] >= min_dur_s and not math.isnan(s["avg_ma_trimmed"]):
            by.setdefault(s["phase"], []).append(s["avg_ma_trimmed"])
    print("\n| Phase | Rounds | Avg current (mA), mean of rounds | sd |")
    print("|---|---|---|---|")
    summary = {}
    for ph in sorted(by):
        st = stats(by[ph])
        print(f"| {ph}: {PHASE_NAMES.get(ph, '?')} | {st['n']} | **{fmt(st['mean'], 4)}** | {fmt(st['sd'], 4)} |")
        summary[str(ph)] = {"name": PHASE_NAMES.get(ph, "?"), **st}
    print("\nPhase 0 mixes deep sleep with the short boot/switch gaps; the long phase-0 "
          "runs (about 30 s) are the deep sleep. Read them in the per-run table above.")
    return {"phases": summary, "runs": segs}


def model(args):
    s = json.load(open(args.session)) if args.session else {}
    d = s.get("derived", {})
    q_conn = args.q_connect if args.q_connect is not None else d.get("q_connect_mc")
    q_pub = args.q_publish if args.q_publish is not None else d.get("q_publish_mc")
    t_sess = args.t_session if args.t_session is not None else d.get("t_session_s")
    q_sample = args.q_sample
    t_sample = args.t_sample
    if "D2" in s.get("markers", {}):
        if q_sample is None:
            q_sample = s["markers"]["D2"]["charge_mc"]["mean"]
        if t_sample is None:
            t_sample = s["markers"]["D2"]["dur_s"]["mean"]
    t_sample = t_sample or 0.0
    i_rest = args.rest_ma if args.rest_ma is not None else s.get("avg_ma_d0_low")
    missing = [n for n, v in (("q_connect", q_conn), ("q_publish", q_pub), ("t_session", t_sess),
                              ("q_sample", q_sample), ("rest_ma", i_rest)) if v is None]
    if missing:
        sys.exit("missing inputs: " + ", ".join(missing) + " (give them as options or via --session)")

    print("Inputs:")
    print(f"  connect/overhead per session  {q_conn:.3f} mC")
    print(f"  per publish (NDATA)           {q_pub:.4f} mC, {args.per_msg} samples per NDATA")
    # The sample marker's charge includes the resting current during the
    # sample, which the rest term below already counts: use only the extra.
    q_sample_extra = max(q_sample - i_rest * t_sample, 0.0)
    print(f"  per sample (+Modbus)          {q_sample:.4f} mC over {t_sample:.3f} s, "
          f"{q_sample_extra:.4f} mC above rest, every {args.sample_s} s")
    print(f"  base session time             {t_sess:.2f} s")
    print(f"  current while resting         {i_rest:.4f} mA")
    print(f"  battery                       {args.battery_mah} mAh, {args.usable:.0%} usable\n")
    print("| Interval | Samples | NDATA msgs | Session (mC) | Samples (mC) | Rest (mC) | **Avg current** | Battery life |")
    print("|---|---|---|---|---|---|---|---|")
    for T in args.intervals:
        n = int(T // args.sample_s)
        msgs = math.ceil(n / args.per_msg)
        q_session = q_conn + msgs * q_pub
        q_samples = n * q_sample_extra
        q_rest = i_rest * max(T - t_sess, 0)            # mA * s = mC
        i_avg = (q_session + q_samples + q_rest) / T     # mC / s = mA
        hours = args.battery_mah * args.usable / i_avg
        print(f"| {T:.0f} s | {n} | {msgs} | {q_session:.2f} | {q_samples:.2f} | {q_rest:.2f} | "
              f"**{i_avg:.3f} mA** | {hours:.0f} h ({hours / 24:.1f} d) |")
    print("\nAssumptions: the session's fixed cost (connect, TLS, NBIRTH, NDEATH) is the same "
          "for every interval and its time is small next to the interval; 'resting' covers "
          "everything between samples (use test 2's D0-low average for 'awake', or a test 4 "
          "phase for a sleep design).")


# -------------------------------------------------------------- selftest

def selftest():
    """Synthetic recordings with known answers; checks the maths end to end."""
    random.seed(1)
    ok = True

    def check(name, got, want, tol):
        nonlocal ok
        good = abs(got - want) <= tol
        ok &= good
        print(f"  {'OK ' if good else 'BAD'} {name}: got {got:.4f}, want {want:.4f} (+/-{tol})")

    # --- markers: 10 kS/s, 60 s; rest 5 mA; session every 20 s for 2 s at 80 mA
    #     with 3 publishes of 100 ms at 200 mA inside; sample every 10 s, 50 ms at 30 mA
    fs, T = 10_000, 60.0
    rows = io.StringIO()
    w = csv.writer(rows)
    w.writerow(["Timestamp(ms)", "Current(uA)", "D0-D7"])
    for k in range(int(T * fs)):
        t = k / fs
        cur, bits = 5.0, 0
        ts = t % 20.0
        if 1.0 <= ts < 3.0:
            bits |= 1
            cur = 80.0
            for p in range(3):
                if 1.5 + 0.4 * p <= ts < 1.6 + 0.4 * p:
                    bits |= 2
                    cur = 200.0
        if (t % 10.0) < 0.05:
            bits |= 4
            cur += 30.0
        s = "".join("1" if bits >> c & 1 else "0" for c in range(8))
        w.writerow([f"{t * 1000:.3f}", f"{cur * 1000 + random.uniform(-50, 50):.1f}", s])
    path = os.path.join(tempfile.gettempdir(), "ppk2_selftest_markers.csv")
    open(path, "w", newline="").write(rows.getvalue())
    res = analyze_markers(open_rows(path, "d0first"))
    iv = res["intervals"]
    print("markers:")
    check("sessions", len(iv[0]), 3, 0)
    check("publishes", len(iv[1]), 9, 0)
    check("samples", len(iv[2]), 6, 0)        # t = 0, 10, 20, 30, 40, 50 s
    check("session duration (s)", stats([x.t / 1000 for x in iv[0]])["mean"], 2.0, 0.001)
    # session charge: 2 s*80 + 3*0.1 s*(200-80) = 196 mA*s = 196 mC
    check("session charge (mC)", stats([to_mc(x.q) for x in iv[0]])["mean"], 196.0, 0.5)
    check("publish charge (mC)", stats([to_mc(x.q) for x in iv[1]])["mean"], 20.0, 0.1)
    # whole file: 54 s at 5 mA + 3 sessions*196 mC + 6 samples*0.05 s*30 mA -> / 60 s
    want_avg = (54 * 5 + 3 * 196 + 6 * 0.05 * 30) / 60
    check("overall average (mA)", res["total"].avg_ma(), want_avg, 0.02)

    # --- phases: 1 kS/s; phases 1..5 for 20 s at known currents, then 30 s phase 0
    levels = {1: 40.0, 2: 25.0, 3: 2.0, 4: 0.5, 5: 12.0, 0: 0.05}
    fs = 1000
    rows = io.StringIO()
    w = csv.writer(rows)
    w.writerow(["Timestamp(ms)", "Current(uA)"] + [f"D{c}" for c in range(8)])
    t = 0.0
    for ph, dur in [(1, 20), (2, 20), (3, 20), (4, 20), (5, 20), (0, 30)] * 2:
        for k in range(int(dur * fs)):
            cur = levels[ph] * 1000
            if k < 200:                                   # 0.2 s transition spike
                cur += 100_000
            w.writerow([f"{t:.3f}", f"{cur:.1f}"] + [str(ph >> c & 1) for c in range(8)])
            t += 1000.0 / fs
    path = os.path.join(tempfile.gettempdir(), "ppk2_selftest_phases.csv")
    open(path, "w", newline="").write(rows.getvalue())
    segs = analyze_phases(open_rows(path, "d0first"), trim_s=1.0)
    print("phases:")
    check("runs", len(segs), 12, 0)
    for s in segs[:6]:
        check(f"phase {s['phase']} trimmed avg (mA)", s["avg_ma_trimmed"], levels[s["phase"]], 0.001)

    # --- model: with the synthetic numbers, a 20 s interval with 3 publishes
    #     per session must give back the synthetic recording's average.
    print("model:")
    rest = 5.0
    q_conn = 196.0 - 3 * 20.0                       # session minus its publishes
    q_extra = 0.05 * 30.0                           # sample adds 30 mA for 50 ms
    i_avg = (q_conn + 3 * 20.0 + 2 * q_extra + rest * (20.0 - 2.0)) / 20.0
    check("model average for the synthetic cycle (mA)", i_avg, want_avg, 0.02)

    print("\nSELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


# ------------------------------------------------------------------- cli

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("markers", help="tests 1-3: sessions, publishes, samples, Modbus polls")
    m.add_argument("csv")
    m.add_argument("--json", help="write the summary here (input for 'model')")
    m.add_argument("--bits", choices=["d0first", "d7first"], default="d0first")

    p = sub.add_parser("phases", help="test 4: average current per sleep phase")
    p.add_argument("csv")
    p.add_argument("--json")
    p.add_argument("--bits", choices=["d0first", "d7first"], default="d0first")
    p.add_argument("--trim", type=float, default=2.5, help="seconds cut off each end of a phase")
    p.add_argument("--min-dur", type=float, default=3.0, help="ignore runs shorter than this (s)")

    mo = sub.add_parser("model", help="average current and battery life for publish intervals")
    mo.add_argument("--session", help="JSON from 'markers' on a test 2 recording")
    mo.add_argument("--q-connect", type=float, help="mC per session besides its publishes")
    mo.add_argument("--q-publish", type=float, help="mC per NDATA publish")
    mo.add_argument("--t-session", type=float, help="s, base session time")
    mo.add_argument("--q-sample", type=float, help="mC per sample incl. Modbus poll")
    mo.add_argument("--t-sample", type=float, help="s per sample (to subtract the rest current)")
    mo.add_argument("--rest-ma", type=float, help="mA between samples (test 2 D0-low or a test 4 phase)")
    mo.add_argument("--sample-s", type=float, default=10.0)
    mo.add_argument("--per-msg", type=int, default=7, help="samples per NDATA (7 over cellular)")
    mo.add_argument("--intervals", type=float, nargs="+", default=[300, 900, 1800, 3600])
    mo.add_argument("--battery-mah", type=float, default=2000.0)
    mo.add_argument("--usable", type=float, default=0.8, help="usable share of the capacity")

    sub.add_parser("selftest", help="check the maths on synthetic data")

    a = ap.parse_args()
    if a.cmd == "selftest":
        sys.exit(selftest())
    if a.cmd == "model":
        model(a)
        return
    if a.cmd == "markers":
        out = report_markers(analyze_markers(open_rows(a.csv, a.bits)), a.csv)
    else:
        out = report_phases(analyze_phases(open_rows(a.csv, a.bits), a.trim), a.csv, a.min_dur)
    if a.json:
        json.dump(out, open(a.json, "w"), indent=2, default=lambda o: None)
        print(f"\nwrote {a.json}")


if __name__ == "__main__":
    main()
