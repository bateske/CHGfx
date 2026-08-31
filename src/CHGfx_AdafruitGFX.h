/*
 * CHGfx_AdafruitGFX.h - drop CHGfx in underneath Adafruit_GFX.
 *
 * WHY YOU MIGHT WANT THIS
 * -----------------------
 * Adafruit_GFX itself is not the slow part. The slow part is
 * Adafruit_ST7735 underneath it, pushing every pixel down a polled SPI
 * link with a fresh address window each time. Adafruit_GFX's own
 * feature set - print(), setTextSize(), the custom GFXfont system,
 * drawBitmap(), triangles, rounded rectangles - is perfectly good.
 *
 * So: subclass Adafruit_GFX, point drawPixel() at the 4 bpp
 * framebuffer, override the span primitives with CHGfx's fast paths,
 * and present with display(). Existing sketches keep working and get
 * the full DMA transport underneath.
 *
 *     #include <Adafruit_GFX.h>      // see the note below - required
 *     #include <CHGfx.h>
 *     #include <CHGfx_AdafruitGFX.h>
 *     CHGfx_GFX tft;
 *
 *     void setup() {
 *         tft.begin();                       // same as Gfx.begin()
 *         tft.setPalette(myPalette, 16);
 *         tft.setTextColor(WHITE_IDX);
 *         tft.setCursor(4, 4);
 *         tft.print("hello");
 *         tft.display();                     // one DMA burst
 *     }
 *
 * THE ONE THING THAT CHANGES
 * --------------------------
 * Adafruit_GFX passes colours as uint16_t. Here a colour is a PALETTE
 * INDEX, 0..15 - there is no 16-colour framebuffer that can hold an
 * arbitrary RGB565. Passing ST77XX_RED (0xF800) would be read as index
 * 0, i.e. black, so map your colours once at setup:
 *
 *     const uint8_t RED = Gfx.nearest(0xF800);
 *
 * Set CHGFX_GFX_AUTOMAP if you would rather have colours mapped for you
 * at every call. It makes old code work verbatim but costs a 16-entry
 * search per drawing call, so it is off by default.
 *
 * REQUIRES the Adafruit GFX Library to be installed. This header is not
 * included by CHGfx.h - include it yourself only if you want the bridge.
 *
 * You must also write `#include <Adafruit_GFX.h>` in your .ino, above
 * this include. That looks redundant, but the Arduino builder works out
 * which library include paths to add by scanning the SKETCH file only.
 * A library reached solely through another library's header never gets
 * its path added, and the include below would fail to resolve.
 */
#pragma once

#include "CHGfx.h"

#if !__has_include(<Adafruit_GFX.h>)
  #error "Adafruit_GFX.h not found. Install the Adafruit GFX Library, AND add \
'#include <Adafruit_GFX.h>' to your .ino ABOVE this include - the Arduino \
builder only resolves libraries that the sketch itself names."
#endif

#include <Adafruit_GFX.h>

class CHGfx_GFX : public Adafruit_GFX {
public:
    CHGfx_GFX() : Adafruit_GFX(GFX_W, GFX_H) { }

    void begin(uint8_t spiDiv = GFX_DIV2, uint8_t colorMode = GFX_16BPP) {
        gfx_begin(spiDiv, colorMode);
    }

    /* ---- the one method Adafruit_GFX actually requires ---- */
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        gfx_pixel(x, y, idx(color));
    }

    /* ---- overrides that matter for speed --------------------------
     * Adafruit_GFX's defaults build every one of these out of
     * drawPixel(). Routing them at the framebuffer instead is where the
     * CPU-side win comes from: a filled span is word stores, 8 pixels
     * at a time, rather than a call and a clip test per pixel. */
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        gfx_hline(x, y, w, idx(color));
    }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        gfx_vline(x, y, h, idx(color));
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        gfx_fillRect(x, y, w, h, idx(color));
    }
    void fillScreen(uint16_t color) override {
        gfx_clear(idx(color));
    }
    void writePixel(int16_t x, int16_t y, uint16_t color) override {
        gfx_pixel(x, y, idx(color));
    }
    void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        gfx_fillRect(x, y, w, h, idx(color));
    }
    void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        gfx_hline(x, y, w, idx(color));
    }
    void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        gfx_vline(x, y, h, idx(color));
    }
    /* Nothing to batch - we are writing to RAM, not to the panel. */
    void startWrite(void) override { }
    void endWrite(void)   override { }

    /* ---- presenting ----
     * Adafruit_ST7735 has no display() because it writes straight to the
     * glass. Here nothing reaches the panel until you ask. */
    void display()                                    { gfx_flush(); }
    void displayAsync()                               { gfx_flushAsync(); }
    void displayRect(int x, int y, int w, int h)      { gfx_flushRect(x, y, w, h); }
    void displayRectAsync(int x, int y, int w, int h) { gfx_flushRectAsync(x, y, w, h); }
    bool busy() const                                 { return gfx_busy(); }
    void wait()                                       { gfx_wait(); }

    /* ---- palette passthrough ---- */
    void setPalette(const uint16_t *p, uint8_t n) { gfx_setPalette(p, n); }
    void setPaletteEntry(uint8_t i, uint16_t c)   { gfx_setPaletteEntry(i, c); }
    uint8_t nearest(uint16_t rgb565) const        { return gfx_nearest(rgb565); }
    void setColorMode(uint8_t m)                  { gfx_setColorMode(m); }

private:
    static inline uint8_t idx(uint16_t color) {
#ifdef CHGFX_GFX_AUTOMAP
        /* Convenience mode: treat the argument as a real RGB565 and find
         * the closest palette slot. Correct for legacy code, but it is a
         * 16-entry search on every drawing call. */
        return gfx_nearest(color);
#else
        return (uint8_t)(color & 0x0F);
#endif
    }
};
