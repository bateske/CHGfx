/*
 * chgfx_draw.cpp - primitives over the 4 bpp framebuffer.
 *
 * Everything here works in nibbles: two pixels per byte, even x in the
 * low nibble. The fast paths all try to reach 32-bit stores, because a
 * word store paints EIGHT pixels. That is the whole reason a paletted
 * buffer beats a full-colour one on a part this small - fill rate scales
 * with bits, not pixels.
 */
#include "CHGfx.h"
#include "CHGfx_font.h"

#define RAMFUNC __attribute__((section(".srodata.ramfunc"), noinline))

static inline uint8_t *rowPtr(int y) { return gfx_fb + y * GFX_FB_STRIDE; }

/* ------------------------------------------------------------------ */
/* Pixels                                                              */
/* ------------------------------------------------------------------ */
void gfx_pixel(int x, int y, uint8_t c) {
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
    uint8_t *p = rowPtr(y) + (x >> 1);
    if (x & 1) *p = (uint8_t)((*p & 0x0F) | (c << 4));
    else       *p = (uint8_t)((*p & 0xF0) | (c & 0x0F));
}

uint8_t gfx_getPixel(int x, int y) {
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return 0;
    uint8_t b = rowPtr(y)[x >> 1];
    return (x & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
}

/* ------------------------------------------------------------------ */
/* Clear - one word store paints 8 pixels                              */
/* ------------------------------------------------------------------ */
RAMFUNC void gfx_clear(uint8_t c) {
    uint32_t v = (uint32_t)(c & 0x0F);
    v |= v << 4; v |= v << 8; v |= v << 16;
    uint32_t *d = (uint32_t *)gfx_fb;
    for (uint32_t i = 0; i < GFX_FB_BYTES / 4; i += 8) {
        d[i+0] = v; d[i+1] = v; d[i+2] = v; d[i+3] = v;
        d[i+4] = v; d[i+5] = v; d[i+6] = v; d[i+7] = v;
    }
}

/* ------------------------------------------------------------------ */
/* Horizontal span: ragged nibble ends, word-store middle              */
/* ------------------------------------------------------------------ */
RAMFUNC void gfx_hline(int x, int y, int w, uint8_t c) {
    if ((unsigned)y >= GFX_H || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (w <= 0) return;

    uint8_t *p = rowPtr(y) + (x >> 1);
    c &= 0x0F;

    /* Odd left edge: patch the high nibble of the first byte. */
    if (x & 1) { *p = (uint8_t)((*p & 0x0F) | (c << 4)); p++; w--; }

    uint8_t  pair = (uint8_t)(c | (c << 4));
    uint32_t quad = (uint32_t)pair; quad |= quad << 8; quad |= quad << 16;

    /* Align to a word boundary a byte at a time. */
    while (w >= 2 && ((uintptr_t)p & 3)) { *p++ = pair; w -= 2; }
    while (w >= 8) { *(uint32_t *)p = quad; p += 4; w -= 8; }
    while (w >= 2) { *p++ = pair; w -= 2; }

    /* Odd right edge: patch the low nibble of the last byte. */
    if (w) *p = (uint8_t)((*p & 0xF0) | c);
}

void gfx_vline(int x, int y, int h, uint8_t c) {
    if ((unsigned)x >= GFX_W || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > GFX_H) h = GFX_H - y;
    if (h <= 0) return;

    uint8_t *p = rowPtr(y) + (x >> 1);
    c &= 0x0F;
    if (x & 1) {
        uint8_t hi = (uint8_t)(c << 4);
        while (h--) { *p = (uint8_t)((*p & 0x0F) | hi); p += GFX_FB_STRIDE; }
    } else {
        while (h--) { *p = (uint8_t)((*p & 0xF0) | c); p += GFX_FB_STRIDE; }
    }
}

void gfx_fillRect(int x, int y, int w, int h, uint8_t c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > GFX_H) h = GFX_H - y;
    while (h-- > 0) gfx_hline(x, y++, w, c);
}

void gfx_rect(int x, int y, int w, int h, uint8_t c) {
    if (w <= 0 || h <= 0) return;
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

/* ------------------------------------------------------------------ */
/* Bresenham                                                           */
/* ------------------------------------------------------------------ */
void gfx_line(int x0, int y0, int x1, int y1, uint8_t c) {
    if (y0 == y1) { gfx_hline(x0 < x1 ? x0 : x1, y0, (x1 > x0 ? x1 - x0 : x0 - x1) + 1, c); return; }
    if (x0 == x1) { gfx_vline(x0, y0 < y1 ? y0 : y1, (y1 > y0 ? y1 - y0 : y0 - y1) + 1, c); return; }

    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(int cx, int cy, int r, uint8_t c) {
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        gfx_pixel(cx + x, cy + y, c); gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx + x, cy - y, c); gfx_pixel(cx - x, cy - y, c);
        gfx_pixel(cx + y, cy + x, c); gfx_pixel(cx - y, cy + x, c);
        gfx_pixel(cx + y, cy - x, c); gfx_pixel(cx - y, cy - x, c);
        if (d < 0) d += 4 * x + 6;
        else       d += 4 * (x - y--) + 10;
        x++;
    }
}

void gfx_fillCircle(int cx, int cy, int r, uint8_t c) {
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        gfx_hline(cx - x, cy + y, 2 * x + 1, c);
        gfx_hline(cx - x, cy - y, 2 * x + 1, c);
        gfx_hline(cx - y, cy + x, 2 * y + 1, c);
        gfx_hline(cx - y, cy - x, 2 * y + 1, c);
        if (d < 0) d += 4 * x + 6;
        else       d += 4 * (x - y--) + 10;
        x++;
    }
}

/* ------------------------------------------------------------------ */
/* Sprite blit                                                         */
/* ------------------------------------------------------------------ */
/*
 * Three cases, fastest first:
 *   opaque + even destination x + even width -> byte copy (2 px/byte)
 *   opaque + odd  destination x              -> nibble-shifted copy
 *   transparent                              -> per-pixel test
 * Sprites stored with even width and blitted to even x are ~8x faster
 * than the transparent path, which is worth designing your art around.
 */
RAMFUNC void gfx_blit(const uint8_t *spr, int x, int y, int w, int h, int transparent)
{
    int srcStride = (w + 1) >> 1;

    int sy0 = 0;
    if (y < 0) { sy0 = -y; h += y; y = 0; }
    if (y + h > GFX_H) h = GFX_H - y;
    if (h <= 0) return;

    int sx0 = 0;
    if (x < 0) { sx0 = -x; w += x; x = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (w <= 0) return;

    const uint8_t *s = spr + sy0 * srcStride;
    uint8_t *dRow = rowPtr(y);

    if (transparent >= 0 && ((x & 1) == 0) && ((sx0 & 1) == 0)) {
        /*
         * Aligned transparent blit, two pixels per iteration.
         *
         * The per-pixel version below costs ~24 cycles/px: extract a
         * nibble, compare, read-modify-write the destination nibble.
         * Here one source byte carries both pixels, so we build a
         * branchless 8-bit keep-mask and do a single store.
         *
         *   x    = src ^ tcPair, so a zero nibble means "transparent"
         *   mask = 0x0F / 0xF0 per nibble, set only where x is non-zero
         *          ((n + 15) >> 4) is 0 for n == 0 and 1 for n >= 1
         *
         * Plus an early-out for the fully-transparent byte, which also
         * skips the destination load - that is most of a typical sprite.
         */
        uint8_t tcp = (uint8_t)(transparent & 0x0F);
        tcp = (uint8_t)(tcp | (tcp << 4));
        int wholeBytes = w >> 1;
        for (int r = 0; r < h; r++) {
            const uint8_t *sp = s + (sx0 >> 1);
            uint8_t *dp = dRow + (x >> 1);
            for (int i = 0; i < wholeBytes; i++) {
                uint32_t sb = sp[i];
                if (sb == tcp) continue;
                uint32_t df = sb ^ tcp;
                uint32_t m = ((((df & 0x0Fu) + 0x0Fu) >> 4) * 0x0Fu)
                           | ((((df & 0xF0u) + 0xF0u) >> 8) * 0xF0u);
                dp[i] = (uint8_t)((dp[i] & ~m) | (sb & m));
            }
            if (w & 1) {
                uint8_t v = (uint8_t)(sp[wholeBytes] & 0x0F);
                if (v != (tcp & 0x0F))
                    dp[wholeBytes] = (uint8_t)((dp[wholeBytes] & 0xF0) | v);
            }
            s += srcStride;
            dRow += GFX_FB_STRIDE;
        }
        return;
    }

    if (transparent < 0 && ((x & 1) == 0) && ((sx0 & 1) == 0)) {
        /* Aligned opaque copy. */
        int wholeBytes = w >> 1;
        for (int r = 0; r < h; r++) {
            const uint8_t *sp = s + (sx0 >> 1);
            uint8_t *dp = dRow + (x >> 1);
            for (int i = 0; i < wholeBytes; i++) dp[i] = sp[i];
            if (w & 1) dp[wholeBytes] = (uint8_t)((dp[wholeBytes] & 0xF0) | (sp[wholeBytes] & 0x0F));
            s += srcStride;
            dRow += GFX_FB_STRIDE;
        }
        return;
    }

    uint8_t tc = (uint8_t)(transparent & 0x0F);
    for (int r = 0; r < h; r++) {
        const uint8_t *sp = s;
        uint8_t *dp = dRow;
        for (int i = 0; i < w; i++) {
            int sxp = sx0 + i;
            uint8_t b = sp[sxp >> 1];
            uint8_t v = (sxp & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
            if (transparent >= 0 && v == tc) continue;
            int dxp = x + i;
            uint8_t *q = dp + (dxp >> 1);
            if (dxp & 1) *q = (uint8_t)((*q & 0x0F) | (v << 4));
            else         *q = (uint8_t)((*q & 0xF0) | v);
        }
        s += srcStride;
        dRow += GFX_FB_STRIDE;
    }
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */
/*
 * Two font systems live here.
 *
 *   * The built-in 5x7, one byte per COLUMN, bit 0 = top. It is 475
 *     bytes and needs no setup, so it stays the default.
 *   * GFXfont, the Adafruit format: proportional, arbitrary height, a
 *     row-major MSB-first bitstream per glyph. Selected with
 *     gfx_setFont() and used by every text call from then on.
 *
 * ORIGIN, and this is the one thing that trips people up: the built-in
 * font draws with y as the TOP of the glyph box, a GFXfont draws with y
 * as the BASELINE. That is not a CHGfx invention - it is exactly what
 * Adafruit_GFX does, so sketches and font data port over unchanged. If
 * you want to keep thinking in top-left coordinates, add
 * gfx_fontBaseline(), which is 0 for the built-in font and the ascent
 * for a custom one:
 *
 *     gfx_text(x, y + gfx_fontBaseline(), "aligned to y", c);
 */

static const GFXfont *s_font   = nullptr;
static int16_t        s_ascent = 0;   /* rows above the baseline, cached */

void gfx_setFont(const GFXfont *f) {
    s_font = f;
    s_ascent = 0;
    if (!f) return;

    /* The struct records no ascent, so derive it once here rather than
     * per call: the topmost ink of any glyph is the most negative
     * yOffset. 95 glyphs is a few microseconds at setup time. */
    int n = (int)f->last - (int)f->first + 1;
    int minYo = 0;
    for (int i = 0; i < n; i++) {
        if (!f->glyph[i].height) continue;          /* space, blanks */
        if (f->glyph[i].yOffset < minYo) minYo = f->glyph[i].yOffset;
    }
    s_ascent = (int16_t)(-minYo);
}

const GFXfont *gfx_font(void)   { return s_font; }
int gfx_fontLineHeight(void)    { return s_font ? (int)s_font->yAdvance : 8; }
int gfx_fontBaseline(void)      { return s_font ? (int)s_ascent : 0; }

/* Characters outside the font's range fall back to '?', and if the font
 * has no '?' either - a digits-only font, say - they are dropped, which
 * is what Adafruit_GFX does. */
static const GFXglyph *glyphFor(const GFXfont *f, unsigned char ch) {
    if (ch < f->first || ch > f->last) {
        if ('?' < f->first || '?' > f->last) return nullptr;
        ch = '?';
    }
    return &f->glyph[ch - f->first];
}

/*
 * Built-in font, scale 1. The obvious version calls gfx_pixel per lit
 * pixel, which is a flash-resident call plus a full clip test 20-odd
 * times per character - that measured 40 us/char. Here the column
 * pointer and the nibble shift are hoisted out and only the row stride
 * is added per pixel.
 */
RAMFUNC static void charBuiltin(int x, int y, char ch, uint8_t c) {
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = chgfx_font5x7 + (ch - 32) * 5;
    c &= 0x0F;

    int row0 = y, row1 = y + 7;
    if (row0 < 0) row0 = 0;
    if (row1 > GFX_H) row1 = GFX_H;
    if (row0 >= row1) return;

    for (int col = 0; col < 5; col++) {
        int px = x + col;
        if ((unsigned)px >= GFX_W) continue;
        uint8_t bits = g[col];
        if (!bits) continue;

        uint8_t *p = rowPtr(row0) + (px >> 1);
        bits >>= (row0 - y);
        if (px & 1) {
            uint8_t v = (uint8_t)(c << 4);
            for (int r = row0; r < row1 && bits; r++, p += GFX_FB_STRIDE) {
                if (bits & 1) *p = (uint8_t)((*p & 0x0F) | v);
                bits >>= 1;
            }
        } else {
            for (int r = row0; r < row1 && bits; r++, p += GFX_FB_STRIDE) {
                if (bits & 1) *p = (uint8_t)((*p & 0xF0) | c);
                bits >>= 1;
            }
        }
    }
}

static void charBuiltinScaled(int x, int y, char ch, uint8_t c, uint8_t scale) {
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = chgfx_font5x7 + (ch - 32) * 5;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row))
                gfx_fillRect(x + col * scale, y + row * scale, scale, scale, c);
    }
}

/*
 * GFXfont glyph, scale 1.
 *
 * The bitstream is row-major and rows are not byte-aligned, so every bit
 * has to be consumed in order even where the pixel is clipped away -
 * hence the running (cur, nbits) accumulator rather than an index. The
 * one shortcut that matters: an all-zero byte skips up to eight columns
 * in a single step, and in a typical glyph most bytes are mostly zero.
 */
RAMFUNC static void drawGlyph1(const uint8_t *bmp, const GFXglyph *g,
                               int x, int y, uint8_t c)
{
    int gw = g->width, gh = g->height;
    if (!gw || !gh) return;

    const uint8_t *p = bmp + g->bitmapOffset;
    int gx = x + g->xOffset;
    int gy = y + g->yOffset;
    c &= 0x0F;
    uint8_t lo = c, hi = (uint8_t)(c << 4);

    uint32_t cur = 0;
    int nbits = 0;

    for (int r = 0; r < gh; r++) {
        int py = gy + r;
        uint8_t *rp = ((unsigned)py < (unsigned)GFX_H) ? rowPtr(py) : nullptr;
        int col = 0;
        while (col < gw) {
            if (!nbits) { cur = *p++; nbits = 8; }
            if (!cur) {                       /* rest of this byte is blank */
                int take = gw - col;
                if (take > nbits) take = nbits;
                col += take; nbits -= take;
                continue;
            }
            if ((cur & 0x80) && rp) {
                unsigned px = (unsigned)(gx + col);
                if (px < (unsigned)GFX_W) {
                    uint8_t *q = rp + (px >> 1);
                    if (px & 1) *q = (uint8_t)((*q & 0x0F) | hi);
                    else        *q = (uint8_t)((*q & 0xF0) | lo);
                }
            }
            cur = (cur << 1) & 0xFF;
            nbits--; col++;
        }
    }
}

static void drawGlyphScaled(const uint8_t *bmp, const GFXglyph *g,
                            int x, int y, uint8_t c, uint8_t scale)
{
    int gw = g->width, gh = g->height;
    if (!gw || !gh) return;

    const uint8_t *p = bmp + g->bitmapOffset;
    int gx = x + g->xOffset * scale;
    int gy = y + g->yOffset * scale;

    uint32_t cur = 0;
    int nbits = 0;

    for (int r = 0; r < gh; r++) {
        int col = 0;
        while (col < gw) {
            if (!nbits) { cur = *p++; nbits = 8; }
            if (!cur) {
                int take = gw - col;
                if (take > nbits) take = nbits;
                col += take; nbits -= take;
                continue;
            }
            if (cur & 0x80)
                gfx_fillRect(gx + col * scale, gy + r * scale, scale, scale, c);
            cur = (cur << 1) & 0xFF;
            nbits--; col++;
        }
    }
}

void gfx_charScaled(int x, int y, char ch, uint8_t c, uint8_t scale) {
    if (scale < 1) scale = 1;
    const GFXfont *f = s_font;
    if (!f) {
        if (scale == 1) charBuiltin(x, y, ch, c);
        else            charBuiltinScaled(x, y, ch, c, scale);
        return;
    }
    const GFXglyph *g = glyphFor(f, (unsigned char)ch);
    if (!g) return;
    if (scale == 1) drawGlyph1(f->bitmap, g, x, y, c);
    else            drawGlyphScaled(f->bitmap, g, x, y, c, scale);
}

void gfx_char(int x, int y, char ch, uint8_t c) {
    gfx_charScaled(x, y, ch, c, 1);
}

void gfx_textScaled(int x, int y, const char *s, uint8_t c, uint8_t scale) {
    if (scale < 1) scale = 1;
    const GFXfont *f = s_font;
    const int x0 = x;

    if (!f) {
        while (*s) {
            char ch = *s++;
            if (ch == '\n') { x = x0; y += 8 * scale; continue; }
            if (ch == '\r') continue;
            if (scale == 1) charBuiltin(x, y, ch, c);
            else            charBuiltinScaled(x, y, ch, c, scale);
            x += 6 * scale;
        }
        return;
    }

    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch == '\n') { x = x0; y += f->yAdvance * scale; continue; }
        if (ch == '\r') continue;
        const GFXglyph *g = glyphFor(f, ch);
        if (!g) continue;
        if (scale == 1) drawGlyph1(f->bitmap, g, x, y, c);
        else            drawGlyphScaled(f->bitmap, g, x, y, c, scale);
        x += g->xAdvance * scale;
    }
}

void gfx_text(int x, int y, const char *s, uint8_t c) {
    gfx_textScaled(x, y, s, c, 1);
}

/* ------------------------------------------------------------------ */
/* Text metrics                                                        */
/* ------------------------------------------------------------------ */
int gfx_textWidthScaled(const char *s, uint8_t scale) {
    if (scale < 1) scale = 1;
    const GFXfont *f = s_font;
    int w = 0, widest = 0;
    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch == '\n') { if (w > widest) widest = w; w = 0; continue; }
        if (ch == '\r') continue;
        if (!f) {
            w += 6 * scale;
        } else {
            const GFXglyph *g = glyphFor(f, ch);
            if (g) w += g->xAdvance * scale;
        }
    }
    return (w > widest) ? w : widest;
}

int gfx_textWidth(const char *s) { return gfx_textWidthScaled(s, 1); }

void gfx_textBounds(const char *s, int x, int y, uint8_t scale,
                    int *bx, int *by, int *bw, int *bh)
{
    if (scale < 1) scale = 1;
    const GFXfont *f = s_font;
    const int x0 = x;
    int minx = 0x7FFF, miny = 0x7FFF, maxx = -0x8000, maxy = -0x8000;

    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch == '\n') {
            x = x0;
            y += (f ? f->yAdvance : 8) * scale;
            continue;
        }
        if (ch == '\r') continue;

        int gx0, gy0, gx1, gy1, adv;
        if (!f) {
            gx0 = x;             gy0 = y;
            gx1 = x + 5 * scale - 1;
            gy1 = y + 7 * scale - 1;
            adv = 6 * scale;
        } else {
            const GFXglyph *g = glyphFor(f, ch);
            if (!g) continue;
            adv = g->xAdvance * scale;
            if (!g->width || !g->height) { x += adv; continue; }
            gx0 = x + g->xOffset * scale;
            gy0 = y + g->yOffset * scale;
            gx1 = gx0 + g->width  * scale - 1;
            gy1 = gy0 + g->height * scale - 1;
        }
        if (gx0 < minx) minx = gx0;
        if (gy0 < miny) miny = gy0;
        if (gx1 > maxx) maxx = gx1;
        if (gy1 > maxy) maxy = gy1;
        x += adv;
    }

    if (maxx < minx) { minx = x0; miny = y; maxx = x0 - 1; maxy = y - 1; }
    if (bx) *bx = minx;
    if (by) *by = miny;
    if (bw) *bw = maxx - minx + 1;
    if (bh) *bh = maxy - miny + 1;
}
