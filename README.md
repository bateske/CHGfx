# CHGfx

Wire-speed ST7735 graphics for the **CH32X035** (QingKe V4C, 48 MHz,
20 KB SRAM). Built for the CHGame handheld — ST7735S 1.44" 128×128 on
SPI1 — but the control pins are remappable.

**90 fps** full-frame at 16 bpp, **119 fps** at 12 bpp, against **2.4 fps**
for naive per-pixel drawing. That is 98% of the chip's theoretical SPI
bandwidth; there is no meaningful headroom left.

On top of the transport: sprites in two formats (including palette-swapped
span sprites that scale and rotate), a clip rectangle, rounded rectangles,
ellipses, dither fills, word-speed row operations, palette fades,
outlined gradient banner text, a tiny 3×5 font, and a PC simulator that
runs your sketch and catches the classic tearing bugs.

See [PERFORMANCE.md](../PERFORMANCE.md) for how those numbers were
reached and the full datasheet reasoning.

## Install

Copy the `CHGfx` folder into your Arduino `libraries/` directory, or in
the IDE use **Sketch ▸ Include Library ▸ Add .ZIP Library**. Then:

```bash
arduino-cli compile -b CHGame:ch32v:CHGame:opt=o2std CHGfx/examples/HelloGraphics
```

Set **Tools ▸ Optimize ▸ Faster (-O2)** in the IDE. The board defaults to
`-Os`, which costs 10–50% on the drawing primitives. The flush's hot
loops run from SRAM whatever the setting.

## Two ways to draw

**Framebuffer mode** (the default) keeps a 4 bpp, 16-colour buffer in
SRAM. Right for sprites, geometry and text - anything that touches some
pixels and leaves the rest alone.

**Direct mode** (`gfx_stream`) has no framebuffer at all. Your callback
computes pixels straight into the buffer DMA is about to transmit. No
framebuffer means no 16-colour limit, so you get the panel's **full**
depth: 65,536 colours at 16 bpp, or 262,144 at 18 bpp. Right for
procedural effects where every pixel changes every frame.

The `Demoscene` example uses both, and composites them in one pass.

## The one thing to understand

A full 128×128 RGB565 framebuffer is **32 KB**. This chip has **20 KB**.
So CHGfx keeps a **4 bpp, 16-colour** framebuffer (8 KB) plus a palette,
and expands it to RGB565 or RGB444 on the way out through a lookup table
while DMA is already transmitting. The conversion keeps up with the wire
with room to spare, so **the framebuffer costs no frame rate** — but it
is not free for the CPU: see [What a flush costs the CPU](#what-a-flush-costs-the-cpu).

Consequence: **colours are palette indices 0–15, not RGB565.** Set the
palette once, then draw with indices. The upside: every frame goes out
through the palette, so recolouring a slot recolours every pixel that
uses it, for nothing.

## Quick start

```cpp
#include <CHGfx.h>

enum : uint8_t { BLACK, DARKGREY, GREY, LIGHTGREY, WHITE,
                 RED, ORANGE, YELLOW, GREEN, DARKGREEN,
                 CYAN, BLUE, NAVY, MAGENTA, PURPLE, PINK };

static const uint16_t palette[16] = {
    0x0000, 0x18E3, 0x4208, 0xC618, 0xFFFF,
    0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x0400,
    0x07FF, 0x001F, 0x0010, 0xF81F, 0x8010, 0xFC9F
};

void setup() {
    Gfx.begin();                     // 24 MHz SPI, 16 bpp
    Gfx.setPalette(palette, 16);
}

void loop() {
    Gfx.wait();                      // previous frame finished shifting out
    Gfx.clear(NAVY);
    Gfx.fillCircle(64, 64, 30, RED);
    Gfx.print(4, 4, "hello", WHITE);
    Gfx.displayAsync();              // returns immediately
    // anything here overlaps the ~11 ms transfer
}
```

`Gfx` is a global instance. Every method is a zero-cost inline forwarder
to a `gfx_*` free function — use whichever style you prefer, they compile
to the same thing. There is one panel, one SPI peripheral and one DMA
channel, so the state is inherently global; the class exists for the
familiar spelling, not to allow two instances.

## Getting more frame rate

Four levers, in order of payoff:

**1. Partial updates.** The wire is the bottleneck, so cost scales with
*area*. Send only what changed:

| Rect | Bytes | Time | fps |
|---|---:|---:|---:|
| 128×128 | 32768 | 11.1 ms | 89 |
| 96×96 | 18432 | 6.3 ms | 158 |
| 64×64 | 8192 | 2.8 ms | 354 |
| 32×32 | 2048 | 0.75 ms | 1336 |

```cpp
Gfx.displayRect(x, y, w, h);         // or displayRectAsync()
```

`x` and `w` are rounded **outward** to a multiple of 2 (16 bpp) or 8
(12 bpp), because two pixels share a byte. Rounding out is always safe.

**2. Colour depth is a dial, in both directions.** Three modes, and the
frame rate is just the byte count:

| Mode | Bytes/frame | Frame | fps | Colours |
|---|---:|---:|---:|---:|
| `GFX_12BPP` RGB444 | 24576 | 8.4 ms | 119 | 4,096 |
| `GFX_16BPP` RGB565 | 32768 | 11.1 ms | 90 | 65,536 |
| `GFX_18BPP` RGB666 | 49152 | 16.4 ms | 61 | 262,144 |

```cpp
Gfx.begin(GFX_DIV2, GFX_12BPP);     // or GFX_16BPP / GFX_18BPP
```

12 bpp costs nothing visually behind a 16-colour palette and is a free
25%. 18 bpp buys one more bit of red and one of blue for a third of the
frame rate - worth it for smooth gradients in direct mode, pointless for
sprite work. Unlike 12 bpp it does not pass through the panel's RGBSET
conversion table, so there is nothing to program.

**3. Direct mode, when every pixel changes.** A plasma or a tunnel gains
nothing from a framebuffer - it would compute every pixel, store it,
then read it back. `gfx_stream()` skips the round trip and drops the
16-colour ceiling with it:

```cpp
void myEffect(uint8_t *dst, int y0, int rows, void *user) {
    uint16_t *d = (uint16_t *)dst;             // RGB565, 16 bpp
    for (int r = 0; r < rows; r++)
        for (int x = 0; x < GFX_W; x++)
            *d++ = someColour(x, y0 + r);
}
Gfx.stream(myEffect);
```

The budget is **32 CPU cycles per pixel** at 16 bpp, 48 at 18 bpp. Stay
under it and you render at full wire speed; go over and the frame rate
degrades in proportion. Put the inner loop in SRAM - flash is 3 wait
states here and measures 2.2x slower on this kind of loop:

```cpp
#define FX __attribute__((section(".srodata.ramfunc.myeffect"), noinline))
FX static void myEffect(uint8_t *dst, int y0, int rows, void *user) { ... }
```

(Give each SRAM function a section name of its own, as above: functions
sharing one name are kept or dropped by the linker together.)

**4. Async present.** `displayAsync()` returns immediately; the transfer
runs on DMA while the main loop does physics, input, audio, AI.

### What a flush costs the CPU

The wire time is the DMA's, but the pixel conversion is the CPU's: it
runs inside the DMA interrupt, a chunk at a time, and the DMA shares the
bus with the CPU. Measured on the board, with a busy main loop running
through an async full-frame flush:

| | 16 bpp | 12 bpp |
|---|---:|---:|
| CPU taken from the main loop, full frame | **2.6–3.2 ms** | **2.5–2.9 ms** |
| … half frame | 1.4–1.6 ms | 1.3–1.5 ms |
| CPU left to the main loop during the flush | ~71–76% | ~66–70% |

(The range is `-O2` to `-Os`.) At 60 fps that is about a sixth of the
CPU, so budget for it — a game AI searching between frames will notice.
The `Benchmark` example measures it as test 7c. Building with
`-DCHGFX_ISR_IN_SRAM` moves the DMA interrupt itself into SRAM too:
4–9% cheaper, for about 330 bytes of SRAM.

Before 1.3, the board's default `-Os` quietly compiled the converters into
flash, where they ran at 3 wait states, and the same flush cost **5.1 ms**.

**The cheapest frame is the one you do not draw.** If nothing on screen
changed — a board game waiting for input, a paused menu — skip the drawing
and flush the buffer you already have. Palette animation still moves,
because the palette is applied during the flush, so a shimmering
highlight or a fade costs a flush and nothing else:

```cpp
void loop() {
    Gfx.wait();
    animatePalette();                 // staged, see "Palette tricks"
    if (stateChanged) drawEverything();
    Gfx.displayAsync();
}
```

### Racing the beam

With one framebuffer you must not draw into rows the flush has not sent
yet. But the flush goes top to bottom, and a row it has converted is
never read again — so the top of the next frame can be drawn while the
bottom of this one is still going out:

```cpp
Gfx.waitRow(64);  Gfx.setClip(0, 0, 128, 64);   drawTopHalf();
Gfx.wait();       Gfx.setClip(0, 64, 128, 64);  drawBottomHalf();
Gfx.resetClip();  Gfx.displayAsync();
```

`flushRow()` is the first row the flush may still read (128 when idle);
`waitRow(y)` waits until rows above `y` are free. The clip rectangle
makes sure nothing strays below the line. On a frame drawn in two halves
this measured **10–14% faster** frames (test 14 in `Benchmark`). It only
helps when your drawing really does go top to bottom.

## Drawing

### The clip rectangle

```cpp
Gfx.setClip(0, 10, 128, 118);   // the playfield, under a 10-pixel HUD
drawBoard();                     // nothing lands outside it
Gfx.resetClip();
```

Everything that paints pixels honours it: fills, lines, shapes, text,
both sprite formats, `clear()`. It is intersected with the screen and
costs nothing to change. `getClip()` reads it back if you want to nest.
The flush, `getPixel()` and `scroll()` ignore it.

### Sprites

Two formats.

**Bitmaps** (`drawSprite`) are 4 bpp, packed exactly like the
framebuffer: 2 px per byte, even x in the low nibble, each row padded to
a whole byte.

```cpp
Gfx.drawSprite(data, x, y, w, h, /*transparent=*/0);   // index 0 = see-through
Gfx.drawSprite(data, x, y, w, h);                      // opaque
```

**Even width blitted to an even x with no transparency hits a byte-copy
path.** Worth designing your art around.

**Span sprites** (`drawSprite4`) store each row as runs of one colour —
smaller than a bitmap, and a run is a fill, not a pixel loop. Every pixel
goes through a 16-entry **remap** table, so one image serves many looks:
a team colour, a red damage flash, a white hit flash, a black silhouette.
With 16 colours, palette swapping is the natural way to get variety.

```cpp
#include "slime.h"                   // python extras/sprite4.py slime.png SLIME > slime.h

static uint8_t blueTeam[16];         // identity, with the body colours swapped
Gfx.drawSprite4(SLIME, x, y);                    // as drawn
Gfx.drawSprite4(SLIME, x, y, blueTeam);          // recoloured
Gfx.drawSprite4(SLIME, x, y, blueTeam, 512);     // and twice the size (Q8 scale)
Gfx.drawSprite4Rot(SLIME, 8, 6, px, py, angle);  // turned about its pixel (8,6)
```

Colour 15 is transparent in the art (the remap can still produce 15).
Scale is Q8 — 256 is 1:1 — and 1:1 has its own fast path. The rotated
version decodes the art into the chunk scratch (1 KB: 32×64 or 45×45
at most) and draws pixel by pixel, so it is several times the cost —
right for a tumbling piece, not for a whole army.

`extras/sprite4.py` packs indexed or RGBA PNGs, whole sheets included
(`--tile W H`).

| 16×12 sprite, `-O2` | Time |
|---|---:|
| `drawSprite4`, remapped | 81 µs |
| `drawSprite4`, 2× | 224 µs |
| `drawSprite4Rot` | 616 µs |
| `drawSprite` 16×16 bitmap, transparent | 102 µs |

### Shapes

```cpp
Gfx.fillRoundRect(x, y, w, h, r, c);   Gfx.drawRoundRect(x, y, w, h, r, c);
Gfx.fillEllipse(cx, cy, rx, ry, c);    Gfx.drawEllipse(cx, cy, rx, ry, c);
Gfx.dither(x, y, w, h, c, phase);      // 50% checkerboard: darkened backdrops
Gfx.remapRect(x, y, w, h, table);      // recolour what is already there
```

Rounded corners are pixel-art arcs (radius 1–4 give the familiar
`{1}`, `{2,1}`, `{3,1,1}`, `{4,2,1,1}` insets) and work at any radius.
Ellipses are integer-only, no square roots — a shadow under a sprite
costs 23 µs.

### Row operations

`newlib-nano`'s `memmove` and `memcpy` are byte loops in flash on this
part. Moving the framebuffer with them is slow; these are word copies in
SRAM:

```cpp
Gfx.scroll(10, 118, dx, dy);          // shake rows 10..127 (odd dx is fine)
Gfx.scroll(10, 118, 0, -1, SKY);      // scroll up, fill the new row
Gfx.copyRow(y, patternRow, x0, x1);   // stamp a prepared row: floors, tiles
```

| Moving 118 rows | Time |
|---|---:|
| `memmove` | 6.3 ms |
| `Gfx.scroll` | 1.0 ms |

## Palette tricks

**Changes are staged.** `setPalette()` and `setPaletteEntry()` take
effect when the next flush starts, never halfway through the one in
flight, so they are safe to call at any time — every frame shows exactly
one palette. That makes palette animation free of drawing: cycle a slot
for a rainbow, pulse one for a highlight, and everything drawn in that
index follows.

**Fades** are applied while the palette is expanded, so they cost nothing
either:

```cpp
Gfx.setFade(amount);            // 0 = normal .. 255 = black
Gfx.setFade(amount, 0xFFFF);    // toward white: a hit flash
```

The palette as set stays in `gfx_pal[]` and `nearest()` still matches
against it; `gfx_paletteOut(i)` gives what index `i` actually shows.

## Text and fonts

The built-in 5×7 font costs 475 bytes and needs no setup. For anything
larger, smaller or proportional, `setFont()` takes a **GFXfont** — the
Adafruit_GFX bitmap font format, unchanged:

```cpp
#include <fonts/CHGfx_SansBold16.h>

Gfx.setFont(&CHGfx_SansBold16);
Gfx.print(4, 20, "GAME OVER", RED);
Gfx.setFont();                     // back to the built-in 5x7
```

Six fonts ship in `src/fonts/` — `Tiny3x5`, `Mono11`, `Sans12`,
`SansBold12`, `SansBold16` and a digits-only `Digits24` for scores —
costing 523 B to 1866 B each, and only the ones you `#include` are linked.

**`CHGfx_Tiny3x5`** is Press Play On Tape's 3×5 pixel font: 32 characters
across the screen, the most legible size that still fits a sentence on a
line. Right for HUDs and plates; at scale 2 it makes crisp menu text.

The format is byte-for-byte Adafruit's, so fonts move both ways with no
conversion: `Gfx.setFont(&FreeSans9pt7b)` works, and CHGfx's fonts work
under Adafruit_GFX's own `setFont()`. `extras/fontconvert.py` turns any
TTF into a new one.

**The origin changes with the font.** Built-in font: `y` is the top of
the glyph box. Custom font: `y` is the *baseline*. That is Adafruit's
convention and matching it is what makes the fonts interchangeable.
`Gfx.fontBaseline()` converts — it returns 0 for the built-in font and
the ascent for a custom one, so this always means "top of the text at
`y`":

```cpp
Gfx.print(x, y + Gfx.fontBaseline(), s, c);
```

`textWidth()`, `fontLineHeight()` and `textBounds()` are there for
centring, right-alignment and boxing. Full details, the metrics table and
the converter's options are in [FONTS.md](FONTS.md).

### Banner text

```cpp
Gfx.printFx(x, y, "CHECKMATE!", 3, GOLD, /*outline*/ BLACK, /*shadow*/ RED,
            ramp, wave);
```

Outlined, shadowed text, with an optional fill colour per pixel row
(`ramp`, for gradient lettering) and a vertical offset per character
(`wave`, for dancing letters). Any font, any scale. Instead of printing
the text nine times over, it renders it once into a 1 bpp mask in the
chunk scratch and grows the outline out of that, so a 3× outlined,
shadowed nine-letter banner costs about 2.9 ms. The text must fit the
1 KB mask (126×62 px); it returns `false` otherwise.

## The chunk scratch

`gfx_chunkScratch()` is 1 KB, word aligned, free between `wait()` and the
next flush: the flush's own chunk buffers. Use it for anything temporary
while you draw — decoding, masks, building a flash page to save. Never
across a flush. `drawSprite4Rot()` and `printFx()` use it too.

## The simulator

`extras/sim` runs a sketch on a PC: the library's real drawing code, with
a simulated panel in place of the SPI and DMA. It is the fastest way to
develop for the board.

```bash
python extras/sim/chsim.py run examples/GameKit --gif kit.gif --frames 300
python extras/sim/chsim.py run MySketch --png shots --input "60:A,64:"
python extras/sim/chsim.py test            # the library's own tests
```

Flushes take the time the board takes and convert rows as the DMA would,
so it catches the bugs that are hard to see on the glass: **drawing into
a frame that is still being sent** (with the row, and the `waitRow()`
that would fix it) and **using the chunk scratch during a flush**. `--cost`
also estimates your sketch's own CPU time on the board. See
[extras/sim/README.md](extras/sim/README.md).

## Already using Adafruit_GFX?

Adafruit_GFX is not what's slow — `Adafruit_ST7735` underneath it is.
`CHGfx_GFX` subclasses Adafruit_GFX and points it at CHGfx's framebuffer,
so `print()`, `setTextSize()`, custom GFXfonts, `drawBitmap()`, triangles
and rounded rects all keep working:

```cpp
#include <Adafruit_GFX.h>          // must be in the .ino, see below
#include <CHGfx.h>
#include <CHGfx_AdafruitGFX.h>

CHGfx_GFX tft;

void setup() {
    tft.begin();
    tft.setPalette(palette, 16);
    const uint8_t RED = tft.nearest(0xF800);   // map RGB565 -> palette index
    tft.fillScreen(0);
    tft.setCursor(4, 4);
    tft.setTextColor(RED);
    tft.print("same API, 37x faster");
    tft.display();                 // nothing reaches the panel until here
}
```

Two gotchas:

* **Colours are palette indices.** `tft.nearest(0xF800)` maps a real
  RGB565 to the closest slot — do it once at setup. Or define
  `CHGFX_GFX_AUTOMAP` to have it done automatically on every call
  (convenient for legacy code, but a 16-entry search per drawing call).
* **`#include <Adafruit_GFX.h>` must appear in your `.ino`**, above the
  CHGfx includes. The Arduino builder decides which library include paths
  to add by scanning the sketch file only, so a library reached solely
  through another library's header never gets resolved.

## Examples

| Example | What it shows |
|---|---|
| `HelloGraphics` | Minimum useful sketch; shapes, text, palette ramp |
| `GameKit` | The game-making kit: clip, span sprites with remaps, scaling and rotation, shapes, the 3×5 font, banner text, shake, palette animation and fades |
| `PartialUpdate` | Dirty-rect presenting vs full-frame, live fps comparison |
| `AdafruitGFXCompat` | Keeping your Adafruit_GFX code, swapping the transport |
| `Benchmark` | The full test suite, flush CPU cost included; prints to USB CDC and the panel |
| `Demoscene` | Eight-part demo: plasma, tunnel, rotozoomer, fire, 3D, copper bars |
| `Fonts` | Custom bitmap fonts, the baseline convention, aligned HUD text |

## Configuration

Override before including `CHGfx.h`, or with `-D` build flags:

| Macro | Default | Notes |
|---|---|---|
| `GFX_W`, `GFX_H` | 128, 128 | Framebuffer is W×H/2 bytes; watch the 20 KB budget. W a multiple of 8 |
| `GFX_CHUNK_ROWS` | 2 | Rows per DMA chunk; costs `W × rows × 4` bytes of SRAM |
| `CHGFX_CS_PORT` / `_PIN` | `GPIOA`, 4 | |
| `CHGFX_DC_PORT` / `_PIN` | `GPIOB`, 0 | |
| `CHGFX_RST_PORT` / `_PIN` | `GPIOB`, 12 | |
| `CHGFX_SDCS_PORT` / `_PIN` | `GPIOB`, 11 | Shared-bus SD card, parked high |
| `CHGFX_NO_SD_PARK` | unset | Define if nothing else shares SPI1 |
| `CHGFX_ISR_IN_SRAM` | unset | DMA interrupt in SRAM: 4–9% less flush CPU, ~330 B more SRAM |

These are compiled into the library's own files, so they have to be
build flags (`--build-property build.extra_flags=-DGFX_CHUNK_ROWS=4`
with `arduino-cli`), not `#define`s in the sketch.

SCK and MOSI are **not** configurable: they are SPI1's pins (PA5, PA7),
and SPI1 is the only peripheral with a DMA path to those lines.

Panel geometry defaults match Adafruit's `INITR_144GREENTAB` (MADCTL
`0xC8`, colstart 2, rowstart 3). If the image is offset or mirrored:

```cpp
Gfx.setPanelOffsets(0xC8, 2, 3);
```

### What it costs in SRAM

Hot loops live in SRAM, and each is in a section of its own, so the
linker keeps only what your sketch calls. `HelloGraphics` uses 11.9 KB of
SRAM in all, the core's included (8 KB of it the framebuffer, 1 KB the
chunk buffers, 1 KB the palette table); `GameKit`, which uses nearly
everything, 13.9 KB. The board leaves a sketch 18 KB, plus 2 KB of stack.

The SRAM code is placed at the start of `.data`, ahead of the small
variables the global pointer reaches with one instruction: placed after
them, it pushes the sketch's variables out of that 4 KB window and every
access to them grows. See PERFORMANCE.md for how much that mattered.

### A note about GPIOB pins 8–15

The CH32X035's `CFGHR` register — the pin config for PB8…PB15, which on
this board is the buttons, the LED, the buzzer, SD_CS and LCD_RST — is
**write-only**. Reading it back does not return what was written, so the
vendor GPIO driver keeps a RAM shadow (`CFGHR_tmpB`) and rebuilds the
whole register from it on every `pinMode()`.

CHGfx writes through that same shadow, so its pin config and the core's
compose in either order. If you configure pins in that range with raw
register writes of your own, do the same — a read-modify-write of
`CFGHR` will silently reconfigure every other pin in the register.

Getting this wrong on LCD_RST specifically is nasty: the pin reverts to
an input, `LCD_RST` runs straight from the MCU to the panel with no
pull-up on the net, and after about a second idle the floating line
drifts low. The ST7735 reads that as a reset pulse and blanks to white
while the CPU keeps streaming frames, unaware. CHGfx re-asserts RST as a
driven-high output on every `setWindow()` as a backstop.

## A caution about the SPI clock

The default is HCLK/2 = **24 MHz**. The ST7735S datasheet specifies
tSCYCW ≥ 66 ns, i.e. **15.1 MHz**, so this is out of spec — it works on
short flex because the same table gives 15 ns high + 15 ns low pulse
widths, which is where panels actually limit.

The `Benchmark` example sweeps 6/12/24 MHz drawing 1-pixel vertical
stripes, the worst case for setup and hold. Clean stripes are fine;
speckle or horizontal shear means back off:

```cpp
Gfx.begin(GFX_DIV4);      // 12 MHz, comfortably in spec
```

## API

Class methods (on `Gfx`) and the equivalent free functions:

| Class | Free function |
|---|---|
| `begin(div, mode)` | `gfx_begin` |
| `display()` / `displayAsync()` | `gfx_flush` / `gfx_flushAsync` |
| `stream(fn, user)` | `gfx_stream` |
| `displayRect()` / `displayRectAsync()` | `gfx_flushRect` / `gfx_flushRectAsync` |
| `busy()` / `wait()` | `gfx_busy` / `gfx_wait` |
| `flushRow()` / `waitRow()` | `gfx_flushRow` / `gfx_waitRow` |
| `setClip()` / `resetClip()` / `getClip()` | `gfx_setClip` / `gfx_resetClip` / `gfx_getClip` |
| `clear()` | `gfx_clear` |
| `drawPixel()` / `getPixel()` | `gfx_pixel` / `gfx_getPixel` |
| `drawFastHLine()` / `drawFastVLine()` | `gfx_hline` / `gfx_vline` |
| `fillRect()` / `drawRect()` | `gfx_fillRect` / `gfx_rect` |
| `fillRoundRect()` / `drawRoundRect()` | `gfx_fillRoundRect` / `gfx_roundRect` |
| `drawLine()` | `gfx_line` |
| `drawCircle()` / `fillCircle()` | `gfx_circle` / `gfx_fillCircle` |
| `drawEllipse()` / `fillEllipse()` | `gfx_ellipse` / `gfx_fillEllipse` |
| `dither()` / `remapRect()` | `gfx_dither` / `gfx_remapRect` |
| `drawSprite()` | `gfx_blit` |
| `drawSprite4()` / `drawSprite4Rot()` | `gfx_sprite4` / `gfx_sprite4Rot` |
| `scroll()` / `copyRow()` | `gfx_scroll` / `gfx_copyRow` |
| `drawChar()` / `print()` | `gfx_char` / `gfx_charScaled` / `gfx_text` / `gfx_textScaled` |
| `printFx()` | `gfx_textFx` |
| `setFont()` / `font()` | `gfx_setFont` / `gfx_font` |
| `fontLineHeight()` / `fontBaseline()` | `gfx_fontLineHeight` / `gfx_fontBaseline` |
| `textWidth()` / `textBounds()` | `gfx_textWidth` / `gfx_textWidthScaled` / `gfx_textBounds` |
| `setPalette()` / `setPaletteEntry()` / `nearest()` | `gfx_setPalette` / `gfx_setPaletteEntry` / `gfx_nearest` |
| `setFade()` / `fade()` | `gfx_setFade` / `gfx_fade` / `gfx_paletteOut` |
| `setColorMode()` / `setSpiDiv()` | `gfx_setColorMode` / `gfx_setSpiDiv` |
| `fillRectDirect()` | `gfx_directFillRect` |
| `buffer()` | `gfx_fb` |

`gfx_fb` is the raw 4 bpp buffer if you want to write your own
primitives, and `gfx_chunkScratch()` the 1 KB of scratch. Low-level
escape hatches (`gfx_cmd`, `gfx_data8`, `gfx_setWindow`,
`gfx_select`/`gfx_deselect`, `gfx_directBlit`) are in `CHGfx.h` with the
reasoning inline.

## What it looks like flat out

The `Demoscene` example, measured on hardware. Every pixel of the direct
parts is computed per frame — no framebuffer, no sprites, no cheating:

| Part | Mode | fps |
|---|---|---:|
| Starfield + text | framebuffer, 16 col | 73 |
| Plasma | direct, 65,536 col | 82 |
| Plasma | direct, 262,144 col (18 bpp) | 53 |
| Textured tunnel | direct, 65,536 col | 82 |
| Rotozoomer | direct, 65,536 col | 83 |
| Fire | direct, 65,536 col | 63 |
| Shaded solid, 20 tris | framebuffer, 16 col | 60 |
| Copper bars + scroller | hybrid | 79 |

The direct parts run at 82–83 fps against a 90 fps hard wire ceiling, so
the per-pixel math is genuinely fitting inside its 32-cycle budget —
about **1.35 million computed pixels per second** from a 48 MHz core
with no FPU, no cache and no blitter.

## Licence

MIT, except the font data. The 5×7 glyphs come from Adafruit's
`glcdfont.c` under BSD, as does the `GFXfont` struct layout; the fonts in
`src/fonts/` are rasterized from DejaVu Sans, which is freely
redistributable, except `CHGfx_Tiny3x5`, which is Press Play On Tape's
3×5 font under the Apache License 2.0 ([LICENSE.Apache-2.0](LICENSE.Apache-2.0)).
Full notices in [LICENSE](LICENSE).
