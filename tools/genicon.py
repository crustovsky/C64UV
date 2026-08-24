#!/usr/bin/env python3
"""Generate assets/c64uv.svg, the app icon.

A miniature C64 boot screen: light blue border, blue screen, a big "64"
over a READY. prompt with a block cursor. Text is rasterised from the
same src/font8x8.h the viewer renders the menu with (LSB = leftmost
pixel), so the icon is reproducible from the sources alone:

    python3 tools/genicon.py > assets/c64uv.svg
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIGHT = "#706DEB"  # VIC colour 14, light blue (Pepto palette, video.c)
DARK = "#2E2C9B"   # VIC colour 6, blue


def load_font():
    text = (ROOT / "src" / "font8x8.h").read_text()
    rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,\s*){7}0x[0-9A-Fa-f]{2})\s*\}", text)
    return [[int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", row)] for row in rows[:128]]


def text_rects(font, s, x0, y0, scale):
    """Emit one rect per horizontal pixel run of the rendered string."""
    rects = []
    for ci, ch in enumerate(s):
        glyph = font[ord(ch)]
        for row in range(8):
            bits = glyph[row]
            col = 0
            while col < 8:
                if bits & (1 << col):
                    run = 0
                    while col + run < 8 and bits & (1 << (col + run)):
                        run += 1
                    rects.append(
                        f'<rect x="{x0 + (ci * 8 + col) * scale}" '
                        f'y="{y0 + row * scale}" '
                        f'width="{run * scale}" height="{scale}" fill="{LIGHT}"/>'
                    )
                    col += run
                else:
                    col += 1
    return rects


def main():
    font = load_font()
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">',
        f'<rect width="128" height="128" rx="12" fill="{LIGHT}"/>',
        f'<rect x="12" y="12" width="104" height="104" fill="{DARK}"/>',
    ]
    parts += text_rects(font, "64", x0=24, y0=24, scale=5)      # big centred "64"
    parts += text_rects(font, "READY.", x0=16, y0=74, scale=2)  # boot prompt
    parts.append(f'<rect x="16" y="92" width="16" height="16" fill="{LIGHT}"/>')  # cursor
    parts.append("</svg>")
    print("\n".join(parts))


if __name__ == "__main__":
    main()
