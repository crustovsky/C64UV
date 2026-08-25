#!/usr/bin/env python3
"""Send a synthetic C64 Ultimate VIC video stream (PAL wire format).

Test pattern: 16 vertical color bars (VIC indices 0..15) plus a white line
sweeping downward one line per frame. Usage:

    python3 tools/mockstream.py [dest_ip] [port] [seconds]
then:
    ./c64uv --no-start [--dump frame.ppm]
"""
import math
import socket
import struct
import sys
import time

W, H, LPP = 384, 272, 4
ARATE, ACHUNK = 47983, 192  # audio: sample rate, stereo frames per packet
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


def audio_packet(aseq: int, pos: int) -> bytes:
    """192 stereo frames of a 440 Hz sine, packed like the Ultimate does."""
    samples = bytearray()
    for i in range(ACHUNK):
        v = int(12000 * math.sin(2 * math.pi * 440 * (pos + i) / ARATE))
        samples += struct.pack("<hh", v, v)
    return struct.pack("<H", aseq & 0xFFFF) + samples


def main() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    aip = DEST[0]
    if 224 <= int(DEST[0].split(".")[0]) <= 239:
        # multicast: same convention as c64uv, audio group = video group + 1
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        parts = DEST[0].split(".")
        aip = ".".join(parts[:3] + [str(int(parts[3]) + 1)])
    adest = (aip, DEST[1] + 1)
    seq = frame = aseq = apos = 0
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
        # one video frame = 20 ms = ~960 audio frames = 5 packets
        while apos < (frame + 1) * ARATE // 50:
            sock.sendto(audio_packet(aseq, apos), adest)
            aseq += 1
            apos += ACHUNK
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
