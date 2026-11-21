"""Like capture.py, but survives the USB port disappearing (deep sleep,
reboot): it keeps reopening the port until the time is up.
usage: capture_reconnect.py COM6 <seconds> out.log"""
import sys, time, serial

port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
end = time.time() + secs
with open(out, "wb") as f:
    while time.time() < end:
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, 115200, 0.5
            s.dtr = False
            s.rts = False
            s.open()
            f.write(f"\n[capture: port open at +{secs - (end - time.time()):.0f} s]\n".encode())
            while time.time() < end:
                f.write(s.read(4096))
                f.flush()
        except (serial.SerialException, OSError):
            f.write(f"\n[capture: port gone at +{secs - (end - time.time()):.0f} s]\n".encode())
            f.flush()
            # Wait until the port is listed again. Never probe it with a plain
            # serial.Serial(port): that opens with DTR/RTS asserted, which
            # resets the ESP32-C6 through its USB-Serial/JTAG.
            from serial.tools import list_ports
            while time.time() < end:
                time.sleep(0.5)
                if any(p.device == port for p in list_ports.comports()):
                    time.sleep(0.5)
                    break
