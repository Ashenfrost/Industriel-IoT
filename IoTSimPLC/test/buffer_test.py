"""Tests the SimPLC sample buffer and clock over Modbus TCP from a PC.

The PC must be on the same Wi-Fi as the SimPLC. Let the SimPLC run for a
minute or two first, so the buffer has a few samples (one per 10 s).

    pip install pymodbus
    python buffer_test.py <simplc-ip>
"""
import sys
import time

from pymodbus.client import ModbusTcpClient

UTC_HI, CLOCK_SET, COUNT, OLDEST, AGE, ACK, DROP, WIN_START, NEWEST = (
    100, 102, 103, 104, 105, 106, 107, 108, 109)
WIN_BASE, WIN_REGS, PER = 200, 120, 10


def regs(c, addr, n):
    r = c.read_holding_registers(addr, count=n)
    assert not r.isError(), r
    return r.registers


def check(title, ok):
    print(("PASS " if ok else "FAIL ") + title)
    return ok


def status(c):
    v = regs(c, CLOCK_SET, 8)
    return dict(clock_set=v[0], count=v[1], oldest=v[2], age=v[3], ack=v[4],
                dropped=v[5], newest=v[7])


def s16(v):
    return v - 65536 if v >= 32768 else v


def main(host):
    c = ModbusTcpClient(host, port=502)
    assert c.connect(), "SimPLC not reachable"

    before = status(c)
    print("status:", before)
    check("buffer has samples", before["count"] > 0)

    # Clock: write UTC (high, low) in one request, read it back
    now = int(time.time())
    c.write_registers(UTC_HI, [now >> 16, now & 0xFFFF])
    hi, lo = regs(c, UTC_HI, 2)
    check(f"clock set and read back ({(hi << 16 | lo) - now:+d} s)",
          abs((hi << 16 | lo) - now) <= 1 and status(c)["clock_set"] == 1)

    # Window from the oldest sample
    c.write_register(WIN_START, before["oldest"])
    w = regs(c, WIN_BASE, WIN_REGS)
    samples = [w[i:i + PER] for i in range(0, WIN_REGS, PER) if w[i] != 0]
    for s in samples[:3]:
        ts = s[1] << 16 | s[2]
        print(f"  seq {s[0]:5d}  {time.strftime('%H:%M:%S', time.localtime(ts))}  "
              f"status {s[3]:#x}  T {[s16(x) / 10 for x in s[4::2]]}  "
              f"H {[x / 10 for x in s[5::2]]}")
    check("window starts at the oldest seq", samples and samples[0][0] == before["oldest"])
    check("seq numbers are consecutive",
          all(b[0] == a[0] % 65535 + 1 for a, b in zip(samples, samples[1:])))
    ts = [s[1] << 16 | s[2] for s in samples]
    check("timestamps 10 s apart and in the past",
          all(9 <= b - a <= 11 for a, b in zip(ts, ts[1:])) and max(ts) <= now + 1)

    # Acknowledge the first two: they must disappear, the rest stays
    if len(samples) >= 3:
        c.write_register(ACK, samples[1][0])
        after = status(c)
        check(f"ack removes up to seq {samples[1][0]}",
              after["oldest"] == samples[2][0] and after["ack"] == samples[1][0])
        # Unknown seq is ignored
        c.write_register(ACK, 0)
        check("ack 0 is ignored", status(c)["oldest"] == samples[2][0])
    c.close()


if __name__ == "__main__":
    main(sys.argv[1])
