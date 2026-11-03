"""Talk to the Astrochronist CYD over USB.

  python tools/cyd.py log [seconds]          print the serial log
  python tools/cyd.py sync                   set the clock and time zone from this PC
  python tools/cyd.py cmd "P 160 120" ...    send raw commands (P tap, S scroll, N tab, ? status)
  python tools/cyd.py shot out.png           save a screenshot of the display

Set CYD_PORT to use a port other than COM13.
"""
import os, sys, time, struct
import serial
from datetime import datetime, timezone

PORT, BAUD = os.environ.get("CYD_PORT", "COM13"), 921600


def open_port():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, BAUD, 0.2
    s.dtr = False; s.rts = False          # don't reset the board when opening
    s.open()
    return s


def read_for(s, secs):
    end = time.time() + secs
    while time.time() < end:
        line = s.readline()
        if line:
            print(line.decode("utf-8", "replace").rstrip())


def send(s, text, wait=0.6):
    s.write((text + "\n").encode()); s.flush()
    read_for(s, wait)


def shot(s, path):
    from PIL import Image
    s.reset_input_buffer()
    s.write(b"C\n"); s.flush()
    t0 = time.time()
    while True:
        line = s.readline()
        if line.startswith(b"SHOT"):
            w, h = map(int, line.split()[1:3]); break
        if time.time() - t0 > 5:
            raise SystemExit("no SHOT header")
    need, buf = w * h * 2, bytearray()
    s.timeout = 3
    while len(buf) < need:
        chunk = s.read(need - len(buf))
        if not chunk:
            raise SystemExit(f"short read {len(buf)}/{need}")
        buf += chunk
    px = struct.unpack(f">{w*h}H", bytes(buf))   # sprite buffer is byte-swapped RGB565
    img = Image.new("RGB", (w, h))
    img.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31) for v in px])
    img.save(path)
    print("saved", path)


def main():
    a = sys.argv[1:]
    s = open_port()
    if a[0] == "log":
        read_for(s, float(a[1]) if len(a) > 1 else 5)
    elif a[0] == "sync":
        off = int(datetime.now().astimezone().utcoffset().total_seconds() // 60)
        sign = "+" if off >= 0 else "-"
        send(s, f"Z{off} UTC{sign}{abs(off)//60:02d}:{abs(off)%60:02d}")
        send(s, f"T{datetime.now(timezone.utc).timestamp()*1000:.0f}")
    elif a[0] == "cmd":
        for c in a[1:]:
            send(s, c)
    elif a[0] == "shot":
        shot(s, a[1])


if __name__ == "__main__":
    main()
