"""Pack a PNG into CHGfx's span-sprite format, for gfx_sprite4().

    python sprite4.py hero.png HERO > hero.h
    python sprite4.py sheet.png WALK --tile 16 24 > walk.h
    python sprite4.py art.png ART --palette 000000,1d2b53,7e2553,... > art.h

The format: w, h, then for every row the number of runs n, then n bytes
of (len - 1) << 4 | colour. Runs are 1..16 pixels; colour 15 means
"transparent, skip" and trailing transparency is left out. Drawing a run
is a fill, so the art is both small and fast.

Colours. Art may use palette indices 0..14; 15 is the skip code. (Pass a
remap table to gfx_sprite4() to draw any colour, 15 included.)
  * An indexed PNG (mode P) is taken as-is: pixel values are the indices.
    The PNG's transparent index, or --transparent N, marks see-through
    pixels.
  * An RGB or RGBA PNG needs --palette: your 16 colours as hex RGB, in
    index order, comma-separated or one per line in a file. Each pixel
    takes the nearest of entries 0..14; alpha below 128 is transparent.

--tile W H cuts the image into W x H frames, left to right, top to
bottom, and also emits a table of them: NAME[i] is frame i.

Needs Pillow (pip install pillow).
"""
import argparse
import sys
from pathlib import Path

SKIP = 15


def parse_palette(spec):
    p = Path(spec)
    text = p.read_text() if p.exists() else spec
    cols = [c.strip().lstrip("#") for c in text.replace("\n", ",").split(",") if c.strip()]
    cols = [c[2:] if c.lower().startswith("0x") else c for c in cols]
    if len(cols) < 15:
        raise SystemExit("sprite4: --palette needs at least 15 colours")
    return [tuple(int(c[i:i + 2], 16) for i in (0, 2, 4)) for c in cols[:16]]


def to_indices(im, palette, transparent):
    """Image -> rows of colour indices, SKIP for transparent pixels."""
    w, h = im.size
    if im.mode == "P" and palette is None:
        tr = transparent
        if tr is None and "transparency" in im.info and isinstance(im.info["transparency"], int):
            tr = im.info["transparency"]
        px = im.load()
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                v = px[x, y]
                if v == tr:
                    row.append(SKIP)
                elif v >= SKIP:
                    raise SystemExit(f"sprite4: pixel ({x},{y}) uses index {v}; art may use 0..14 "
                                     f"(15 is transparent - remap to draw it)")
                else:
                    row.append(v)
            rows.append(row)
        return rows
    if palette is None:
        raise SystemExit("sprite4: an RGB image needs --palette (or save it as an indexed PNG)")
    im = im.convert("RGBA")
    px = im.load()
    cache = {}
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                row.append(SKIP)
                continue
            key = (r, g, b)
            if key not in cache:
                cache[key] = min(range(15), key=lambda i: (palette[i][0] - r) ** 2 * 2 +
                                 (palette[i][1] - g) ** 2 * 4 + (palette[i][2] - b) ** 2 * 3)
            row.append(cache[key])
        rows.append(row)
    return rows


def pack(rows):
    h, w = len(rows), len(rows[0])
    if w > 255 or h > 255:
        raise SystemExit("sprite4: frames are at most 255x255")
    out = [w, h]
    for row in rows:
        runs, x = [], 0
        while x < w:
            c, s = row[x], x
            while x < w and row[x] == c and x - s < 16:
                x += 1
            runs.append((x - s, c))
        while runs and runs[-1][1] == SKIP:
            runs.pop()
        out.append(len(runs))
        out += [((n - 1) << 4) | c for n, c in runs]
    return out


def unpack(data):
    """The inverse, for checking: -> rows of indices, SKIP where transparent."""
    w, h, i = data[0], data[1], 2
    rows = []
    for _ in range(h):
        n = data[i]
        i += 1
        row = []
        for b in data[i:i + n]:
            row += [b & 15] * ((b >> 4) + 1)
        i += n
        rows.append(row + [SKIP] * (w - len(row)))
    return rows


def c_array(name, data):
    body = ",\n".join("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) for i in range(0, len(data), 12))
    return f"const uint8_t {name}[{len(data)}] = {{\n{body}\n}};"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("png")
    ap.add_argument("name")
    ap.add_argument("--palette")
    ap.add_argument("--transparent", type=int)
    ap.add_argument("--tile", type=int, nargs=2, metavar=("W", "H"))
    a = ap.parse_args()

    from PIL import Image
    im = Image.open(a.png)
    pal = parse_palette(a.palette) if a.palette else None
    rows = to_indices(im, pal, a.transparent)
    W, H = im.size
    tw, th = a.tile if a.tile else (W, H)
    frames = []
    for ty in range(0, H - th + 1, th):
        for tx in range(0, W - tw + 1, tw):
            frames.append([r[tx:tx + tw] for r in rows[ty:ty + th]])

    raw = tw * th * len(frames) // 2
    out = [f"/* {Path(a.png).name} - generated by CHGfx extras/sprite4.py, for gfx_sprite4(). */",
           "#pragma once", "#include <stdint.h>", ""]
    total = 0
    if len(frames) == 1:
        data = pack(frames[0])
        assert unpack(data) == frames[0]
        out.append(c_array(a.name, data))
        total = len(data)
    else:
        for i, f in enumerate(frames):
            data = pack(f)
            assert unpack(data) == f
            out.append(c_array(f"{a.name}_{i}", data))
            total += len(data)
        out.append(f"const uint8_t *const {a.name}[{len(frames)}] = {{ " +
                   ", ".join(f"{a.name}_{i}" for i in range(len(frames))) + " };")
    print("\n".join(out))
    print(f"sprite4: {len(frames)} frame(s) of {tw}x{th}, {total} bytes "
          f"(plain 4 bpp would be {raw})", file=sys.stderr)


if __name__ == "__main__":
    main()
