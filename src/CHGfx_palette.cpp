/*
 * CHGfx_palette.cpp - the 16-colour palette and the fade applied to it.
 *
 * Portable on purpose: no SPI, no DMA, so the PC simulator compiles this
 * file as-is and shows exactly the colours the panel would.
 *
 * STAGING. A palette change used to rebuild the 256-entry expansion LUT
 * on the spot. The DMA interrupt reads that LUT for every chunk of an
 * async flush, so a change made while a frame was still going out gave
 * that frame old colours at the top and new ones at the bottom. Now a
 * change only marks the palette dirty, and the LUT is rebuilt when the
 * next flush starts, after the previous one has finished. Palette calls
 * are safe at any time, and every frame shows exactly one palette.
 */
#include "CHGfx_internal.h"

uint16_t gfx_pal[16];

static bool     s_dirty = true;
static uint8_t  s_fadeAmount = 0;      /* 0 = none, 255 = all target */
static uint16_t s_fadeTarget = 0x0000;

void gfx_setPalette(const uint16_t *rgb565, uint8_t count) {
    if (rgb565 != gfx_pal)
        for (uint8_t i = 0; i < count && i < 16; i++) gfx_pal[i] = rgb565[i];
    s_dirty = true;
}

void gfx_setPaletteEntry(uint8_t index, uint16_t rgb565) {
    gfx_pal[index & 0x0F] = rgb565;
    s_dirty = true;
}

void gfx_setFade(uint8_t amount, uint16_t rgb565) {
    if (amount == s_fadeAmount && rgb565 == s_fadeTarget) return;
    s_fadeAmount = amount;
    s_fadeTarget = rgb565;
    s_dirty = true;
}

uint8_t gfx_fade(void) { return s_fadeAmount; }

/* Blend one RGB565 colour toward the fade target, per component, with
 * rounding: amount 255 lands exactly on the target. */
static uint16_t fadeOne(uint16_t c) {
    uint32_t a = s_fadeAmount;
    if (!a) return c;
    uint16_t t = s_fadeTarget;
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    int tr = (t >> 11) & 0x1F, tg = (t >> 5) & 0x3F, tb = t & 0x1F;
    r += ((tr - r) * (int)a + (tr >= r ? 127 : -127)) / 255;
    g += ((tg - g) * (int)a + (tg >= g ? 127 : -127)) / 255;
    b += ((tb - b) * (int)a + (tb >= b ? 127 : -127)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t gfx_paletteOut(uint8_t index) { return fadeOne(gfx_pal[index & 0x0F]); }

bool gfx__paletteTake(uint16_t out[16]) {
    for (uint8_t i = 0; i < 16; i++) out[i] = fadeOne(gfx_pal[i]);
    bool was = s_dirty;
    s_dirty = false;
    return was;
}

void gfx__paletteTouch(void) { s_dirty = true; }

/* Nearest palette entry, by squared distance in RGB565's own component
 * space. Weighted 2:4:1 after the usual luminance rule of thumb, scaled
 * so the 5/6/5 bit widths do not skew the result toward green. Uses the
 * palette as set, not as faded. */
uint8_t gfx_nearest(uint16_t rgb565) {
    int r = (rgb565 >> 11) & 0x1F;
    int g = (rgb565 >> 5)  & 0x3F;
    int b =  rgb565        & 0x1F;
    uint8_t best = 0;
    int32_t bestD = 0x7FFFFFFF;
    for (uint8_t i = 0; i < 16; i++) {
        int dr = r - ((gfx_pal[i] >> 11) & 0x1F);
        int dg = g - ((gfx_pal[i] >> 5)  & 0x3F);
        int db = b - ( gfx_pal[i]        & 0x1F);
        int32_t d = 8 * dr * dr + 4 * dg * dg + 4 * db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

/* sin(i * pi/128) in Q14, i = 0..64: a quarter wave, 130 bytes. */
static const int16_t s_sinQ[65] = {
        0,   402,   804,  1205,  1606,  2006,  2404,  2801,  3196,  3590,
     3981,  4370,  4756,  5139,  5520,  5897,  6270,  6639,  7005,  7366,
     7723,  8076,  8423,  8765,  9102,  9434,  9760, 10080, 10394, 10702,
    11003, 11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395,
    13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286,
    15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261,
    16305, 16340, 16364, 16379, 16384
};

int gfx__sin14(uint8_t a) {
    uint8_t q = a & 63;
    int v = (a & 64) ? s_sinQ[64 - q] : s_sinQ[q];
    return (a & 128) ? -v : v;
}
