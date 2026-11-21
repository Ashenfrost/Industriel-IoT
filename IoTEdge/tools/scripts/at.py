"""Send AT commands through the ESP32 AT bridge and print the answers.
usage: python at.py COM6 "AT" "ATI" "WAIT=5" "AT+CCLK?" ...
Each command waits for OK / ERROR (or a timeout). WAIT=n sleeps n seconds
and prints anything the modem says meanwhile (URCs). A command can carry its
own timeout as "AT+CNTP|60" (seconds).
"""
import sys, time, serial

port, cmds = sys.argv[1], sys.argv[2:]
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = False  # don't pull BOOT low (see earlier download-mode issue)
s.rts = False
s.open()
time.sleep(0.3)
s.reset_input_buffer()

def read_until_final(timeout):
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        buf += s.read(512)
        txt = buf.decode(errors="replace")
        lines = [l.strip() for l in txt.splitlines()]
        if any(l == "OK" or l == "ERROR" or l.startswith("+CME ERROR") for l in lines):
            # grab trailing bytes that arrive right after OK
            time.sleep(0.1)
            buf += s.read(512)
            break
    return buf.decode(errors="replace")

for c in cmds:
    if c.startswith("WAIT="):
        secs = float(c[5:])
        print(f"--- waiting {secs:g} s for URCs")
        end, buf = time.time() + secs, b""
        while time.time() < end:
            buf += s.read(512)
        print(buf.decode(errors="replace").strip() or "(nothing)")
        continue
    cmd, _, t = c.partition("|")
    timeout = float(t) if t else 5.0
    cmd, _, rawhex = cmd.partition("@")   # 'AT+X=13@0A0B..' -> wait for '>' then raw bytes
    s.write((cmd + "\r").encode())
    pre = ""
    if rawhex:
        buf, end = b"", time.time() + 5
        while time.time() < end and b">" not in buf:
            buf += s.read(64)
        pre = buf.decode(errors="replace")
        if b">" not in buf:
            print(f">>> {cmd}\n    (no '>' prompt) {pre.strip()}")
            continue
        s.write(bytes.fromhex(rawhex))
    out = pre + read_until_final(timeout)
    lines = [l for l in (x.strip() for x in out.splitlines()) if l and l != cmd]
    print(f">>> {cmd}")
    print("    " + ("\n    ".join(lines) if lines else "(no answer)"))
s.close()
