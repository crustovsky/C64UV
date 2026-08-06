#!/usr/bin/env python3
"""Send a synthetic C64 Ultimate VIC video stream (PAL wire format).

Test pattern: 16 vertical color bars (VIC indices 0..15) plus a white line
sweeping downward one line per frame. Usage:

    python3 tools/mockstream.py [dest_ip] [port] [seconds]
then:
    ./c64uv --no-start [--dump frame.ppm]
"""
import socket
import struct
import sys
import time

W, H, LPP = 384, 272, 4
DEST = (sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1",
        int(sys.argv[2]) if len(sys.argv) > 2 else 11000)
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0

BAR_W = W // 16  # 24 px per color bar


def build_frame(sweep_y: int) -> list[bytes]:
    """Return the packed 4bpp payload for each line."""
    lines = []
    for y in range(H):
        row = bytearray()
        for x in range(0, W, 2):
            if y == sweep_y:
                lo = hi = 1  # white sweep line
            else:
                lo, hi = x // BAR_W, (x + 1) // BAR_W
            row.append(lo | hi << 4)  # low nibble = left pixel
        lines.append(bytes(row))
    return lines


def main() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = frame = 0
    t0 = time.monotonic()
    sent = 0
    while time.monotonic() - t0 < SECONDS:
        lines = build_frame(frame % H)
        for start in range(0, H, LPP):
            line_field = start | (0x8000 if start + LPP >= H else 0)
            hdr = struct.pack("<HHHHBBH", seq & 0xFFFF, frame & 0xFFFF,
                              line_field, W, LPP, 4, 0)
            sock.sendto(hdr + b"".join(lines[start:start + LPP]), DEST)
            seq += 1
        frame += 1
        sent += 1
        # pace to 50 fps
        next_due = t0 + frame * 0.02
        delay = next_due - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    print(f"sent {sent} frames to {DEST[0]}:{DEST[1]}")


if __name__ == "__main__":
    main()
