"""Record the ESP32-C6 USB console to a file without resetting the board.

usage: python capture.py COM6 <seconds> out.log

DTR and RTS are set to False BEFORE the port opens. On the C6's built-in
USB-Serial/JTAG, pyserial's default (both asserted on open) is read as
"enter download mode": the chip then sits in the ROM bootloader
("boot:0x4 ... waiting for download") until it is unplugged.
"""
import sys, time, serial

port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
# The board may just have been reset (flash/esptool) and USB may re-enumerate,
# so retry opening for a few seconds
s = None
deadline = time.time() + 10
while s is None and time.time() < deadline:
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.5
        s.dtr = False
        s.rts = False
        s.open()
    except serial.SerialException:
        s = None
        time.sleep(0.2)
if s is None:
    sys.exit("could not open " + port)
end = time.time() + secs
with open(out, "wb") as f:
    while time.time() < end:
        f.write(s.read(4096))
        f.flush()
s.close()
