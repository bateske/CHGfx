#!/usr/bin/env python3
"""
fontconvert.py - turn a TTF/OTF into a CHGfx (= Adafruit GFXfont) header.

This is the tool that produced everything in src/fonts/. It is the Python
equivalent of Adafruit's fontconvert.c, and emits the identical binary
layout, so its output works under Adafruit_GFX too.

    python fontconvert.py DejaVuSans.ttf 12 CHGfx_Sans9 > CHGfx_Sans9.h

Usage:
    fontconvert.py <font-file> <pixel-size> <symbol-name> [options]

Options:
    --first N       first character code (default 32, space)
    --last N        last character code  (default 126, '~')
    --chars STR     use exactly these characters; the emitted range spans
                    min..max of the set and anything in between that you
                    did not ask for is emitted as an empty glyph. Cheaper
                    than --first/--last when you only need digits.
    --line N        override yAdvance (baseline-to-baseline), in pixels
    --threshold N   1..255. Only used if --gray is given.
    --gray          render antialiased and threshold, instead of asking
                    FreeType for a hinted 1-bit bitmap. Occasionally
                    better above ~16 px; usually worse below it.

Pixel size is the em size handed to FreeType, NOT the cap height and not
the line height. DejaVu Sans at 12 px gives roughly a 9 pt look with an
11 px ascender - hence the "9" in CHGfx_Sans9, matching the pt-based
naming Adafruit uses.
"""

import sys
import os
import argparse
from PIL import Image, ImageDraw, ImageFont

PAD = 8  # scratch margin so hinted overshoot cannot fall off the canvas


def render(font, ch, gray, threshold):
    """Rasterize one character. Returns (bitmap_rows, w, h, xoff, yoff).

    bitmap_rows is a list of lists of 0/1, tightly cropped to the inked
    area. xoff/yoff are relative to the pen position on the baseline.
    """
    try:
        length = font.getlength(ch)
    except Exception:
        return [], 0, 0, 0, 0, 0

    box_w = int(length) + 4 * PAD
    box_h = font.size * 3 + 2 * PAD
    base = box_h // 2

    mode = "L" if gray else "1"
    img = Image.new(mode, (box_w, box_h), 0)
    d = ImageDraw.Draw(img)
    # anchor "ls" = left edge, baseline. That is exactly the GFXfont pen.
    d.text((2 * PAD, base), ch, font=font, fill=255 if gray else 1, anchor="ls")

    px = img.load()
    if gray:
        get = lambda x, y: 1 if px[x, y] >= threshold else 0
    else:
        get = lambda x, y: 1 if px[x, y] else 0

    # Tight crop.
    x0, y0, x1, y1 = box_w, box_h, -1, -1
    for y in range(box_h):
        for x in range(box_w):
            if get(x, y):
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y

    advance = int(round(length))
    if x1 < 0:                       # blank glyph (space)
        return [], 0, 0, 0, 0, advance

    w = x1 - x0 + 1
    h = y1 - y0 + 1
    rows = [[get(x0 + c, y0 + r) for c in range(w)] for r in range(h)]
    return rows, w, h, x0 - 2 * PAD, y0 - base, advance


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("font")
    ap.add_argument("size", type=int)
    ap.add_argument("name")
    ap.add_argument("--first", type=int, default=32)
    ap.add_argument("--last", type=int, default=126)
    ap.add_argument("--chars", default=None)
    ap.add_argument("--line", type=int, default=None)
    ap.add_argument("--threshold", type=int, default=128)
    ap.add_argument("--gray", action="store_true")
    ap.add_argument("--note", default=None, help="extra provenance line for the header comment")
    a = ap.parse_args()

    if a.chars:
        codes = sorted(set(ord(c) for c in a.chars))
        first, last = codes[0], codes[-1]
        wanted = set(codes)
    else:
        first, last = a.first, a.last
        wanted = set(range(first, last + 1))

    font = ImageFont.truetype(a.font, a.size)
    ascent, descent = font.getmetrics()
    y_advance = a.line if a.line is not None else ascent + descent

    bitmap = bytearray()
    glyphs = []
    bits = []          # pending bit accumulator, flushed per glyph boundary

    for code in range(first, last + 1):
        offset = len(bitmap)
        if code not in wanted:
            glyphs.append((code, offset, 0, 0, 0, 0, 0))
            continue

        rows, w, h, xo, yo, adv = render(font, chr(code), a.gray, a.threshold)
        # Pack MSB-first, rows concatenated with no padding between rows.
        acc = 0
        n = 0
        for r in rows:
            for b in r:
                acc = (acc << 1) | b
                n += 1
                if n == 8:
                    bitmap.append(acc)
                    acc, n = 0, 0
        if n:
            bitmap.append(acc << (8 - n))   # pad the final byte of the glyph
        glyphs.append((code, offset, w, h, adv, xo, yo))

    if len(bitmap) > 0xFFFF:
        sys.exit("bitmap is %d bytes; bitmapOffset is uint16_t" % len(bitmap))

    src = os.path.basename(a.font)
    out = []
    w = out.append
    w("/*")
    w(" * %s - generated by extras/fontconvert.py, do not hand-edit." % (a.name + ".h"))
    w(" *")
    w(" * Source: %s at %d px" % (src, a.size))
    w(" * Range:  0x%02X..0x%02X (%d glyphs)" % (first, last, last - first + 1))
    w(" * Size:   %d bytes bitmap + %d bytes glyph table + %d struct = %d bytes flash"
      % (len(bitmap), (last - first + 1) * 7, 10, len(bitmap) + (last - first + 1) * 7 + 10))
    w(" * Line:   %d px baseline-to-baseline, %d px ascent" % (y_advance, ascent))
    if a.note:
        w(" *")
        for line in a.note.split("\n"):
            w(" * " + line)
    w(" */")
    w("#pragma once")
    w('#include "../CHGfx_gfxfont.h"')
    w("")

    w("const uint8_t %sBitmaps[] = {" % a.name)
    for i in range(0, len(bitmap), 12):
        w("    " + " ".join("0x%02X," % b for b in bitmap[i:i + 12]))
    w("};")
    w("")

    w("const GFXglyph %sGlyphs[] = {" % a.name)
    for (code, off, gw, gh, adv, xo, yo) in glyphs:
        ch = chr(code)
        label = {0x20: "space", 0x5C: "backslash"}.get(code, ch)
        if code == 0x2A:
            label = "asterisk"
        w("    { %5d, %3d, %3d, %3d, %4d, %4d },   /* 0x%02X %s */"
          % (off, gw, gh, adv, xo, yo, code, label))
    w("};")
    w("")

    w("const GFXfont %s = {" % a.name)
    w("    (uint8_t  *)%sBitmaps," % a.name)
    w("    (GFXglyph *)%sGlyphs," % a.name)
    w("    0x%02X, 0x%02X, %d" % (first, last, y_advance))
    w("};")

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
