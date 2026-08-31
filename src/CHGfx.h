/*
 * chgfx - a maximum-throughput ST7735 driver for the CHGame board
 *         (CH32X035G8U6 @ 48 MHz, QingKe V4C, 20 KB SRAM, 62 KB flash)
 *
 * WHY THIS EXISTS
 * ---------------
 * Adafruit_GFX + Adafruit_ST7735 on this part is slow for three separate
 * reasons, and they compound:
 *
 *   1. Every drawPixel() sends CASET + RASET + RAMWR (11 bytes of command
 *      traffic) to paint 2 bytes of pixel. ~85% of the bus is overhead.
 *   2. The Arduino SPI class transfers one byte at a time in a polled loop
 *      (cores/.../libraries/SPI/src/utility/spi_com.c spins on TXE for
 *      every single byte). The CPU is the bottleneck, not the wire.
 *   3. There is no framebuffer, so overdraw goes straight out over SPI.
 *
 * WHAT THIS DOES INSTEAD
 * ----------------------
 *   * Register-level SPI1 at HCLK/2 = 24 MHz. That is the CH32X035 hard
 *     ceiling: "Maximum clock frequency supports up to half of FHCLK"
 *     (Reference Manual ch.16.1), and HCLK cannot exceed 48 MHz because
 *     this part has no PLL, only the 48 MHz HSI RC (RM ch.3.3).
 *   * DMA1 channel 3 - the SPI1_TX request line (RM 9.2.3) - pushes whole
 *     chunks with zero CPU involvement.
 *   * 16-bit SPI data frames (DFF=1) so DMA does half as many bus cycles.
 *   * A 4 bpp (16-colour) framebuffer. 128*128 RGB565 is 32 KB and this
 *     chip has 20 KB, so a full-colour framebuffer is physically
 *     impossible. 4 bpp is 8 KB and leaves room to live.
 *   * A 256-entry uint32 lookup table expands one framebuffer byte
 *     (= 2 pixels) into one 32-bit store. Conversion costs ~3 cycles per
 *     pixel against a 32-cycle-per-pixel SPI budget, so the wire stays
 *     saturated and the framebuffer is effectively free.
 *   * Optional 12 bpp (RGB444) output - ST7735 COLMOD 0x03. Three bytes
 *     per two pixels instead of four: 25% less traffic, and with a
 *     16-colour palette you lose nothing you were actually using.
 *   * Ping-pong chunk buffers driven by the DMA transfer-complete ISR, so
 *     gfx_flushAsync() returns immediately and game logic overlaps the
 *     ~11 ms it takes to shift a frame out.
 *   * The conversion inner loops are linked into .data so they execute
 *     from SRAM. Flash on this part runs at 3 wait states above 24 MHz
 *     (RM 20.3.1, FLASH_ACTLR LATENCY = 10b); SRAM runs at zero.
 *
 * CEILING
 * -------
 *   24 MHz SPI = 3.0 MB/s.
 *   128*128*2   = 32768 B -> 10.9 ms/frame ->  91 fps  (16 bpp)
 *   128*128*1.5 = 24576 B ->  8.2 ms/frame -> 122 fps  (12 bpp)
 *   Nothing on this chip beats that: there is no parallel LCD interface,
 *   no second SPI on these pins, and no PLL to go past 48 MHz HCLK.
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>

#if !defined(CH32X035)
  #error "CHGfx targets the CH32X035. It drives SPI1 and DMA1 channel 3 directly and is not portable as-is. Select a CH32X035 board."
#endif

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */
/* Override before including, or with a -D build flag. Tested at
 * 128x128; the framebuffer is W*H/2 bytes, so keep an eye on the 20 KB
 * SRAM budget if you raise these (160x128 = 10 KB, still fits). W must
 * be even and W*H/2 must be a multiple of 32. */
#ifndef GFX_W
#define GFX_W            128
#endif
#ifndef GFX_H
#define GFX_H            128
#endif
#define GFX_FB_STRIDE    (GFX_W / 2)              /* 64 bytes per row  */
#define GFX_FB_BYTES     (GFX_FB_STRIDE * GFX_H)  /* 8192 bytes        */

/* Rows converted per DMA chunk. Two chunk buffers are allocated, so
 * this directly costs GFX_W * GFX_CHUNK_ROWS * 4 bytes of SRAM.
 *
 * 2 rows = 512 B per buffer, 1 KB total. Measured against the obvious
 * 4 rows on real hardware: gfx_flush went 11051 -> 11143 us, about
 * 0.8%, which is the same order as run-to-run variation. That buys back
 * a kilobyte, and on a part with 20 KB total a kilobyte is 5% of all
 * the memory there is - the right way round for this chip. Raise it if
 * you have RAM to spare and want the last percent. */
#define GFX_CHUNK_ROWS   2
#define GFX_CHUNK_BYTES  (GFX_W * GFX_CHUNK_ROWS * 2)   /* worst case, 16 bpp */

/* ------------------------------------------------------------------ */
/* Wiring                                                              */
/* ------------------------------------------------------------------ */
/*
 * SCK and MOSI are fixed: they are SPI1's pins on this package (PA5 and
 * PA7), and SPI1 is the only peripheral with a DMA path to these lines.
 * The three control pins are yours to move - redefine any of these
 * before including CHGfx.h, or with -D build flags.
 *
 * Defaults are the CHGame board (see variant_CHGame.h):
 *   PA4 = LCD_CS, PB0 = LCD_DC, PB12 = LCD_RST, PB11 = SD_CS
 * SD_CS is driven high at begin() because the microSD slot shares SPI1.
 */
#ifndef CHGFX_CS_PORT
#define CHGFX_CS_PORT    GPIOA
#define CHGFX_CS_PIN     4
#endif
#ifndef CHGFX_DC_PORT
#define CHGFX_DC_PORT    GPIOB
#define CHGFX_DC_PIN     0
#endif
#ifndef CHGFX_RST_PORT
#define CHGFX_RST_PORT   GPIOB
#define CHGFX_RST_PIN    12
#endif
/* Define CHGFX_NO_SD_PARK to skip driving a shared SD chip select high. */
#ifndef CHGFX_SDCS_PORT
#define CHGFX_SDCS_PORT  GPIOB
#define CHGFX_SDCS_PIN   11
#endif

/* ------------------------------------------------------------------ */
/* Colour output modes                                                 */
/* ------------------------------------------------------------------ */
enum : uint8_t {
    GFX_16BPP = 0,   /* ST7735 COLMOD 0x05, RGB565, 2 bytes/px   -> 90 fps */
    GFX_12BPP = 1,   /* ST7735 COLMOD 0x03, RGB444, 1.5 bytes/px -> 119 fps */
    GFX_18BPP = 2    /* ST7735 COLMOD 0x06, RGB666, 3 bytes/px   -> 61 fps */
};

/*
 * A note on 18 bpp. Each pixel is three bytes and each byte carries its
 * 6-bit component in bits 7:2, bits 1:0 don't-care (ST7735S DS 9.8.4).
 * 128*128*3 = 49152 B, so a frame costs 16.4 ms instead of 10.9 - you
 * are buying one extra bit of red and one of blue for a third of your
 * frame rate. Worth it for smooth gradients, pointless for sprite work.
 * Unlike 12 bpp it does not pass through the RGBSET conversion table:
 * the datasheet only defines 4k->262k and 65k->262k tables, so 18-bit
 * data reaches the frame memory directly.
 */

/* SPI baud divider (HCLK / n). 2 => 24 MHz, 4 => 12 MHz, 8 => 6 MHz.
 * NOTE: the ST7735S datasheet specifies tSCYCW(min) = 66 ns, i.e. 15 MHz.
 * 24 MHz is out of spec and only works on short flex. gfx_spiSweep() in
 * the benchmark exists to find out whether YOUR panel tolerates it. */
enum : uint8_t { GFX_DIV2 = 2, GFX_DIV4 = 4, GFX_DIV8 = 8, GFX_DIV16 = 16 };

/* ------------------------------------------------------------------ */
/* Framebuffer + palette                                               */
/* ------------------------------------------------------------------ */
/* 4 bpp, two pixels per byte. Even x in the LOW nibble, odd x in the
 * HIGH nibble - that ordering makes the expansion LUT a single uint32
 * store with no shuffling on a little-endian core. */
extern uint8_t  gfx_fb[GFX_FB_BYTES];
extern uint16_t gfx_pal[16];

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
void gfx_begin(uint8_t spiDiv = GFX_DIV2, uint8_t colorMode = GFX_16BPP);
void gfx_setSpiDiv(uint8_t div);
void gfx_setColorMode(uint8_t mode);
uint8_t gfx_colorMode(void);
uint8_t gfx_spiDiv(void);
uint32_t gfx_spiHz(void);

/* MADCTL / window offsets differ between 1.44" panel batches. Defaults
 * match Adafruit's INITR_144GREENTAB (madctl 0xC8, colstart 2, rowstart 3),
 * which is what your Adafruit_ST7735 setup is already using. */
void gfx_setPanelOffsets(uint8_t madctl, uint8_t colStart, uint8_t rowStart);
void gfx_setInverted(bool on);

/* Panel-side frame rate (ST7735 FRMCTR1, 0xB1). Lower values scan the
 * glass faster, which cuts the latency between a GRAM write and the pixel
 * actually changing. Defaults to the fast setting. */
void gfx_setPanelFrameRate(uint8_t rtna, uint8_t fpa, uint8_t bpa);

/* Palette. Rebuilds the expansion LUT, so call it outside the hot loop. */
void gfx_setPalette(const uint16_t *rgb565, uint8_t count);
void gfx_setPaletteEntry(uint8_t index, uint16_t rgb565);
static inline uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ------------------------------------------------------------------ */
/* Presenting the framebuffer                                          */
/* ------------------------------------------------------------------ */
void gfx_flush(void);       /* convert + DMA the whole frame, blocking  */
void gfx_flushAsync(void);  /* same, but returns after the first chunk  */
bool gfx_busy(void);
void gfx_wait(void);

/* ------------------------------------------------------------------ */
/* Direct streaming - bypass the framebuffer entirely                  */
/* ------------------------------------------------------------------ */
/*
 * The 4 bpp framebuffer exists because 128*128 RGB565 will not fit in
 * 20 KB. But a procedural effect - plasma, tunnel, rotozoomer, fire -
 * does not need a framebuffer at all: it can compute pixels straight
 * into the buffer the DMA is about to send. That gets you the panel's
 * FULL colour depth, 65536 or 262144 colours, with no framebuffer and
 * no palette.
 *
 * Your callback fills `rows` rows starting at row `y0`, writing native
 * panel format into `dst`:
 *      GFX_16BPP -> 2 bytes/px, RGB565 little-endian (write uint16_t)
 *      GFX_18BPP -> 3 bytes/px, each component in bits 7:2
 * It is called once per chunk while the previous chunk is still on the
 * wire, so the transfer never stalls as long as you stay inside budget.
 *
 * THE BUDGET, and it is the whole game:
 *      16 bpp -> 32 CPU cycles per pixel
 *      18 bpp -> 48 CPU cycles per pixel
 * Stay under it and you render at full wire speed. Go over and the
 * frame rate degrades in proportion - nothing breaks, it just slows.
 * Put your inner loop in SRAM; flash costs 3 wait states here and
 * measured 2.2x slower on exactly this kind of loop.
 *
 * Requires GFX_16BPP or GFX_18BPP. In 12 bpp the packing is 1.5 bytes
 * per pixel with pixels straddling bytes, which is no use to a
 * per-pixel generator, so gfx_stream() switches to 16 bpp for you.
 */
typedef void (*gfx_streamFn)(uint8_t *dst, int y0, int rows, void *user);
void gfx_stream(gfx_streamFn fn, void *user);

/* Bytes per pixel in the current mode: 2, or 3 for GFX_18BPP. */
uint8_t gfx_bytesPerPixel(void);

/* Pack helpers for stream callbacks. */
static inline uint8_t *gfx_px565(uint8_t *p, uint16_t rgb565) {
    *(uint16_t *)p = rgb565;
    return p + 2;
}
static inline uint8_t *gfx_px666(uint8_t *p, uint8_t r6, uint8_t g6, uint8_t b6) {
    p[0] = (uint8_t)(r6 << 2);
    p[1] = (uint8_t)(g6 << 2);
    p[2] = (uint8_t)(b6 << 2);
    return p + 3;
}

/* Partial update. The wire is the bottleneck, so sending a quarter of
 * the screen costs a quarter of the time - this is the single biggest
 * lever left once DMA is in place. Declare what your frame actually
 * changed and a mostly-static scene runs several times faster.
 *
 * x/w are rounded OUTWARD to a multiple of 2 (16 bpp) or 8 (12 bpp),
 * because the 4 bpp source has to start and end on a byte. Rounding out
 * is always safe - you just send a few more pixels than strictly needed. */
void gfx_flushRect(int x, int y, int w, int h);
void gfx_flushRectAsync(int x, int y, int w, int h);

/* ------------------------------------------------------------------ */
/* Framebuffer drawing (all clipped, all operate on the 4 bpp buffer)  */
/* ------------------------------------------------------------------ */
void gfx_clear(uint8_t c);
void gfx_pixel(int x, int y, uint8_t c);
uint8_t gfx_getPixel(int x, int y);
void gfx_hline(int x, int y, int w, uint8_t c);
void gfx_vline(int x, int y, int h, uint8_t c);
void gfx_fillRect(int x, int y, int w, int h, uint8_t c);
void gfx_rect(int x, int y, int w, int h, uint8_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t c);
void gfx_circle(int cx, int cy, int r, uint8_t c);
void gfx_fillCircle(int cx, int cy, int r, uint8_t c);

/* 4 bpp sprite blit. Source rows are packed the same way as the
 * framebuffer (2 px/byte, even x low nibble), each row padded to a whole
 * byte. transparent = colour index skipped, or -1 for an opaque copy
 * (the opaque even-x case runs at memcpy speed). */
void gfx_blit(const uint8_t *spr, int x, int y, int w, int h, int transparent);

/* Built-in 5x7 font, 1 byte per column, ASCII 32..126. */
void gfx_char(int x, int y, char ch, uint8_t c);
void gfx_text(int x, int y, const char *s, uint8_t c);
void gfx_textScaled(int x, int y, const char *s, uint8_t c, uint8_t scale);

/* ------------------------------------------------------------------ */
/* Direct-to-panel paths (bypass the framebuffer entirely)             */
/* ------------------------------------------------------------------ */
/* Chip select. Assert once, stream a whole frame, deassert - that is the
 * entire performance thesis of this driver in two functions. */
void gfx_select(void);
void gfx_deselect(void);

void gfx_setWindow(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void gfx_cmd(uint8_t c);
void gfx_data8(uint8_t d);

/* Programs the ST7735 RGBSET (2Dh) colour-depth conversion LUT for the
 * current mode. Done automatically for 12 bpp because the datasheet says
 * the table powers up "Random"; call gfx_setWriteColorLut(false) before
 * gfx_setColorMode() if you would rather trust the factory contents. */
void gfx_writeColorLut(void);
void gfx_setWriteColorLut(bool on);

/* Fills a rectangle using DMA with memory-increment DISABLED: the DMA
 * engine re-reads one halfword N times. Zero RAM, zero CPU, full wire
 * speed. 16 bpp only - a 12 bpp solid colour has a 3-byte repeat that
 * does not fit in a single halfword. */
void gfx_directFillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565);

/* Raw DMA of a caller-owned buffer already in panel format. */
void gfx_directBlit(const void *data, uint32_t bytes, bool halfword);

/* Blocking byte-at-a-time write, for baseline comparisons. */
void gfx_blockingWrite(const uint8_t *data, uint32_t bytes);

/* ------------------------------------------------------------------ */
/* Internals exposed for the benchmark                                 */
/* ------------------------------------------------------------------ */
/* Convert `rows` framebuffer rows starting at `row` into `dst`; returns
 * bytes written. _ram lives in SRAM, _flash lives in flash. Benchmarking
 * both is how you measure the 3-wait-state penalty. */
uint32_t gfx_convertRows_ram(uint8_t *dst, uint16_t row, uint16_t rows);
uint32_t gfx_convertRows_flash(uint8_t *dst, uint16_t row, uint16_t rows);
uint32_t gfx_convertSpan_ram(uint8_t *dst, const uint8_t *src, uint32_t srcBytes);

/* A scratch buffer the benchmark can borrow (2 * GFX_CHUNK_BYTES). */
uint8_t *gfx_chunkScratch(void);

/* Bytes the panel receives for a full frame in the current mode. */
uint32_t gfx_frameBytes(void);

/* ------------------------------------------------------------------ */
/* Palette helper                                                      */
/* ------------------------------------------------------------------ */
/* Nearest palette index for a full RGB565 colour. Linear search over 16
 * entries - fine at setup time to build named constants, far too slow to
 * call per pixel. */
uint8_t gfx_nearest(uint16_t rgb565);

/* ------------------------------------------------------------------ */
/* Idiomatic Arduino wrapper                                           */
/* ------------------------------------------------------------------ */
/*
 * Everything below is a zero-cost inline forwarder to the gfx_* calls.
 * There is exactly one panel, one SPI peripheral and one DMA channel, so
 * the driver state is inherently global - the class exists for the
 * familiar `Gfx.clear(0)` spelling, not to allow two instances.
 * Use whichever style you prefer; they are the same code.
 */
class CHGfx {
public:
    void begin(uint8_t spiDiv = GFX_DIV2, uint8_t colorMode = GFX_16BPP) { gfx_begin(spiDiv, colorMode); }

    /* Configuration */
    void setSpiDiv(uint8_t d)                        { gfx_setSpiDiv(d); }
    void setColorMode(uint8_t m)                     { gfx_setColorMode(m); }
    uint8_t colorMode() const                        { return gfx_colorMode(); }
    uint32_t spiHz() const                           { return gfx_spiHz(); }
    void setPanelOffsets(uint8_t m, uint8_t cs, uint8_t rs) { gfx_setPanelOffsets(m, cs, rs); }
    void setInverted(bool on)                        { gfx_setInverted(on); }
    void setPanelFrameRate(uint8_t r, uint8_t f, uint8_t b) { gfx_setPanelFrameRate(r, f, b); }

    /* Palette */
    void setPalette(const uint16_t *p, uint8_t n)    { gfx_setPalette(p, n); }
    void setPaletteEntry(uint8_t i, uint16_t c)      { gfx_setPaletteEntry(i, c); }
    uint8_t nearest(uint16_t rgb565) const           { return gfx_nearest(rgb565); }
    static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return gfx_rgb(r, g, b); }

    /* Present */
    void display()                                   { gfx_flush(); }
    void stream(gfx_streamFn fn, void *user = nullptr) { gfx_stream(fn, user); }
    void displayAsync()                              { gfx_flushAsync(); }
    void displayRect(int x, int y, int w, int h)     { gfx_flushRect(x, y, w, h); }
    void displayRectAsync(int x, int y, int w, int h){ gfx_flushRectAsync(x, y, w, h); }
    bool busy() const                                { return gfx_busy(); }
    void wait()                                      { gfx_wait(); }

    /* Draw */
    void clear(uint8_t c)                                  { gfx_clear(c); }
    void drawPixel(int x, int y, uint8_t c)                { gfx_pixel(x, y, c); }
    uint8_t getPixel(int x, int y) const                   { return gfx_getPixel(x, y); }
    void drawFastHLine(int x, int y, int w, uint8_t c)     { gfx_hline(x, y, w, c); }
    void drawFastVLine(int x, int y, int h, uint8_t c)     { gfx_vline(x, y, h, c); }
    void fillRect(int x, int y, int w, int h, uint8_t c)   { gfx_fillRect(x, y, w, h, c); }
    void drawRect(int x, int y, int w, int h, uint8_t c)   { gfx_rect(x, y, w, h, c); }
    void drawLine(int x0, int y0, int x1, int y1, uint8_t c) { gfx_line(x0, y0, x1, y1, c); }
    void drawCircle(int cx, int cy, int r, uint8_t c)      { gfx_circle(cx, cy, r, c); }
    void fillCircle(int cx, int cy, int r, uint8_t c)      { gfx_fillCircle(cx, cy, r, c); }
    void drawSprite(const uint8_t *s, int x, int y, int w, int h, int transparent = -1)
                                                           { gfx_blit(s, x, y, w, h, transparent); }
    void drawChar(int x, int y, char ch, uint8_t c)        { gfx_char(x, y, ch, c); }
    void print(int x, int y, const char *s, uint8_t c)     { gfx_text(x, y, s, c); }
    void print(int x, int y, const char *s, uint8_t c, uint8_t scale)
                                                           { gfx_textScaled(x, y, s, c, scale); }

    /* Direct-to-panel, bypassing the framebuffer */
    void fillRectDirect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565)
                                                           { gfx_directFillRect(x, y, w, h, rgb565); }

    /* Raw framebuffer, if you want to write your own primitives */
    uint8_t *buffer() const                                { return gfx_fb; }
    static constexpr int width()  { return GFX_W; }
    static constexpr int height() { return GFX_H; }
};

/* The one instance. Declared here, defined in CHGfx.cpp. */
extern CHGfx Gfx;
