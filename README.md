# CHGfx

Wire-speed ST7735 graphics for the **CH32X035** (QingKe V4C, 48 MHz,
20 KB SRAM). Built for the CHGame handheld — ST7735S 1.44" 128×128 on
SPI1 — but the control pins are remappable.

**90 fps** full-frame at 16 bpp, **119 fps** at 12 bpp, against **2.4 fps**
for naive per-pixel drawing. That is 98% of the chip's theoretical SPI
bandwidth; there is no meaningful headroom left.

See [PERFORMANCE.md](../PERFORMANCE.md) for how those numbers were
reached and the full datasheet reasoning.

## Install

Copy the `CHGfx` folder into your Arduino `libraries/` directory, or in
the IDE use **Sketch ▸ Include Library ▸ Add .ZIP Library**. Then:

```bash
arduino-cli compile -b CHGame:ch32v:CHGame:opt=o2std CHGfx/examples/HelloGraphics
```

Set **Tools ▸ Optimize ▸ Faster (-O2)** in the IDE. The board defaults to
`-Os`, which costs 10–20% on the drawing primitives.

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
while DMA is already transmitting. Conversion costs 1.5 ms per frame
against 11 ms of wire time, so **the framebuffer is effectively free**.

Consequence: **colours are palette indices 0–15, not RGB565.** Set the
palette once, then draw with indices.

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
#define FX __attribute__((section(".srodata.ramfunc"), noinline))
FX static void myEffect(uint8_t *dst, int y0, int rows, void *user) { ... }
```

**4. Async present.** `displayAsync()` returns immediately and keeps
**93–98% of the CPU** available during the transfer.

Note the honest limit: with a single framebuffer you must `wait()` before
drawing the next frame, so *drawing* cannot overlap the transfer — only
work that doesn't touch the framebuffer (physics, input, audio, AI).
Double-buffering would fix that but needs another 8 KB, and only ~6 KB
remain.

## Sprites

4 bpp, packed exactly like the framebuffer: 2 px per byte, even x in the
low nibble, each row padded to a whole byte.

```cpp
Gfx.drawSprite(data, x, y, w, h, /*transparent=*/0);   // index 0 = see-through
Gfx.drawSprite(data, x, y, w, h);                      // opaque
```

**Even width blitted to an even x with no transparency hits a byte-copy
path.** Worth designing your art around: it is measurably the fastest
case, and transparent blits were the single largest CPU cost in a real
game frame.

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
| `PartialUpdate` | Dirty-rect presenting vs full-frame, live fps comparison |
| `AdafruitGFXCompat` | Keeping your Adafruit_GFX code, swapping the transport |
| `Benchmark` | The full 12-test suite; prints to USB CDC and the panel |
| `Demoscene` | Eight-part demo: plasma, tunnel, rotozoomer, fire, 3D, copper bars |

## Configuration

Override before including `CHGfx.h`, or with `-D` build flags:

| Macro | Default | Notes |
|---|---|---|
| `GFX_W`, `GFX_H` | 128, 128 | Framebuffer is W×H/2 bytes; watch the 20 KB budget |
| `CHGFX_CS_PORT` / `_PIN` | `GPIOA`, 4 | |
| `CHGFX_DC_PORT` / `_PIN` | `GPIOB`, 0 | |
| `CHGFX_RST_PORT` / `_PIN` | `GPIOB`, 12 | |
| `CHGFX_SDCS_PORT` / `_PIN` | `GPIOB`, 11 | Shared-bus SD card, parked high |
| `CHGFX_NO_SD_PARK` | unset | Define if nothing else shares SPI1 |

SCK and MOSI are **not** configurable: they are SPI1's pins (PA5, PA7),
and SPI1 is the only peripheral with a DMA path to those lines.

Panel geometry defaults match Adafruit's `INITR_144GREENTAB` (MADCTL
`0xC8`, colstart 2, rowstart 3). If the image is offset or mirrored:

```cpp
Gfx.setPanelOffsets(0xC8, 2, 3);
```

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
| `clear()` | `gfx_clear` |
| `drawPixel()` / `getPixel()` | `gfx_pixel` / `gfx_getPixel` |
| `drawFastHLine()` / `drawFastVLine()` | `gfx_hline` / `gfx_vline` |
| `fillRect()` / `drawRect()` | `gfx_fillRect` / `gfx_rect` |
| `drawLine()` | `gfx_line` |
| `drawCircle()` / `fillCircle()` | `gfx_circle` / `gfx_fillCircle` |
| `drawSprite()` | `gfx_blit` |
| `drawChar()` / `print()` | `gfx_char` / `gfx_text` / `gfx_textScaled` |
| `setPalette()` / `nearest()` | `gfx_setPalette` / `gfx_nearest` |
| `setColorMode()` / `setSpiDiv()` | `gfx_setColorMode` / `gfx_setSpiDiv` |
| `fillRectDirect()` | `gfx_directFillRect` |
| `buffer()` | `gfx_fb` |

`gfx_fb` is the raw 4 bpp buffer if you want to write your own
primitives. Low-level escape hatches (`gfx_cmd`, `gfx_data8`,
`gfx_setWindow`, `gfx_select`/`gfx_deselect`, `gfx_directBlit`) are in
`CHGfx.h` with the reasoning inline.

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

MIT, except the 5×7 font glyphs, which come from Adafruit's `glcdfont.c`
under BSD. See [LICENSE](LICENSE).
