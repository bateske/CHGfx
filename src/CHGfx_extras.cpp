/*
 * CHGfx_extras.cpp - the primitives games kept writing for themselves:
 * rounded rectangles, ellipses, dither and remap fills, span sprites
 * (plain, scaled and rotated) and word-speed row operations.
 *
 * Nothing here costs a sketch that does not call it: every function is
 * in its own section, the SRAM ones included, so the linker drops what
 * is unused.
 */
#include "CHGfx_internal.h"

static_assert(GFX_FB_STRIDE % 4 == 0, "row operations work in words: GFX_W must be a multiple of 8");

#define ROW_WORDS (GFX_FB_STRIDE / 4)

/* floor(sqrt(v)) for v >= 0. */
static int isqrt(int v) {
    if (v <= 0) return 0;
    int r = 0, bit = 1 << 14;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Rounded rectangles                                                  */
/* ------------------------------------------------------------------ */
/*
 * Row i of a corner (0 = the outer edge) starts inset[i] pixels in:
 *     inset[i] = r - floor(sqrt(r^2 - (r - i)^2))
 * which for r = 1..4 is {1}, {2,1}, {3,1,1}, {4,2,1,1}: pixel-art arcs,
 * the table CHChess and CHBlackjack drew their panels with, and it keeps
 * its shape at larger radii. Insets are capped so every row keeps at
 * least a pixel or two.
 */
static int clampRadius(int r, int w, int h) {
    int m = (w < h ? w : h) / 2;
    if (r > m) r = m;
    return r < 0 ? 0 : r;
}

static void cornerInsets(int r, int w, uint8_t *in) {
    int cap = (w - 1) / 2;
    for (int i = 0; i < r; i++) {
        int v = r - isqrt(r * r - (r - i) * (r - i));
        in[i] = (uint8_t)(v > cap ? cap : v);
    }
}

void gfx_fillRoundRect(int x, int y, int w, int h, int r, uint8_t c) {
    if (w <= 0 || h <= 0) return;
    r = clampRadius(r, w, h);
    uint8_t in[GFX_H / 2 + 1];
    cornerInsets(r, w, in);
    for (int i = 0; i < r; i++) {
        gfx_hline(x + in[i], y + i,         w - 2 * in[i], c);
        gfx_hline(x + in[i], y + h - 1 - i, w - 2 * in[i], c);
    }
    gfx_fillRect(x, y + r, w, h - 2 * r, c);
}

void gfx_roundRect(int x, int y, int w, int h, int r, uint8_t c) {
    if (w <= 0 || h <= 0) return;
    r = clampRadius(r, w, h);
    if (!r) { gfx_rect(x, y, w, h, c); return; }
    uint8_t in[GFX_H / 2 + 1];
    cornerInsets(r, w, in);
    gfx_hline(x + in[0], y,         w - 2 * in[0], c);
    gfx_hline(x + in[0], y + h - 1, w - 2 * in[0], c);
    for (int i = 1; i < r; i++) {
        /* The corner's pixels on row i: from its own inset to just short
         * of the row above's, so the arc stays connected. */
        int len = in[i - 1] - in[i];
        if (len < 1) len = 1;
        gfx_hline(x + in[i],           y + i,         len, c);
        gfx_hline(x + w - in[i] - len, y + i,         len, c);
        gfx_hline(x + in[i],           y + h - 1 - i, len, c);
        gfx_hline(x + w - in[i] - len, y + h - 1 - i, len, c);
    }
    gfx_vline(x,         y + r, h - 2 * r, c);
    gfx_vline(x + w - 1, y + r, h - 2 * r, c);
}

/* ------------------------------------------------------------------ */
/* Ellipses                                                            */
/* ------------------------------------------------------------------ */
/*
 * Half-width of row dy: the largest x with
 *     x^2 * ry^2 <= rx^2 * (ry^2 - dy^2 + ry/2)
 * i.e. x = floor(rx * sqrt((ry^2 - dy^2 + ry/2) / ry^2)). The ry/2 term
 * rounds the flat ends the way a hand-drawn ellipse is rounded. x only
 * ever shrinks as dy grows, so it is found by stepping down from the row
 * before - no square roots. Unsigned 32-bit holds it up to rx, ry = 255.
 */
namespace {
struct EllipseRows {
    uint32_t rx2, ry2, ry, x;
    int dy;
    EllipseRows(int rx_, int ry_) : rx2((uint32_t)(rx_ * rx_)), ry2((uint32_t)(ry_ * ry_)),
                                    ry((uint32_t)ry_), x((uint32_t)rx_), dy(-1) {}
    /* Half-width of the next row down (the first call gives row 0). */
    int next() {
        dy++;
        if (!ry) return (int)x;
        uint32_t t = ry2 - (uint32_t)(dy * dy) + (ry >> 1);
        uint32_t lim = rx2 * t;
        while (x && x * x * ry2 > lim) x--;
        return (int)x;
    }
};
}  // namespace

static bool ellipseArgs(int &rx, int &ry) {
    if (rx < 0 || ry < 0) return false;
    if (rx > 255) rx = 255;
    if (ry > 255) ry = 255;
    return true;
}

void gfx_fillEllipse(int cx, int cy, int rx, int ry, uint8_t c) {
    if (!ellipseArgs(rx, ry)) return;
    EllipseRows e(rx, ry);
    for (int dy = 0; dy <= ry; dy++) {
        int hw = e.next();
        gfx_hline(cx - hw, cy + dy, 2 * hw + 1, c);
        if (dy) gfx_hline(cx - hw, cy - dy, 2 * hw + 1, c);
    }
}

void gfx_ellipse(int cx, int cy, int rx, int ry, uint8_t c) {
    if (!ellipseArgs(rx, ry)) return;
    EllipseRows e(rx, ry);
    int hw = e.next();
    for (int dy = 0; dy <= ry; dy++) {
        if (dy == ry) {                          /* the flat top and bottom */
            gfx_hline(cx - hw, cy + dy, 2 * hw + 1, c);
            if (dy) gfx_hline(cx - hw, cy - dy, 2 * hw + 1, c);
            break;
        }
        int below = e.next();                    /* half-width of row dy + 1 */
        /* Row dy's edge runs from just outside the next row's edge out
         * to its own, so steep sides stay connected. */
        int a = below + 1;
        if (a > hw) a = hw;
        int len = hw - a + 1;
        gfx_hline(cx + a,  cy + dy, len, c);
        gfx_hline(cx - hw, cy + dy, len, c);
        if (dy) {
            gfx_hline(cx + a,  cy - dy, len, c);
            gfx_hline(cx - hw, cy - dy, len, c);
        }
        hw = below;
    }
}

/* ------------------------------------------------------------------ */
/* Dither and remap                                                    */
/* ------------------------------------------------------------------ */
/* Clip a rectangle to the clip rectangle. False if nothing is left. */
static bool clipRect(int &x, int &y, int &w, int &h) {
    const GfxClip k = gfx__clip;
    int x1 = x + w, y1 = y + h;
    if (x < k.x0) x = k.x0;
    if (y < k.y0) y = k.y0;
    if (x1 > k.x1) x1 = k.x1;
    if (y1 > k.y1) y1 = k.y1;
    w = x1 - x; h = y1 - y;
    return w > 0 && h > 0;
}

/* Pixels [x0, x1) of a row where the nibble mask m selects them: m is
 * 0x0F for even x only, 0xF0 for odd x only. cc = colour in both nibbles. */
GFX_RAMFUNC(ditherspan) static void maskedSpan(uint8_t *row, int x0, int x1, uint8_t cc, uint8_t m) {
    if (x0 >= x1) return;
    uint8_t *p = row + (x0 >> 1);
    if (x0 & 1) {                                   /* odd first pixel: high nibble */
        if (m & 0xF0) *p = (uint8_t)((*p & 0x0F) | (cc & 0xF0));
        p++; x0++;
    }
    int bytes = (x1 - x0) >> 1;
    uint32_t m32 = m * 0x01010101u, c32 = cc * 0x01010101u;
    while (bytes && ((uintptr_t)p & 3)) { *p = (uint8_t)((*p & ~m) | (cc & m)); p++; bytes--; }
    while (bytes >= 4) { *(uint32_t *)p = (*(uint32_t *)p & ~m32) | (c32 & m32); p += 4; bytes -= 4; }
    while (bytes--) { *p = (uint8_t)((*p & ~m) | (cc & m)); p++; }
    if ((x1 - x0) & 1)                              /* even last pixel: low nibble */
        if (m & 0x0F) *p = (uint8_t)((*p & 0xF0) | (cc & 0x0F));
}

void gfx_dither(int x, int y, int w, int h, uint8_t c, uint8_t phase) {
    if (!clipRect(x, y, w, h)) return;
    c &= 0x0F;
    uint8_t cc = (uint8_t)(c | (c << 4));
    for (int yy = y; yy < y + h; yy++) {
        /* Pixels where x + yy + phase is even: even x on even rows. */
        uint8_t m = ((yy + phase) & 1) ? 0xF0 : 0x0F;
        maskedSpan(gfx__row(yy), x, x + w, cc, m);
    }
}

/* A byte - two pixels - at a time through a 256-entry table built from
 * the 16-entry one: one load, one lookup, one store per two pixels. The
 * table costs ~256 steps to build, so a small rectangle skips it. */
GFX_RAMFUNC(remaprect) void gfx_remapRect(int x, int y, int w, int h, const uint8_t *m) {
    if (!clipRect(x, y, w, h)) return;
    uint8_t t[256];
    bool big = w * h >= 512;
    if (big)
        for (int b = 0; b < 256; b++) t[b] = (uint8_t)((m[b & 15] & 15) | ((m[b >> 4] & 15) << 4));
    for (int j = 0; j < h; j++) {
        uint8_t *row = gfx__row(y + j);
        int i = x, e = x + w;
        if (i & 1) { uint8_t b = row[i >> 1]; row[i >> 1] = (uint8_t)((b & 0x0F) | ((m[b >> 4] & 15) << 4)); i++; }
        uint8_t *p = row + (i >> 1), *pe = row + (e >> 1);
        if (big) for (; p < pe; p++) *p = t[*p];
        else     for (; p < pe; p++) *p = (uint8_t)((m[*p & 15] & 15) | ((m[*p >> 4] & 15) << 4));
        if (e & 1) { uint8_t b = row[e >> 1]; row[e >> 1] = (uint8_t)((b & 0xF0) | (m[b & 15] & 15)); }
    }
}

/* ------------------------------------------------------------------ */
/* Span sprites                                                        */
/* ------------------------------------------------------------------ */
static GFX_INLINE uint8_t mapColour(const uint8_t *remap, uint8_t c) {
    return remap ? (uint8_t)(remap[c] & 0x0F) : c;
}

/* 1:1. A run is a span: the common case is a handful of stores. */
GFX_RAMFUNC(sprite4) static void sprite4Plain(const uint8_t *d, int x, int y, const uint8_t *remap) {
    const GfxClip k = gfx__clip;
    int h = d[1];
    d += 2;
    for (int j = 0; j < h; j++) {
        uint8_t n = *d++;
        const uint8_t *runs = d;
        d += n;
        int yy = y + j;
        if (yy < k.y0) continue;
        if (yy >= k.y1) break;
        uint8_t *row = gfx__row(yy);
        int q = x;
        for (uint8_t i = 0; i < n && q < k.x1; i++) {
            uint8_t b = runs[i];
            int len = (b >> 4) + 1, c = b & 15;
            if (c != 15) {
                int a = q < k.x0 ? k.x0 : q, e = q + len > k.x1 ? k.x1 : q + len;
                gfx__run(row, a, e, mapColour(remap, (uint8_t)c));
            }
            q += len;
        }
    }
}

/* Scaled, nearest neighbour. Source row j covers screen rows
 * [y + j*s/256, y + (j+1)*s/256) and a run [px, px + len) covers columns
 * scaled the same way, so neighbouring runs and rows meet exactly. Kept
 * apart from the 1:1 loop: folding the scale into it measurably slowed
 * the common unscaled case (register pressure). */
GFX_RAMFUNC(sprite4scaled) static void sprite4Scaled(const uint8_t *d, int x, int y,
                                                     const uint8_t *remap, int scale) {
    const GfxClip k = gfx__clip;
    int h = d[1];
    d += 2;
    for (int j = 0; j < h; j++) {
        uint8_t n = *d++;
        const uint8_t *runs = d;
        d += n;
        int r0 = y + ((j * scale) >> 8), r1 = y + (((j + 1) * scale) >> 8);
        if (r1 <= k.y0) continue;
        if (r0 >= k.y1) break;
        if (r0 < k.y0) r0 = k.y0;
        if (r1 > k.y1) r1 = k.y1;
        for (int yy = r0; yy < r1; yy++) {
            uint8_t *row = gfx__row(yy);
            int px = 0;
            for (uint8_t i = 0; i < n; i++) {
                uint8_t b = runs[i];
                int len = (b >> 4) + 1, c = b & 15;
                if (c != 15) {
                    int a = x + ((px * scale) >> 8), e = x + (((px + len) * scale) >> 8);
                    if (a < k.x0) a = k.x0;
                    if (e > k.x1) e = k.x1;
                    gfx__span(row, a, e, mapColour(remap, (uint8_t)c));
                }
                px += len;
            }
        }
    }
}

void gfx_sprite4(const uint8_t *spr, int x, int y, const uint8_t *remap, int scale) {
    if (scale <= 0) return;
    const GfxClip k = gfx__clip;
    int w = spr[0], h = spr[1];
    if (scale != 256) { w = (w * scale) >> 8; h = (h * scale) >> 8; }
    if (x >= k.x1 || y >= k.y1 || x + w <= k.x0 || y + h <= k.y0) return;
    if (scale == 256) sprite4Plain(spr, x, y, remap);
    else              sprite4Scaled(spr, x, y, remap, scale);
}

/* One screen row of a rotated sprite: step (u, v) through the decoded
 * source, 16.16 fixed point, one pixel at a time. */
GFX_RAMFUNC(sprite4rot) static void rotSpan(const uint8_t *src, int sw, int sh, uint8_t *row,
                                            int x0, int x1, int32_t u, int32_t v,
                                            int32_t du, int32_t dv, const uint8_t *remap) {
    int stride = (sw + 1) >> 1;
    for (int x = x0; x < x1; x++, u += du, v += dv) {
        int sx = u >> 16, sy = v >> 16;
        if ((unsigned)sx >= (unsigned)sw || (unsigned)sy >= (unsigned)sh) continue;
        uint8_t b = src[sy * stride + (sx >> 1)];
        uint8_t c = (sx & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 15);
        if (c != 15) gfx__plot(row, x, mapColour(remap, c));
    }
}

void gfx_sprite4Rot(const uint8_t *d, int ax, int ay, int px, int py,
                    uint8_t angle, int scale, const uint8_t *remap) {
    int w = d[0], h = d[1], stride = (w + 1) >> 1;
    if (stride * h > 2 * GFX_CHUNK_BYTES || scale <= 0 || !w || !h) return;
    if (scale < 8) scale = 8;
    gfx_wait();                          /* the scratch is the flush's */

    /* Decode to plain 4 bpp, 15 = transparent. */
    uint8_t *buf = gfx_chunkScratch();
    for (int i = 0; i < stride * h; i++) buf[i] = 0xFF;
    const uint8_t *p = d + 2;
    for (int j = 0; j < h; j++) {
        uint8_t n = *p++;
        int x = 0;
        while (n--) {
            uint8_t b = *p++;
            int len = (b >> 4) + 1;
            uint8_t c = b & 15;
            if (c != 15)
                for (int k = 0; k < len && x + k < w; k++) gfx__plot(buf + j * stride, x + k, c);
            x += len;
        }
    }

    /* Inverse map, screen offset (dx, dy) from the pivot to source:
     *     src = R(-angle) * (dx, dy) / scale + (ax, ay)
     * cos and sin are Q14; divided by the Q8 scale they stay Q14. */
    int32_t ic = (int32_t)gfx__sin14((uint8_t)(angle + 64)) * 256 / scale;
    int32_t is = (int32_t)gfx__sin14(angle) * 256 / scale;

    /* Bounding box: a square round the circle through the corner farthest
     * from the pivot, scaled, plus a pixel for the half-pixel sampling. */
    int d2 = 0;
    const int cx[4] = { -ax, w - ax, -ax, w - ax }, cy[4] = { -ay, -ay, h - ay, h - ay };
    for (int k = 0; k < 4; k++) {
        int m = cx[k] * cx[k] + cy[k] * cy[k];
        if (m > d2) d2 = m;
    }
    int r = (isqrt(d2) + 2) * scale / 256 + 1;
    const GfxClip k = gfx__clip;
    int y0 = py - r, y1 = py + r, x0 = px - r, x1 = px + r;
    if (y0 < k.y0) y0 = k.y0;
    if (y1 > k.y1) y1 = k.y1;
    if (x0 < k.x0) x0 = k.x0;
    if (x1 > k.x1) x1 = k.x1;
    if (x0 >= x1) return;

    for (int y = y0; y < y1; y++) {
        int dy = y - py, dx = x0 - px;
        /* Sample pixel centres: +0.5 in source space. Q14 -> 16.16 is <<2. */
        int32_t u = ((ic * dx + is * dy) << 2) + ((int32_t)ax << 16) + 0x8000;
        int32_t v = ((-is * dx + ic * dy) << 2) + ((int32_t)ay << 16) + 0x8000;
        rotSpan(buf, w, h, gfx__row(y), x0, x1, u, v, ic << 2, -is << 2, remap);
    }
}

/* ------------------------------------------------------------------ */
/* Row operations                                                      */
/* ------------------------------------------------------------------ */
/* Pixels [a, b) from row src to row dst, same x: ragged nibble ends,
 * word copies in the middle when both rows are word aligned. */
GFX_RAMFUNC(copypixels) static void copyPixels(uint8_t *dst, const uint8_t *src, int a, int b) {
    if (a >= b) return;
    if (a & 1) { dst[a >> 1] = (uint8_t)((dst[a >> 1] & 0x0F) | (src[a >> 1] & 0xF0)); a++; }
    if (b & 1) { b--; dst[b >> 1] = (uint8_t)((dst[b >> 1] & 0xF0) | (src[b >> 1] & 0x0F)); }
    int i = a >> 1, e = b >> 1;
    if ((((uintptr_t)dst ^ (uintptr_t)src) & 3) == 0) {
        while (i < e && ((uintptr_t)(dst + i) & 3)) { dst[i] = src[i]; i++; }
        for (; i + 4 <= e; i += 4) *(uint32_t *)(dst + i) = *(const uint32_t *)(src + i);
    }
    for (; i < e; i++) dst[i] = src[i];
}

void gfx_copyRow(int y, const uint8_t *src, int x0, int x1) {
    const GfxClip k = gfx__clip;
    if (y < k.y0 || y >= k.y1) return;
    if (x0 < k.x0) x0 = k.x0;
    if (x1 > k.x1) x1 = k.x1;
    copyPixels(gfx__row(y), src, x0, x1);
}

/* A row shifted dx pixels right (left if negative), treated as one
 * 16-word little-endian number: pixel p sits at bits 4p..4p+3, so moving
 * pixels right is a left shift by 4*dx bits. out may be in: the loops run
 * away from the direction of travel. Words wholly uncovered by the shift
 * are not written, and the partly uncovered one is left with zeros where
 * nothing moved in - the caller fixes both. */
GFX_RAMFUNC(shiftrow) static void shiftRow(uint32_t *out, const uint32_t *in, int dx) {
    int n = 4 * (dx < 0 ? -dx : dx);
    int q = n >> 5, b = n & 31;
    if (q >= ROW_WORDS) return;
    if (dx > 0) {
        if (b) {
            for (int j = ROW_WORDS - 1; j > q; j--) out[j] = (in[j - q] << b) | (in[j - q - 1] >> (32 - b));
            out[q] = in[0] << b;
        } else {
            for (int j = ROW_WORDS - 1; j >= q; j--) out[j] = in[j - q];
        }
    } else {
        const int last = ROW_WORDS - 1 - q;
        if (b) {
            for (int j = 0; j < last; j++) out[j] = (in[j + q] >> b) | (in[j + q + 1] << (32 - b));
            out[last] = in[ROW_WORDS - 1] >> b;
        } else {
            for (int j = 0; j <= last; j++) out[j] = in[j + q];
        }
    }
}

GFX_RAMFUNC(copywords) static void copyWords(uint32_t *d, const uint32_t *s) {
    for (int j = 0; j < ROW_WORDS; j++) d[j] = s[j];
}

void gfx_scroll(int y, int h, int dx, int dy, int fill) {
    int y1 = y + h;
    if (y < 0) y = 0;
    if (y1 > GFX_H) y1 = GFX_H;
    if (y >= y1 || (!dx && !dy)) return;
    const uint8_t fc = (uint8_t)(fill & 0x0F);

    /* Columns the horizontal move uncovers. */
    int ua = 0, ub = 0;
    if (dx > 0)      { ua = 0;          ub = dx < GFX_W ? dx : GFX_W; }
    else if (dx < 0) { ua = GFX_W + dx; ub = GFX_W; if (ua < 0) ua = 0; }

    /* Walk away from the direction of travel, so every source row is read
     * before anything overwrites it. */
    int n = y1 - y;
    for (int k = 0; k < n; k++) {
        int r = dy > 0 ? y1 - 1 - k : y + k;
        int s = r - dy;
        uint8_t *dst = gfx__row(r);
        if (s < y || s >= y1) {                    /* nothing moves into this row */
            if (fill >= 0) gfx__span(dst, 0, GFX_W, fc);
            continue;
        }
        const uint8_t *src = gfx__row(s);
        if (!dx) {
            if (s != r) copyWords((uint32_t *)dst, (const uint32_t *)src);
            continue;
        }
        /* Shift straight into the destination. To keep what the uncovered
         * pixels had, save the words they are in first. */
        uint32_t *dw = (uint32_t *)dst, save[ROW_WORDS];
        if (fill < 0)
            for (int i = ua >> 3; i < (ub + 7) >> 3; i++) save[i] = dw[i];
        shiftRow(dw, (const uint32_t *)src, dx);
        if (fill >= 0) gfx__span(dst, ua, ub, fc);
        else           copyPixels(dst, (const uint8_t *)save, ua, ub);
    }
}
