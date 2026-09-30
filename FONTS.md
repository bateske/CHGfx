# Fonts

CHGfx ships a 5×7 font built in and supports **GFXfont**, the
Adafruit_GFX bitmap font format, for anything larger or proportional.

```cpp
#include <CHGfx.h>
#include <fonts/CHGfx_Sans12.h>

Gfx.setFont(&CHGfx_Sans12);
Gfx.print(4, 20, "Hello", WHITE);
Gfx.setFont();                     // back to the built-in 5x7
```

The format is byte-for-byte Adafruit's, down to the `_GFXFONT_H_` include
guard, so it works in both directions with no conversion:

```cpp
#include <Fonts/FreeSans9pt7b.h>   // from the Adafruit GFX Library
Gfx.setFont(&FreeSans9pt7b);       // an Adafruit font, in CHGfx

tft.setFont(&CHGfx_Sans12);        // a CHGfx font, in Adafruit_GFX
```

## The origin changes with the font

**Built-in font: `y` is the top of the glyph box. Custom font: `y` is the
baseline.** Ascenders sit above the `y` you pass and descenders hang
below it. That is Adafruit_GFX's convention, and matching it is what lets
font data and sketches move between the two libraries unedited.

`Gfx.fontBaseline()` is the conversion. It returns 0 for the built-in
font and the ink ascent for a custom one, so this means "top of the text
at `y`" whichever font is selected:

```cpp
Gfx.print(x, y + Gfx.fontBaseline(), s, c);
```

## The bundled fonts

In `src/fonts/`. Only the ones you `#include` are linked, and flash is
the scarce resource here — the CH32X035G8U6 gives a sketch about 50 KB
after the bootloader.

| Font | Glyphs | Line | Baseline | Digit | Flash | Notes |
|---|---:|---:|---:|---:|---:|---|
| built-in 5×7 | 0x20–0x7E | 8 | 0 | 6 | **475 B** | Always there, no `#include` |
| `CHGfx_Tiny3x5` | 0x20–0x7E | 7 | 5 | 4 | **857 B** | 3×5 pixels, 32 characters a line: HUDs, plates |
| `CHGfx_Mono11` | 0x20–0x7E | 14 | 9 | 7 | **1233 B** | Fixed pitch, 18 columns across |
| `CHGfx_Sans12` | 0x20–0x7E | 15 | 10 | 8 | **1344 B** | Proportional body text |
| `CHGfx_SansBold12` | 0x20–0x7E | 15 | 10 | 8 | **1444 B** | Same size, reads on busy art |
| `CHGfx_SansBold16` | 0x20–0x7E | 19 | 13 | 11 | **1866 B** | Titles, menu headings |
| `CHGfx_Digits24` | `+` – `:` | 29 | 18 | 17 | **523 B** | `+,-./0123456789:` only |

"Line" is baseline-to-baseline spacing (`fontLineHeight()`), "Baseline"
is the ink ascent (`fontBaseline()`), "Digit" is the advance width of a
numeral — all in pixels at scale 1.

`CHGfx_Digits24` is the shape to copy when flash is tight: a display font
you only ever point at a score or a timer does not need 95 glyphs. Sixteen
characters buy a 24 px face for a third of what the small text faces cost.
Characters outside a font's range are drawn as `?`, or dropped when the
font has no `?` — a space is *not* in `CHGfx_Digits24`, so pad with `0`
or leave gaps with cursor arithmetic.

The five larger fonts are rasterized from DejaVu Sans, which is freely
redistributable; the notice is in [LICENSE](LICENSE).

`CHGfx_Tiny3x5` is the other way round: a pixel font drawn by hand, Press
Play On Tape's 3×5 from "Blackjack" for the Arduboy (Apache License 2.0,
see [LICENSE.Apache-2.0](LICENSE.Apache-2.0)), completed here to the whole
printable ASCII range. Capitals are 5 px tall and lowercase 4, with a
one-pixel descender; every character advances 4 px, so text measures
`4 × length - 1` pixels of ink. It is sharp at scale 2 and 3 as well —
6×10 menu text, or banner lettering with `printFx()`:

```cpp
#include <fonts/CHGfx_Tiny3x5.h>

Gfx.setFont(&CHGfx_Tiny3x5);
Gfx.print(2, 2 + Gfx.fontBaseline(), "SCORE 001234", WHITE);
Gfx.print(10, 60, "PRESS A", YELLOW, 2);
```

## Metrics

```cpp
int  Gfx.fontLineHeight();                    // baseline-to-baseline
int  Gfx.fontBaseline();                      // ink above the baseline
int  Gfx.textWidth(s);                        // advance width, for alignment
int  Gfx.textWidth(s, scale);
void Gfx.textBounds(s, x, y, scale, &bx, &by, &bw, &bh);   // box around the text
```

Centring and right-alignment:

```cpp
Gfx.print((GFX_W - Gfx.textWidth(s)) / 2, y + Gfx.fontBaseline(), s, c);
Gfx.print(right - Gfx.textWidth(s), y + Gfx.fontBaseline(), s, c);
```

`\n` starts a new line back at the x you started from, in both fonts.
`textBounds()` returns the tight ink box for a custom font and the 5×7
cell box for the built-in one, so a leading space still counts there.

Integer scaling works with custom fonts the same way it does with the
built-in one — `Gfx.print(x, y, s, c, 2)` — though a bigger font almost
always looks better than a doubled small one.

## Converting your own

`extras/fontconvert.py` turns any TTF or OTF into a header in this
format. It needs Pillow, and it is what produced everything above.

```bash
pip install pillow
python extras/fontconvert.py DejaVuSans.ttf 12 MyFont > src/fonts/MyFont.h
```

Arguments are the font file, the **pixel em size**, and the C symbol
name. Useful options:

| Option | Effect |
|---|---|
| `--first N` `--last N` | Character range, default 0x20–0x7E |
| `--chars STR` | Only these characters; the range spans them |
| `--line N` | Override baseline-to-baseline spacing |
| `--gray` | Antialias then threshold, instead of asking FreeType for a hinted 1-bit bitmap. Sometimes better above ~16 px, usually worse below |
| `--threshold N` | Cut-off for `--gray`, 1–255 |

The generated header's comment block reports the exact flash cost, the
line height and the baseline, so you can size a font against your budget
before you link it.

A note on sizes: the pixel size is the **em** size handed to FreeType,
not the cap height and not the line height. DejaVu Sans at 12 px is about
10 px from baseline to the top of a capital. Below roughly 10 px, hinted
TTF output gets mushy and the built-in 5×7 is usually the better answer.

Adafruit's own `fontconvert` C tool produces identical output and works
here too, as does every font in the Adafruit GFX Library's `Fonts/`
directory. Be warned that those are sized for 141 dpi, so they are much
larger than their names suggest: `FreeSans9pt7b` is a 22 px line and
measured 1932 bytes linked, against 15 px and 1344 for `CHGfx_Sans12`.

## Cost at runtime

Glyph bitmaps are a row-major MSB-first bitstream with no row padding, so
every bit is consumed in order even where a pixel is clipped away. The
inner loop skips eight columns at a time on an all-zero byte, which is
most of a typical glyph, and writes framebuffer nibbles directly rather
than going through `gfx_pixel()`.

Text is not on the hot path for a game frame — a HUD is tens of glyphs
against 16384 pixels — but if you are drawing a full screen of text every
frame, the built-in 5×7 is meaningfully cheaper: its glyphs are one byte
per column with the row stride hoisted, and it never touches a bitstream.
