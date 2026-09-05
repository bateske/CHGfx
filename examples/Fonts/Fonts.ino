/*
 * Fonts - custom bitmap fonts on CHGfx.
 *
 * The built-in 5x7 needs no setup and costs 475 bytes. When you want
 * something bigger or proportional, gfx_setFont()/Gfx.setFont() takes a
 * GFXfont - the Adafruit_GFX font format, unchanged - so the fonts in
 * src/fonts/ and every font in Adafruit_GFX's Fonts/ directory both work.
 *
 * THE ONE GOTCHA
 * --------------
 * The built-in font positions text by its TOP edge. A custom font
 * positions it by its BASELINE, so ascenders go above the y you pass and
 * descenders hang below it. That is Adafruit's convention and this
 * library follows it. Gfx.fontBaseline() is the conversion: it returns 0
 * for the built-in font and the ascent for a custom one, so
 *
 *     Gfx.print(x, y + Gfx.fontBaseline(), s, c);
 *
 * always means "top of the text at y", whichever font is selected.
 *
 * FLASH
 * -----
 * The CH32X035G8U6 has 62 KB. A 95-glyph font here costs 1.1-1.8 KB;
 * the digits-only one costs 0.5 KB. Only fonts you #include are linked.
 */
#include <CHGfx.h>
#include <fonts/CHGfx_Mono11.h>
#include <fonts/CHGfx_Sans12.h>
#include <fonts/CHGfx_SansBold12.h>
#include <fonts/CHGfx_SansBold16.h>
#include <fonts/CHGfx_Digits24.h>

/* Palette indices - a colour here is a 0..15 slot, not an RGB565. */
enum : uint8_t { BLACK = 0, WHITE = 1, GREY = 2, RED = 3, YELLOW = 4, CYAN = 5 };

static const uint16_t palette[16] = {
    gfx_rgb(0, 0, 0),        /* 0 black  */
    gfx_rgb(255, 255, 255),  /* 1 white  */
    gfx_rgb(110, 110, 120),  /* 2 grey   */
    gfx_rgb(230, 60, 60),    /* 3 red    */
    gfx_rgb(250, 210, 60),   /* 4 yellow */
    gfx_rgb(80, 220, 230),   /* 5 cyan   */
};

/* Right-align a string against x, using the current font's metrics. */
static void printRight(int x, int y, const char *s, uint8_t c) {
    Gfx.print(x - Gfx.textWidth(s), y, s, c);
}

/* Centre a string on the screen, top edge at y, whatever font is set. */
static void printCentred(int y, const char *s, uint8_t c) {
    int w = Gfx.textWidth(s);
    Gfx.print((GFX_W - w) / 2, y + Gfx.fontBaseline(), s, c);
}

void setup() {
    Gfx.begin();
    Gfx.setPalette(palette, 16);
}

/* ---- page 1: the fonts, one per line, top-aligned ---- */
static void pageSpecimens() {
    Gfx.clear(BLACK);

    int y = 2;

    Gfx.setFont();                       /* back to the built-in 5x7 */
    Gfx.print(2, y, "built-in 5x7", GREY);
    y += Gfx.fontLineHeight() + 2;

    Gfx.setFont(&CHGfx_Mono11);
    Gfx.print(2, y + Gfx.fontBaseline(), "Mono11 0O1lI", WHITE);
    y += Gfx.fontLineHeight();

    Gfx.setFont(&CHGfx_Sans12);
    Gfx.print(2, y + Gfx.fontBaseline(), "Sans12 jgpqy", WHITE);
    y += Gfx.fontLineHeight();

    Gfx.setFont(&CHGfx_SansBold12);
    Gfx.print(2, y + Gfx.fontBaseline(), "SansBold12", YELLOW);
    y += Gfx.fontLineHeight();

    Gfx.setFont(&CHGfx_SansBold16);
    Gfx.print(2, y + Gfx.fontBaseline(), "Bold16", CYAN);
    y += Gfx.fontLineHeight();

    /* Scaling still works, and on a paletted framebuffer it is cheap -
     * each lit pixel becomes one fillRect, which is word stores. */
    Gfx.setFont(&CHGfx_Sans12);
    Gfx.print(2, y + Gfx.fontBaseline() * 2, "x2", RED, 2);

    Gfx.display();
}

/* ---- page 2: a HUD, which is what the metrics are for ---- */
static void pageHud(uint16_t score, uint8_t lives) {
    Gfx.clear(BLACK);

    Gfx.setFont(&CHGfx_SansBold12);
    printCentred(4, "HIGH SCORE", GREY);

    /* Digits24 carries '+' through ':' only - 501 bytes for a display
     * font, because you never needed the letters. Anything outside that
     * range, a space included, is simply skipped. */
    char buf[12];
    snprintf(buf, sizeof buf, "%05u", score);
    Gfx.setFont(&CHGfx_Digits24);
    printCentred(22, buf, YELLOW);

    /* Right-aligned against the screen edge using textWidth(). */
    Gfx.setFont(&CHGfx_Mono11);
    snprintf(buf, sizeof buf, "x%u", lives);
    printRight(GFX_W - 3, 60 + Gfx.fontBaseline(), buf, RED);

    /* Multi-line: '\n' returns to the x you started at. */
    Gfx.setFont(&CHGfx_Sans12);
    Gfx.print(3, 78 + Gfx.fontBaseline(), "wave 3\nspeed 1.5x\nshield up", CYAN);

    /* textBounds() gives the ink box - here, a frame around the last
     * line with two pixels of air on every side. */
    int bx, by, bw, bh;
    Gfx.textBounds("shield up", 3, 78 + Gfx.fontBaseline() + 2 * Gfx.fontLineHeight(),
                   1, &bx, &by, &bw, &bh);
    Gfx.drawRect(bx - 2, by - 2, bw + 4, bh + 4, GREY);

    Gfx.display();
}

void loop() {
    pageSpecimens();
    delay(3000);

    for (uint16_t s = 0; s < 4; s++) {
        pageHud((uint16_t)(1234 + s * 1111), 3);
        delay(700);
    }
}
