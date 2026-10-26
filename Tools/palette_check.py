"""Checks that a screenshot shows only the level's palette colours, as the original's 8-bit screen did.

Usage: python Tools/palette_check.py <screenshot.png> <palette.png> [--mask out.png] [--skip x0,y0,x1,y1 ...]

palette.png is the level's 256x1 strip (T_<LEVEL>_Palette, exported from the import). Prints the
share of pixels that are exactly a palette colour and the commonest colours that are not, each with
where it first appears and its nearest palette entry. --mask writes the off-palette pixels in white
on black. --skip leaves rectangles out (letterbox bars, a HUD drawn in another palette). No
third-party dependencies.
"""
import argparse
import struct
import sys
import zlib
from collections import Counter

from png_pixels import read_png


def write_png(path, width, height, rows):
    """8-bit RGB PNG from rows of bytes."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("screenshot")
    ap.add_argument("palette")
    ap.add_argument("--mask")
    ap.add_argument("--skip", nargs="*", default=[])
    a = ap.parse_args()

    pw, _, pbpp, prow = read_png(a.palette)
    palette = [tuple(prow[0][i * pbpp:i * pbpp + 3]) for i in range(pw)]
    allowed = set(palette)

    w, h, bpp, rows = read_png(a.screenshot)
    skips = [tuple(map(int, s.split(","))) for s in a.skip]

    def skipped(x, y):
        return any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in skips)

    off, first, counted = Counter(), {}, 0
    mask = [bytearray(w * 3) for _ in range(h)]
    for y in range(h):
        row = rows[y]
        for x in range(w):
            if skipped(x, y):
                continue
            counted += 1
            c = tuple(row[x * bpp:x * bpp + 3])
            if c in allowed:
                continue
            off[c] += 1
            first.setdefault(c, (x, y))
            mask[y][x * 3:x * 3 + 3] = b"\xff\xff\xff"

    bad = sum(off.values())
    print(f"{a.screenshot}: {counted} pixels checked, {counted - bad} on the palette "
          f"({100.0 * (counted - bad) / max(counted, 1):.2f}%), {bad} off it in {len(off)} colours")
    for c, n in off.most_common(12):
        near = min(range(len(palette)), key=lambda i: sum((p - q) ** 2 for p, q in zip(palette[i], c)))
        print(f"  {c} x{n} first at {first[c]}, nearest palette[{near}] = {palette[near]}")
    if a.mask:
        write_png(a.mask, w, h, mask)
        print(f"mask: {a.mask}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
