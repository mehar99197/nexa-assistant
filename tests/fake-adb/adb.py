#!/usr/bin/env python3
"""Cross-platform stand-in for adb used by tests/fake-adb/adb.cmd on Windows."""
import os
import struct
import sys
import tempfile
import time
import zlib


def ensure_screen(path):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return
    w, h = 540, 1200

    def pixel(x, y):
        if y < 60 or y >= 1000:
            return (31, 36, 46)
        row = (y - 100) // 110
        if 100 <= y < 980 and (y - 100) % 110 < 90 and 20 <= x < 520:
            return (42 + row * 6, 48, 60 + row * 10)
        return (18, 20, 26)

    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def main():
    screen = os.path.join(tempfile.gettempdir(), "nexa-fake-adb-screen.png")
    ensure_screen(screen)
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == "-s":
        args = args[2:]
    if not args:
        return 0
    cmd = args[0]
    if cmd == "devices":
        sys.stdout.write("List of devices attached\n")
        sys.stdout.write("FAKE123   device usb:1-1 product:fake model:Pixel_9 device:fake transport_id:1\n")
    elif cmd == "mdns":
        sys.stdout.write("List of discovered mdns services\n")
    elif cmd == "shell":
        sub = " ".join(args[1:])
        if sub.startswith("uiautomator"):
            dump_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fake-ui-dump.xml")
            with open(dump_path, "rb") as f:
                sys.stdout.buffer.write(f.read())
        else:
            sys.stdout.write("manufacturer=Google\nmodel=Pixel 9\nrelease=15\nhardware=FAKE123\n")
            sys.stdout.write("  level: 76\nPhysical size: 1080x2400\nSuccess\n")
    elif cmd == "exec-out":
        time.sleep(0.3)
        with open(screen, "rb") as f:
            sys.stdout.buffer.write(f.read())
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
