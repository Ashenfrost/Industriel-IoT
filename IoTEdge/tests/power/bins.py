#!/usr/bin/env python3
"""bins.py - average a PPK2 CSV into fixed time bins (when markers are missing).

usage: python bins.py rec.csv --bin 1.0 [--out bins.csv]

Writes one row per bin: start time (s), average (mA), max (mA), and the OR of
the digital channels in the bin. A 15 min, 100 kS/s recording (~90 M rows)
becomes 900 rows at 1 s bins: small enough to look at, plot, or segment by
current level. Streams the file; standard library only.
"""
import argparse
import csv
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--bin", type=float, default=1.0, help="bin width in seconds")
    ap.add_argument("--out", help="CSV to write (default: stdout)")
    a = ap.parse_args()

    width_ms = a.bin * 1000.0
    out = open(a.out, "w", newline="") if a.out else sys.stdout
    w = csv.writer(out)
    w.writerow(["start_s", "avg_mA", "max_mA", "digital_or"])

    with open(a.csv, newline="", encoding="utf-8-sig") as f:
        r = csv.reader(f)
        header = [h.strip().lower() for h in next(r)]
        ti = next(i for i, h in enumerate(header) if h.startswith("timestamp"))
        ci = next(i for i, h in enumerate(header) if h.startswith("current"))
        di = next((i for i, h in enumerate(header) if h.startswith("d0")), None)
        scale = 1000.0 if "(ma)" in header[ci] else 1.0   # -> uA

        cur_bin, s, n, mx, dor = None, 0.0, 0, 0.0, 0
        for row in r:
            try:
                t = float(row[ti])
                i = float(row[ci]) * scale
            except (ValueError, IndexError):
                continue
            b = int(t // width_ms)
            if cur_bin is None:
                cur_bin = b
            if b != cur_bin:
                w.writerow([f"{cur_bin * a.bin:.3f}", f"{s / n / 1000:.4f}", f"{mx / 1000:.3f}", dor])
                cur_bin, s, n, mx, dor = b, 0.0, 0, 0.0, 0
            s += i
            n += 1
            if i > mx:
                mx = i
            if di is not None:
                dor |= int(row[di], 2) if row[di].strip() else 0
        if n:
            w.writerow([f"{cur_bin * a.bin:.3f}", f"{s / n / 1000:.4f}", f"{mx / 1000:.3f}", dor])
    if a.out:
        out.close()


if __name__ == "__main__":
    main()
