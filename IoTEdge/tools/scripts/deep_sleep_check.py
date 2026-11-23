"""One quick end-to-end test of the deep sleep design, with PASS/FAIL.

Resets the Edge, follows its log until a session with acknowledgement is
done (or the time runs out), then reads the broker and sparkplug-host logs
on the VPS (read-only, over ssh) for the same period.

Build the Edge with short intervals and the simulated deep sleep, so the
USB log keeps running and a session comes quickly, e.g.:
  -DAPP_DEEP_SLEEP=1 -DAPP_DEEP_SLEEP_SIM=1 -DAPP_ALARM_CHECK_S=20 -DAPP_SEND_AGE_S=80

usage: python deep_sleep_check.py COM6 [seconds=150] [ssh=admin@193.181.218.152]
"""
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 150
ssh = sys.argv[3] if len(sys.argv) > 3 else "admin@193.181.218.152"
ansi = re.compile(r"\x1b\[[0-9;]*m")


def reset_edge():
    """Reset via esptool as soon as the port is there (it may be asleep)."""
    end = time.time() + 90
    while time.time() < end:
        if any(p.device == port for p in list_ports.comports()):
            r = subprocess.run([sys.executable, "-m", "esptool", "--port", port,
                                "--after", "hard-reset", "read-mac"],
                               capture_output=True, text=True)
            if "Hard resetting" in r.stdout:
                return True
        time.sleep(0.2)
    return False


def follow_log():
    """Edge log lines until an acknowledged session (plus a few s) or timeout."""
    lines, buf, done_at = [], b"", None
    end = time.time() + secs
    s = None
    while time.time() < end and (done_at is None or time.time() < done_at + 4):
        try:
            if s is None:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = port, 115200, 0.2
                s.dtr = s.rts = False
                s.open()
            buf += s.read(4096)
        except (serial.SerialException, OSError):
            s = None  # real deep sleep or reset: wait for the port again
            time.sleep(0.5)
            continue
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = ansi.sub("", raw.decode(errors="replace")).strip()
            if line:
                lines.append(line)
                if any(k in line for k in ("wake_cycle", "Session", "NDATA sent", "NBIRTH",
                                           "err>", "Acknowledged", "FATAL")):
                    print("  " + line[line.find("<"):] if "<" in line else "  " + line)
                if "Acknowledged up to seq" in line and done_at is None:
                    done_at = time.time()
    return lines


def vps_logs(since_s):
    cmd = (f"docker logs --since {since_s}s iot-stack-sparkplug-host-1 2>&1 | grep -v STORE; "
           f"echo ===; docker logs --since {since_s}s iot-stack-mosquitto-1 2>&1 "
           f"| grep -E 'malformed|device-.*disconnected'")
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", ssh, cmd],
                       capture_output=True, text=True, timeout=60)
    host, _, broker = r.stdout.partition("===")
    return host, broker, r.returncode


def main():
    t0 = time.time()
    print("Resetting the Edge ...")
    if not reset_edge():
        print("FAIL could not reset the Edge (port never appeared)")
        return 1
    print("Following the Edge log ...")
    log = "\n".join(follow_log())
    host, broker, rc = vps_logs(int(time.time() - t0) + 5)

    # Judge the last session: earlier wakes may have failed and retried
    last = log[log.rfind("Sending "):] if "Sending " in log else ""
    retries = log.count("Session failed")
    m = re.search(r"Sending (\d+) sample", last)
    checks = [
        ("Edge reads the SimPLC", re.search(r"SimPLC read \d+ ms \(rc 0\)", log)),
        ("Edge sends a batch", m),
        ("session completes", "Session done" in last and "Session failed" not in last),
        (f"first session works (failed before: {retries})", retries == 0),
        ("SimPLC clock set", "SimPLC clock set" in last),
        ("samples acknowledged", "Acknowledged up to seq" in last),
        ("no crash", "FATAL" not in log),
        ("VPS logs read", rc == 0),
        ("host got NBIRTH", "born:" in host),
        ("no corrupt payloads at host", "Bad protobuf" not in host),
        ("no malformed packets at broker", "malformed" not in broker),
        ("no rejected session", "rejected" not in host),
    ]
    print()
    for name, ok in checks:
        print(("PASS " if ok else "FAIL ") + name)
    failed = sum(1 for _, ok in checks if not ok)
    print(f"\n{'ALL PASSED' if not failed else f'{failed} FAILED'} "
          f"({m.group(1) if m else 0} samples, {time.time() - t0:.0f} s)")
    if failed:
        print("\n--- host log ---\n" + host.strip()[-1500:])
        print("--- broker ---\n" + broker.strip()[-800:])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
