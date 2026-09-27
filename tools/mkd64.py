#!/usr/bin/env python3
"""Write a minimal .d64 holding one autostarting BASIC program.

    python3 tools/mkd64.py [out.d64]

The program (first file on the disk, so LOAD"*",8,1 / RUN gets it) clears
the screen, prints a banner and cycles the border colour: an unmistakable
sign that a dropped image was mounted, loaded and started. Standard 35
tracks, 174848 bytes, no error bytes; BAM and directory as the 1541 DOS
writes them, so the Ultimate's drive emulation and real drives read it.
"""
import struct
import sys

SECTORS = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5  # per track 1..35


def offset(track, sector):
    return (sum(SECTORS[: track - 1]) + sector) * 256


def basic(lines):
    """Tokenise (line number, tokenised bytes) pairs into a $0801 PRG."""
    out = bytearray(b"\x01\x08")  # load address
    addr = 0x0801
    for num, body in lines:
        nxt = addr + 5 + len(body)
        out += struct.pack("<HH", nxt, num) + body + b"\x00"
        addr = nxt
    out += b"\x00\x00"
    return bytes(out)


PRINT, POKE, FOR, TO, NEXT, GOTO, CHR = 0x99, 0x97, 0x81, 0xA4, 0x82, 0x89, 0xC7
T = bytes
PROGRAM = basic([
    (10, T([POKE]) + b"53281,0:" + T([PRINT, CHR]) + b'(147)'),
    (20, T([PRINT]) + b'"   HELLO FROM C64UV"'),
    (30, T([PRINT]) + b'"   THIS DISK WAS MOUNTED AND RUN"'),
    (40, T([FOR]) + b"C=0" + T([TO]) + b"15:" + T([POKE]) + b"53280,C:"
         + T([FOR]) + b"D=0" + T([TO]) + b"150:" + T([NEXT]) + b":" + T([NEXT])),
    (50, T([GOTO]) + b"40"),
])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "c64uv-hello.d64"
    img = bytearray(sum(SECTORS) * 256)

    # file data: track 17 from sector 0, chained 254 bytes per sector
    chunks = [PROGRAM[i:i + 254] for i in range(0, len(PROGRAM), 254)]
    used = {(18, 0), (18, 1)}
    for i, chunk in enumerate(chunks):
        pos = offset(17, i)
        last = i == len(chunks) - 1
        img[pos:pos + 2] = bytes([0, len(chunk) + 1]) if last else bytes([17, i + 1])
        img[pos + 2:pos + 2 + len(chunk)] = chunk
        used.add((17, i))

    # BAM at 18/0: dir pointer, format, per-track free count + 3 bitmap bytes
    bam = offset(18, 0)
    img[bam:bam + 4] = bytes([18, 1, 0x41, 0])
    for t in range(1, 36):
        free = [s for s in range(SECTORS[t - 1]) if (t, s) not in used]
        bits = sum(1 << s for s in free)
        img[bam + t * 4:bam + t * 4 + 4] = bytes([len(free)]) + bits.to_bytes(3, "little")
    name = b"C64UV HELLO".ljust(16, b"\xa0")
    img[bam + 0x90:bam + 0xAB] = name + b"\xa0\xa0" + b"UV" + b"\xa0" + b"2A" + b"\xa0" * 4

    # directory at 18/1: one PRG entry pointing at 17/0
    d = offset(18, 1)
    img[d:d + 2] = bytes([0, 0xFF])
    img[d + 2:d + 5] = bytes([0x82, 17, 0])
    img[d + 5:d + 21] = b"HELLO".ljust(16, b"\xa0")
    img[d + 30:d + 32] = struct.pack("<H", len(chunks))

    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out} ({len(img)} bytes, program {len(PROGRAM)} bytes in "
          f"{len(chunks)} sector{'s' if len(chunks) > 1 else ''})")


if __name__ == "__main__":
    main()
