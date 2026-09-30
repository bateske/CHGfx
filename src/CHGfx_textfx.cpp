/*
 * CHGfx_textfx.cpp - outlined, shadowed, gradient-filled text.
 *
 * The naive way to outline text is to print it eight times, offset in
 * every direction, then once more on top: nine full passes of the glyph
 * renderer. Instead the text is rendered ONCE into a 1 bpp mask, the
 * outline is grown out of the mask with byte-wide ORs (a row ORed with
 * its neighbours above and below, then with itself shifted a bit left
 * and right), and every shape is painted as horizontal spans with the
 * framebuffer's word stores.
 *
 * The mask lives in gfx_chunkScratch(): the flush's chunk buffers, idle
 * between gfx_wait() and the next flush. Hence the 1 KB limit, and the
 * gfx_wait() at the top.
 */
#include "CHGfx_internal.h"

#define MASK_BYTES   (2 * GFX_CHUNK_BYTES)
#define MASK_STRIDE  32                    /* bytes per mask row, at most: 256 px */

namespace {

/* The mask: bit (mx, my) is screen pixel (ox + mx, oy + my). MSB first. */
struct Mask {
    uint8_t *bits;
    int stride, w, h;
    int ox, oy;
};

void setBits(uint8_t *row, int a, int b) {
    while (a < b && (a & 7)) { row[a >> 3] |= (uint8_t)(0x80 >> (a & 7)); a++; }
    while (a + 8 <= b)       { row[a >> 3] = 0xFF; a += 8; }
    while (a < b)            { row[a >> 3] |= (uint8_t)(0x80 >> (a & 7)); a++; }
}

/* Set a screen-space rectangle in the mask. */
void setBlock(const Mask &m, int sx, int sy, int w, int h) {
    int x0 = sx - m.ox, y0 = sy - m.oy, x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > m.w) x1 = m.w;
    if (y1 > m.h) y1 = m.h;
    for (int y = y0; y < y1; y++) setBits(m.bits + y * m.stride, x0, x1);
}

/* Built-in 5x7: columns, bit 0 = top, seven rows. One block per
 * vertical run. */
void maskBuiltin(const Mask &m, int x, int y, char ch, int scale) {
    const uint8_t *g = gfx__builtinGlyph(ch);
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col] & 0x7F;
        int row = 0;
        while (bits) {
            while (!(bits & 1)) { bits >>= 1; row++; }
            int n = 0;
            while (bits & 1) { bits >>= 1; n++; }
            setBlock(m, x + col * scale, y + row * scale, scale, n * scale);
            row += n;
        }
    }
}

/* GFXfont: a row-major bitstream. One block per horizontal run. */
void maskGlyph(const Mask &m, const GFXfont *f, const GFXglyph *g, int x, int y, int scale) {
    int gw = g->width, gh = g->height;
    const uint8_t *p = f->bitmap + g->bitmapOffset;
    int gx = x + g->xOffset * scale, gy = y + g->yOffset * scale;
    uint32_t cur = 0;
    int nbits = 0;
    for (int r = 0; r < gh; r++) {
        int run = -1;
        for (int col = 0; col < gw; col++) {
            if (!nbits) { cur = *p++; nbits = 8; }
            bool on = (cur & 0x80) != 0;
            cur = (cur << 1) & 0xFF;
            nbits--;
            if (on && run < 0) run = col;
            if (!on && run >= 0) { setBlock(m, gx + run * scale, gy + r * scale, (col - run) * scale, scale); run = -1; }
        }
        if (run >= 0) setBlock(m, gx + run * scale, gy + r * scale, (gw - run) * scale, scale);
    }
}

const GFXglyph *glyphFor(const GFXfont *f, unsigned char ch) {
    if (ch < f->first || ch > f->last) {
        if ('?' < f->first || '?' > f->last) return nullptr;
        ch = '?';
    }
    return &f->glyph[ch - f->first];
}

/* Lay the string out exactly as gfx_textScaled() does, into the mask. */
void maskText(const Mask &m, int x, int y, const char *s, int scale, const int8_t *dy) {
    const GFXfont *f = gfx_font();
    const int x0 = x;
    for (int k = 0; s[k]; k++) {
        unsigned char ch = (unsigned char)s[k];
        if (ch == '\n') { x = x0; y += (f ? f->yAdvance : 8) * scale; continue; }
        if (ch == '\r') continue;
        int yy = y + (dy ? dy[k] : 0);
        if (!f) {
            maskBuiltin(m, x, yy, (char)ch, scale);
            x += 6 * scale;
        } else {
            const GFXglyph *g = glyphFor(f, ch);
            if (!g) continue;
            if (g->width && g->height) maskGlyph(m, f, g, x, yy, scale);
            x += g->xAdvance * scale;
        }
    }
}

/* Grow mask row r by one pixel in all eight directions into out. */
GFX_RAMFUNC(textfxdilate) void dilateRow(const Mask &m, int r, uint8_t *out) {
    const uint8_t *row = m.bits + r * m.stride;
    const uint8_t *up = r > 0 ? row - m.stride : nullptr;
    const uint8_t *dn = r + 1 < m.h ? row + m.stride : nullptr;
    uint8_t v[MASK_STRIDE];
    for (int b = 0; b < m.stride; b++)
        v[b] = (uint8_t)(row[b] | (up ? up[b] : 0) | (dn ? dn[b] : 0));
    for (int b = 0; b < m.stride; b++) {
        uint8_t l = (uint8_t)(v[b] << 1 | (b + 1 < m.stride ? v[b + 1] >> 7 : 0));   /* from x+1 */
        uint8_t rr = (uint8_t)(v[b] >> 1 | (b > 0 ? v[b - 1] << 7 : 0));           /* from x-1 */
        out[b] = (uint8_t)(v[b] | l | rr);
    }
}

/* Four MSB-first mask bits as framebuffer nibble masks: the leftmost
 * pixel is the low nibble of the first byte. */
const uint16_t EXPAND[16] = {
    0x0000, 0xF000, 0x0F00, 0xFF00, 0x00F0, 0xF0F0, 0x0FF0, 0xFFF0,
    0x000F, 0xF00F, 0x0F0F, 0xFF0F, 0x00FF, 0xF0FF, 0x0FFF, 0xFFFF
};

/* Paint the set bits of one mask row, bit 0 at screen x sx, row sy,
 * clipped. A mask byte is eight pixels, four framebuffer bytes: widen it
 * to a nibble mask and merge the colour in with it, a byte at a time. */
GFX_RAMFUNC(textfxpaint) void paintRow(const uint8_t *bits, int n, int sx, int sy, uint8_t c) {
    const GfxClip k = gfx__clip;
    if (sy < k.y0 || sy >= k.y1) return;
    int v0 = k.x0 - sx, v1 = k.x1 - sx;          /* visible bits [v0, v1) */
    if (v0 < 0) v0 = 0;
    if (v1 > n) v1 = n;
    if (v0 >= v1) return;
    uint8_t *row = gfx__row(sy);
    const uint32_t cc = c * 0x11111111u;
    const int i0 = v0 >> 3, i1 = (v1 - 1) >> 3;
    for (int i = i0; i <= i1; i++) {
        uint32_t b = bits[i];
        if (i == i0) b &= 0xFFu >> (v0 & 7);
        if (i == i1) b &= 0xFF00u >> (((v1 - 1) & 7) + 1);
        if (!b) continue;
        uint32_t m = EXPAND[b >> 4] | (uint32_t)EXPAND[b & 15] << 16, spill = 0;
        int x = sx + (i << 3);
        if (x & 1) { spill = m >> 28; m <<= 4; }  /* odd x: everything a nibble along */
        /* Only bytes with pixels to paint are touched, and those are on
         * screen, even when x itself is not. */
        int base = x >> 1;
        for (int j = 0; j < 4; j++, m >>= 8)
            if (m & 0xFF) { uint8_t &q = row[base + j]; q = (uint8_t)((q & ~m) | (cc & m)); }
        if (spill) { uint8_t &q = row[base + 4]; q = (uint8_t)((q & ~spill) | (cc & spill)); }
    }
}

}  // namespace

bool gfx_textFx(int x, int y, const char *s, uint8_t scale, uint8_t fill,
                int outline, int shadow, const uint8_t *ramp, const int8_t *dy)
{
    if (scale < 1) scale = 1;

    /* Ink box, stretched by the per-character offsets. */
    int bx, by, bw, bh;
    gfx_textBounds(s, x, y, scale, &bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0) return true;               /* nothing to draw */
    int dmin = 0, dmax = 0;
    if (dy) {
        for (int k = 0; s[k]; k++) {
            if (dy[k] < dmin) dmin = dy[k];
            if (dy[k] > dmax) dmax = dy[k];
        }
    }
    by += dmin;
    bh += dmax - dmin;

    /* The mask: the ink box plus a 1 px margin all round for the outline. */
    Mask m;
    m.w = bw + 2; m.h = bh + 2;
    m.stride = (m.w + 7) >> 3;
    m.ox = bx - 1; m.oy = by - 1;
    if (m.stride > MASK_STRIDE || m.stride * m.h > MASK_BYTES) return false;

    /* Nothing of it inside the clip: done, and no need to wait. The +1s
     * are the shadow's reach. */
    const GfxClip k = gfx__clip;
    if (m.ox >= k.x1 || m.oy >= k.y1 || m.ox + m.w + 1 <= k.x0 || m.oy + m.h + 1 <= k.y0) return true;

    gfx_wait();
    m.bits = gfx_chunkScratch();
    for (int i = 0; i < m.stride * m.h; i++) m.bits[i] = 0;
    maskText(m, x, y, s, scale, dy);

    uint8_t d[MASK_STRIDE];
    if (shadow >= 0) {
        uint8_t c = (uint8_t)(shadow & 0x0F);
        for (int r = 0; r < m.h; r++) {
            const uint8_t *src = m.bits + r * m.stride;
            if (outline >= 0) { dilateRow(m, r, d); src = d; }
            paintRow(src, m.w, m.ox + 1, m.oy + r + 1, c);
        }
    }
    if (outline >= 0) {
        uint8_t c = (uint8_t)(outline & 0x0F);
        for (int r = 0; r < m.h; r++) {
            dilateRow(m, r, d);
            paintRow(d, m.w, m.ox, m.oy + r, c);
        }
    }
    for (int r = 1; r < m.h - 1; r++) {
        uint8_t c = (uint8_t)((ramp ? ramp[r - 1] : fill) & 0x0F);
        paintRow(m.bits + r * m.stride, m.w, m.ox, m.oy + r, c);
    }
    return true;
}
