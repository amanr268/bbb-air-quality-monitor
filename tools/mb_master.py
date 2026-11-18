#!/usr/bin/env python3
"""
Poll the air-quality monitor over Modbus RTU and print its registers.
No extra packages needed (plain termios), runs on macOS and Linux.

  python3 tools/mb_master.py /dev/cu.usbserial-XXXX [--baud 9600] [--id 1] [--loop]

Wiring: USB-RS485 adapter A/B to the board's RS485 module A/B; or a 3.3 V USB-UART
straight to UART4 (adapter TX -> P9.11, RX -> P9.13, GND); or the bench tap onto
the HW-519's A/B (TX -> 1k -> A, RX -> A, B -> 10k -> 3.3 V, GND) - the echo of
the request that the tap produces is removed automatically.
"""
import argparse, os, select, struct, termios, time

NAMES = [
    ("PM1.0", 1, "ug/m3"), ("PM2.5", 10, "ug/m3"), ("PM10", 10, "ug/m3"),
    ("TVOC", 1, "ppb"), ("eCO2", 1, "ppm"), ("Temperature", 10, "C"), ("Humidity", 10, "%RH"),
    ("AQI", 1, ""), ("AQI category", 1, ""), ("Status bits", 1, ""), ("Seq low", 1, ""), ("Seq high", 1, ""),
    ("PM2.5 1 min", 10, "ug/m3"), ("PM2.5 5 min", 10, "ug/m3"), ("PM2.5 10 min", 10, "ug/m3"),
    ("PM2.5 20 min", 10, "ug/m3"), ("PM2.5 24 h", 10, "ug/m3"),
    ("PM10 1 min", 10, "ug/m3"), ("PM10 5 min", 10, "ug/m3"), ("PM10 10 min", 10, "ug/m3"),
    ("PM10 20 min", 10, "ug/m3"), ("PM10 24 h", 10, "ug/m3"),
    ("AQI 24 h", 1, ""), ("Minutes of 24 h data", 1, "min"), ("Map version", 1, ""),
]
CATS = ["Good", "Satisfactory", "Moderate", "Poor", "Very Poor", "Severe"]
BAUDS = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400, 115200: termios.B115200}


def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def open_port(dev, baud):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0                               # raw in/out/local
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.CSTOPB   # 8N2
    a[4] = a[5] = BAUDS[baud]
    a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def read_regs(fd, sid, start, qty, timeout=1.0):
    req = bytes([sid, 4]) + struct.pack(">HH", start, qty)
    req += struct.pack("<H", crc16(req))
    termios.tcflush(fd, termios.TCIFLUSH)        # drop anything left from earlier polls
    os.write(fd, req)
    want = 3 + 2 * qty + 2                        # normal reply length
    out, t0 = b"", time.time()
    while time.time() - t0 < timeout:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            out += os.read(fd, 256)
        # two-wire RS485 bench tap (A line wired to both TX and RX): our own
        # request is echoed back first - drop it
        body = out[len(req):] if out.startswith(req) else out
        if len(body) >= want or (len(body) >= 5 and body[1] & 0x80):
            break                                 # complete reply (or exception)
    out = out[len(req):] if out.startswith(req) else out
    if len(out) < 5 or crc16(out[:-2]) != struct.unpack("<H", out[-2:])[0]:
        raise IOError(f"no/invalid reply ({out.hex(' ') or 'nothing'})")
    if out[1] & 0x80:
        raise IOError(f"exception {out[2]}")
    return struct.unpack(f">{qty}H", out[3:3 + 2 * qty]), (time.time() - t0) * 1000


def show(regs, ms):
    print(f"--- reply in {ms:.0f} ms")
    for i, (name, scale, unit) in enumerate(NAMES):
        v = regs[i]
        if v == 0xFFFF or (i == 5 and v == 0x8000):
            txt = "--"
        elif i == 5:
            txt = f"{struct.unpack('>h', struct.pack('>H', v))[0] / 10:.1f}"
        elif i == 8:
            txt = CATS[v] if v < len(CATS) else str(v)
        elif i in (9, 24):
            txt = f"0x{v:04x}"
        else:
            txt = f"{v / scale:g}"
        print(f"{i:3d}  {name:<22} {txt} {unit if txt != '--' else ''}")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("port")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--id", type=int, default=1)
    p.add_argument("--loop", action="store_true", help="poll every 2 s")
    a = p.parse_args()
    fd = open_port(a.port, a.baud)
    while True:
        try:
            show(*read_regs(fd, a.id, 0, len(NAMES)))
        except IOError as e:
            print("error:", e)
        if not a.loop:
            break
        time.sleep(2)
