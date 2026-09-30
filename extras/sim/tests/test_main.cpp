/*
 * CHGfx host tests:   python extras/sim/chsim.py test
 *
 * The library's drawing code, compiled for the PC exactly as the board
 * compiles it, checked three ways:
 *
 *   1. Against a reference: a deliberately naive, pixel-at-a-time
 *      implementation of what each call is documented to do, on random
 *      arguments that run off every edge of the screen.
 *   2. Against the clip invariant: with a clip rectangle set, a call must
 *      produce exactly what it produces unclipped inside the rectangle,
 *      and leave every pixel outside it alone. That one rule covers every
 *      primitive without a second reference for each.
 *   3. Properties, where the exact pixels are a matter of taste (outlined
 *      shapes): the outline covers the filled shape's edge and nothing
 *      outside the filled shape.
 *
 * Plus the palette fade, the 3x5 font's glyphs, and the simulator's own
 * flush model (progress, tearing detection), which the other tests rely
 * on.
 */
#include <Arduino.h>
#include <CHGfx.h>
#include <CHGfx_internal.h>
#include <CHGfx_font.h>
#include <fonts/CHGfx_Sans12.h>
#include <fonts/CHGfx_Tiny3x5.h>
#include <vector>
#include <functional>
#include "tiny3x5_cols.h"
#include "../host/sim.h"

/* ------------------------------------------------------------------ */
/* The simulator hooks chsim_gfx.cpp needs                             */
/* ------------------------------------------------------------------ */
static uint32_t s_now = 0;
static int s_bugs = 0, s_frames = 0;
uint32_t sim_now() { return s_now; }
void sim_advance(uint32_t us) { s_now += us; sim_flushProgress(s_now); }
void sim_sync() {}
void sim_bug(const char *, ...) { s_bugs++; }
bool sim_buttonHeld(uint32_t) { return false; }
void sim_framePresented() { s_frames++; }

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */
static int s_checks = 0, s_failures = 0;
static char s_case[256];

static uint32_t s_rng = 12345;
static uint32_t rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }   /* inclusive */

typedef std::vector<uint8_t> Fb;            /* one byte per pixel, W*H */

static Fb grab() {
    Fb f(GFX_W * GFX_H);
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) f[y * GFX_W + x] = gfx_getPixel(x, y);
    return f;
}
static void load(const Fb &f) {
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) {
            uint8_t &b = gfx_fb[y * GFX_FB_STRIDE + (x >> 1)];
            uint8_t c = f[y * GFX_W + x];
            b = (x & 1) ? (uint8_t)((b & 0x0F) | (c << 4)) : (uint8_t)((b & 0xF0) | c);
        }
}
static Fb randomFb() {
    Fb f(GFX_W * GFX_H);
    for (auto &p : f) p = (uint8_t)(rnd() & 15);
    return f;
}
static inline bool onScreen(int x, int y) { return x >= 0 && y >= 0 && x < GFX_W && y < GFX_H; }
static inline void rput(Fb &f, int x, int y, int c) { if (onScreen(x, y)) f[y * GFX_W + x] = (uint8_t)(c & 15); }

static bool same(const Fb &got, const Fb &want, const char *what) {
    s_checks++;
    for (int i = 0; i < GFX_W * GFX_H; i++)
        if (got[i] != want[i]) {
            s_failures++;
            if (s_failures <= 25)
                printf("FAIL %s [%s]: pixel (%d,%d) is %d, want %d\n", what, s_case,
                       i % GFX_W, i / GFX_W, got[i], want[i]);
            return false;
        }
    return true;
}
static void check(bool ok, const char *what) {
    s_checks++;
    if (!ok) { s_failures++; if (s_failures <= 25) printf("FAIL %s [%s]\n", what, s_case); }
}

/* Run op on a random background; compare with ref applied to the same. */
static void vsRef(const char *what, const std::function<void()> &op, const std::function<void(Fb &)> &ref) {
    Fb bg = randomFb();
    load(bg);
    gfx_resetClip();
    op();
    Fb want = bg;
    ref(want);
    same(grab(), want, what);
}

/* The clip invariant, for one op and one random clip rectangle. */
static void vsClip(const char *what, const std::function<void()> &op) {
    Fb bg = randomFb();
    load(bg);
    gfx_resetClip();
    op();
    Fb full = grab();
    int cx = rr(-20, 130), cy = rr(-20, 130), cw = rr(-5, 140), ch = rr(-5, 140);
    load(bg);
    gfx_setClip(cx, cy, cw, ch);
    op();
    Fb got = grab();
    gfx_resetClip();
    Fb want = bg;
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++)
            if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) want[y * GFX_W + x] = full[y * GFX_W + x];
    char buf[64];
    snprintf(buf, sizeof buf, "%s, clip %d,%d %dx%d", what, cx, cy, cw, ch);
    same(got, want, buf);
}

/* ------------------------------------------------------------------ */
/* Sprites for the tests                                               */
/* ------------------------------------------------------------------ */
struct Art { int w, h; std::vector<uint8_t> px; };        /* 15 = transparent */

static Art randomArt(int maxW, int maxH) {
    Art a;
    a.w = rr(1, maxW); a.h = rr(1, maxH);
    a.px.resize(a.w * a.h);
    for (int y = 0; y < a.h; y++) {
        int x = 0;
        while (x < a.w) {                     /* runs, so the packer has work to do */
            int len = rr(1, 22), c = (rnd() % 4 == 0) ? 15 : rr(0, 15);
            for (int k = 0; k < len && x < a.w; k++, x++) a.px[y * a.w + x] = (uint8_t)c;
        }
    }
    return a;
}

/* The format's packer, as extras/sprite4.py and the games' tools do it. */
static std::vector<uint8_t> pack4(const Art &a) {
    std::vector<uint8_t> out = { (uint8_t)a.w, (uint8_t)a.h };
    for (int y = 0; y < a.h; y++) {
        std::vector<uint8_t> runs;
        int x = 0;
        while (x < a.w) {
            int c = a.px[y * a.w + x], s = x;
            while (x < a.w && a.px[y * a.w + x] == c && x - s < 16) x++;
            runs.push_back((uint8_t)(((x - s - 1) << 4) | c));
        }
        while (!runs.empty() && (runs.back() & 15) == 15) runs.pop_back();
        out.push_back((uint8_t)runs.size());
        out.insert(out.end(), runs.begin(), runs.end());
    }
    return out;
}

/* 4 bpp packed, like the framebuffer, for gfx_blit. */
static std::vector<uint8_t> packBlit(const Art &a) {
    int stride = (a.w + 1) / 2;
    std::vector<uint8_t> out(stride * a.h, 0);
    for (int y = 0; y < a.h; y++)
        for (int x = 0; x < a.w; x++) {
            uint8_t c = a.px[y * a.w + x] & 15;
            out[y * stride + x / 2] |= (uint8_t)((x & 1) ? c << 4 : c);
        }
    return out;
}

static std::vector<uint8_t> randomRemap() {
    std::vector<uint8_t> m(16);
    for (auto &v : m) v = (uint8_t)rr(0, 15);
    return m;
}

/* ------------------------------------------------------------------ */
/* References                                                          */
/* ------------------------------------------------------------------ */
static void refRect(Fb &f, int x, int y, int w, int h, int c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) rput(f, i, j, c);
}

/* Text is drawn through a callback so the textFx reference can use a
 * canvas bigger than the screen: ink just off-screen still outlines
 * pixels on it. */
typedef std::function<void(int, int)> Put;

static void putRect(const Put &put, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) put(i, j);
}

/* GFXfont glyph pixels, straight from the bitstream. */
static void refGlyph(const Put &put, const GFXfont *font, const GFXglyph *g, int x, int y, int s) {
    const uint8_t *bmp = font->bitmap + g->bitmapOffset;
    int bit = 0;
    for (int r = 0; r < g->height; r++)
        for (int col = 0; col < g->width; col++, bit++)
            if (bmp[bit >> 3] & (0x80 >> (bit & 7)))
                putRect(put, x + (g->xOffset + col) * s, y + (g->yOffset + r) * s, s, s);
}

static void refTextPut(const Put &put, const GFXfont *font, int x, int y, const char *str, int s) {
    int x0 = x;
    for (; *str; str++) {
        unsigned char ch = (unsigned char)*str;
        if (ch == '\n') { x = x0; y += (font ? font->yAdvance : 8) * s; continue; }
        if (!font) {
            if (ch < 32 || ch > 126) ch = '?';
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (chgfx_font5x7[(ch - 32) * 5 + col] & (1 << row)) putRect(put, x + col * s, y + row * s, s, s);
            x += 6 * s;
        } else {
            if (ch < font->first || ch > font->last) ch = '?';
            const GFXglyph *g = &font->glyph[ch - font->first];
            refGlyph(put, font, g, x, y, s);
            x += g->xAdvance * s;
        }
    }
}

static void refText(Fb &f, const GFXfont *font, int x, int y, const char *str, int c, int s) {
    refTextPut([&](int i, int j) { rput(f, i, j, c); }, font, x, y, str, s);
}

static std::string randomText() {
    static const char *words[] = { "Hello", "CH32", "gfx", "Quick", "jumpy", "{A~B}", "0123", "?!", "\n", " ", "pq@#" };
    std::string s;
    int n = rr(1, 5);
    for (int i = 0; i < n; i++) s += words[rr(0, 10)];
    return s;
}

/* Pixels a draw call touched: run it onto two different uniform
 * backgrounds and see which pixels changed on either. */
static Fb inked(const std::function<void()> &op) {
    Fb mask(GFX_W * GFX_H, 0);
    for (uint8_t bgc : { (uint8_t)0, (uint8_t)15 }) {
        gfx_resetClip();
        gfx_clear(bgc);
        op();
        Fb g = grab();
        for (int i = 0; i < GFX_W * GFX_H; i++) if (g[i] != bgc) mask[i] = 1;
    }
    return mask;
}

static int isqrtRef(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */
static void testBasics() {
    for (int t = 0; t < 400; t++) {
        int x = rr(-30, 140), y = rr(-30, 140), w = rr(-3, 140), h = rr(-3, 140), c = rr(0, 15);
        snprintf(s_case, sizeof s_case, "%d,%d %dx%d c%d", x, y, w, h, c);
        vsRef("pixel", [&] { gfx_pixel(x, y, (uint8_t)c); }, [&](Fb &f) { rput(f, x, y, c); });
        vsRef("hline", [&] { gfx_hline(x, y, w, (uint8_t)c); }, [&](Fb &f) { refRect(f, x, y, w, 1, c); });
        vsRef("vline", [&] { gfx_vline(x, y, h, (uint8_t)c); }, [&](Fb &f) { refRect(f, x, y, 1, h, c); });
        vsRef("fillRect", [&] { gfx_fillRect(x, y, w, h, (uint8_t)c); }, [&](Fb &f) { refRect(f, x, y, w, h, c); });
        vsRef("rect", [&] { gfx_rect(x, y, w, h, (uint8_t)c); }, [&](Fb &f) {
            if (w <= 0 || h <= 0) return;
            refRect(f, x, y, w, 1, c); refRect(f, x, y + h - 1, w, 1, c);
            refRect(f, x, y, 1, h, c); refRect(f, x + w - 1, y, 1, h, c);
        });
        vsRef("dither", [&] { gfx_dither(x, y, w, h, (uint8_t)c, (uint8_t)(t & 1)); }, [&](Fb &f) {
            for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
                if (((i + j + (t & 1)) & 1) == 0) rput(f, i, j, c);
        });
        std::vector<uint8_t> m = randomRemap();
        vsRef("remapRect", [&] { gfx_remapRect(x, y, w, h, m.data()); }, [&](Fb &f) {
            for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
                if (onScreen(i, j)) f[j * GFX_W + i] = m[f[j * GFX_W + i]];
        });
        vsRef("clear", [&] { gfx_clear((uint8_t)c); }, [&](Fb &f) { refRect(f, 0, 0, GFX_W, GFX_H, c); });

        vsClip("pixel", [&] { gfx_pixel(x, y, (uint8_t)c); });
        vsClip("hline", [&] { gfx_hline(x, y, w, (uint8_t)c); });
        vsClip("vline", [&] { gfx_vline(x, y, h, (uint8_t)c); });
        vsClip("fillRect", [&] { gfx_fillRect(x, y, w, h, (uint8_t)c); });
        vsClip("rect", [&] { gfx_rect(x, y, w, h, (uint8_t)c); });
        vsClip("dither", [&] { gfx_dither(x, y, w, h, (uint8_t)c, 1); });
        vsClip("remapRect", [&] { gfx_remapRect(x, y, w, h, m.data()); });
        vsClip("clear", [&] { gfx_clear((uint8_t)c); });
        int x1 = rr(-40, 170), y1 = rr(-40, 170), r = rr(0, 70);
        vsClip("line", [&] { gfx_line(x, y, x1, y1, (uint8_t)c); });
        vsClip("circle", [&] { gfx_circle(x, y, r, (uint8_t)c); });
        vsClip("fillCircle", [&] { gfx_fillCircle(x, y, r, (uint8_t)c); });
    }
}

static void testBlit() {
    for (int t = 0; t < 600; t++) {
        Art a = randomArt(24, 24);
        std::vector<uint8_t> data = packBlit(a);
        int x = rr(-30, 140), y = rr(-30, 140), tr = (t % 3 == 0) ? -1 : rr(0, 15);
        if (t % 5 == 0) x &= ~1;              /* make sure the aligned paths get a workout */
        snprintf(s_case, sizeof s_case, "%dx%d at %d,%d tr %d", a.w, a.h, x, y, tr);
        vsRef("blit", [&] { gfx_blit(data.data(), x, y, a.w, a.h, tr); }, [&](Fb &f) {
            for (int j = 0; j < a.h; j++) for (int i = 0; i < a.w; i++) {
                int c = a.px[j * a.w + i] & 15;
                if (tr >= 0 && c == tr) continue;
                rput(f, x + i, y + j, c);
            }
        });
        vsClip("blit", [&] { gfx_blit(data.data(), x, y, a.w, a.h, tr); });
    }
}

static void refSprite4(Fb &f, const Art &a, int x, int y, const uint8_t *remap, int s) {
    for (int j = 0; j < a.h; j++) for (int i = 0; i < a.w; i++) {
        int c = a.px[j * a.w + i];
        if (c == 15) continue;
        int col = remap ? remap[c] : c;
        int x0 = x + ((i * s) >> 8), x1 = x + (((i + 1) * s) >> 8);
        int y0 = y + ((j * s) >> 8), y1 = y + (((j + 1) * s) >> 8);
        refRect(f, x0, y0, x1 - x0, y1 - y0, col);
    }
}

static void testSprite4() {
    static const int scales[] = { 256, 256, 256, 512, 384, 300, 200, 128, 77, 1000 };
    for (int t = 0; t < 800; t++) {
        Art a = randomArt(40, 40);
        std::vector<uint8_t> data = pack4(a);
        std::vector<uint8_t> m = randomRemap();
        const uint8_t *remap = (t & 1) ? m.data() : nullptr;
        int x = rr(-60, 140), y = rr(-60, 140), s = scales[t % 10];
        snprintf(s_case, sizeof s_case, "%dx%d at %d,%d scale %d%s", a.w, a.h, x, y, s, remap ? " remap" : "");
        vsRef("sprite4", [&] { gfx_sprite4(data.data(), x, y, remap, s); },
              [&](Fb &f) { refSprite4(f, a, x, y, remap, s); });
        vsClip("sprite4", [&] { gfx_sprite4(data.data(), x, y, remap, s); });
    }
}

static void testSprite4Rot() {
    for (int t = 0; t < 300; t++) {
        Art a = randomArt(32, 32);
        std::vector<uint8_t> data = pack4(a);
        int ax = rr(0, a.w - 1), ay = rr(0, a.h - 1), px = rr(-10, 138), py = rr(-10, 138);
        int quarter = t % 4;
        snprintf(s_case, sizeof s_case, "%dx%d pivot %d,%d at %d,%d, %d/4 turn", a.w, a.h, ax, ay, px, py, quarter);
        /* Quarter turns are exact in Q14, so they must match a plain
         * rotation of the art: clockwise on screen, pivot on (px, py). */
        vsRef("sprite4Rot", [&] { gfx_sprite4Rot(data.data(), ax, ay, px, py, (uint8_t)(quarter * 64)); },
              [&](Fb &f) {
                  for (int j = 0; j < a.h; j++) for (int i = 0; i < a.w; i++) {
                      int c = a.px[j * a.w + i];
                      if (c == 15) continue;
                      int u = i - ax, v = j - ay, sx, sy;
                      switch (quarter) {
                          case 0:  sx = u;  sy = v;  break;
                          case 1:  sx = -v; sy = u;  break;
                          case 2:  sx = -u; sy = -v; break;
                          default: sx = v;  sy = -u; break;
                      }
                      rput(f, px + sx, py + sy, c);
                  }
              });
        uint8_t ang = (uint8_t)rnd();
        int s = rr(64, 700);
        vsClip("sprite4Rot", [&] { gfx_sprite4Rot(data.data(), ax, ay, px, py, ang, s); });
        /* Any angle and scale: the same fixed-point mapping, evaluated for
         * every pixel on the screen - so a bounding box that cut the
         * sprite short would show. */
        snprintf(s_case, sizeof s_case, "%dx%d pivot %d,%d at %d,%d angle %d scale %d", a.w, a.h, ax, ay, px, py, ang, s);
        vsRef("sprite4Rot box", [&] { gfx_sprite4Rot(data.data(), ax, ay, px, py, ang, s); }, [&](Fb &f) {
            int32_t ic = (int32_t)gfx__sin14((uint8_t)(ang + 64)) * 256 / s;
            int32_t is = (int32_t)gfx__sin14(ang) * 256 / s;
            for (int y = 0; y < GFX_H; y++) for (int x = 0; x < GFX_W; x++) {
                int dx = x - px, dy = y - py;
                int32_t u = ((ic * dx + is * dy) << 2) + ((int32_t)ax << 16) + 0x8000;
                int32_t v = ((-is * dx + ic * dy) << 2) + ((int32_t)ay << 16) + 0x8000;
                int sx = u >> 16, sy = v >> 16;
                if (sx < 0 || sy < 0 || sx >= a.w || sy >= a.h) continue;
                int c = a.px[sy * a.w + sx];
                if (c != 15) f[y * GFX_W + x] = (uint8_t)c;
            }
        });
    }
}

static void testShapes() {
    for (int t = 0; t < 400; t++) {
        int x = rr(-20, 120), y = rr(-20, 120), w = rr(1, 90), h = rr(1, 90), r = rr(0, 50), c = rr(0, 15);
        snprintf(s_case, sizeof s_case, "%d,%d %dx%d r%d", x, y, w, h, r);
        vsClip("fillRoundRect", [&] { gfx_fillRoundRect(x, y, w, h, r, (uint8_t)c); });
        vsClip("roundRect", [&] { gfx_roundRect(x, y, w, h, r, (uint8_t)c); });

        /* The fill stays inside its box, and is the box less its corners. */
        Fb fill = inked([&] { gfx_fillRoundRect(x, y, w, h, r, 1); });
        Fb line = inked([&] { gfx_roundRect(x, y, w, h, r, 1); });
        bool inside = true, edge = true, sub = true;
        for (int j = 0; j < GFX_H; j++) for (int i = 0; i < GFX_W; i++) {
            bool f = fill[j * GFX_W + i], l = line[j * GFX_W + i];
            if (f && (i < x || i >= x + w || j < y || j >= y + h)) inside = false;
            if (l && !f) sub = false;
            /* An edge of the fill: filled, with a 4-neighbour outside it
             * (off-screen neighbours do not count). */
            if (f) {
                bool e = false;
                const int d[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
                for (auto &k : d) {
                    int ni = i + k[0], nj = j + k[1];
                    if (onScreen(ni, nj) && !fill[nj * GFX_W + ni]) e = true;
                }
                if (e && !l) edge = false;
            }
        }
        check(inside, "fillRoundRect inside its box");
        check(sub, "roundRect inside fillRoundRect");
        check(edge, "roundRect covers fillRoundRect's edge");
        /* The corners are the published pixel-art table for r <= 4. */
        if (r >= 1 && r <= 4 && w >= 2 * r + 1 && h >= 2 * r) {
            static const uint8_t INSET[4][4] = { {1}, {2, 1}, {3, 1, 1}, {4, 2, 1, 1} };
            vsRef("fillRoundRect table", [&] { gfx_fillRoundRect(x, y, w, h, r, (uint8_t)c); }, [&](Fb &f) {
                for (int i = 0; i < r; i++) {
                    int in = INSET[r - 1][i];
                    refRect(f, x + in, y + i, w - 2 * in, 1, c);
                    refRect(f, x + in, y + h - 1 - i, w - 2 * in, 1, c);
                }
                refRect(f, x, y + r, w, h - 2 * r, c);
            });
        }

        int cx = rr(-20, 148), cy = rr(-20, 148), rx = rr(0, 70), ry = rr(0, 70);
        snprintf(s_case, sizeof s_case, "ellipse %d,%d r %d,%d", cx, cy, rx, ry);
        vsRef("fillEllipse", [&] { gfx_fillEllipse(cx, cy, rx, ry, (uint8_t)c); }, [&](Fb &f) {
            for (int dy = -ry; dy <= ry; dy++) {
                int a = dy < 0 ? -dy : dy, hw;
                if (!ry) hw = rx;
                else hw = isqrtRef((int)((int64_t)rx * rx * (ry * ry - a * a + ry / 2) / (ry * ry)));
                if (hw > rx) hw = rx;          /* the rounding term never widens it past 2rx+1 */
                refRect(f, cx - hw, cy + dy, 2 * hw + 1, 1, c);
            }
        });
        vsClip("fillEllipse", [&] { gfx_fillEllipse(cx, cy, rx, ry, (uint8_t)c); });
        vsClip("ellipse", [&] { gfx_ellipse(cx, cy, rx, ry, (uint8_t)c); });
        Fb ef = inked([&] { gfx_fillEllipse(cx, cy, rx, ry, 1); });
        Fb el = inked([&] { gfx_ellipse(cx, cy, rx, ry, 1); });
        bool esub = true, eedge = true;
        for (int j = 0; j < GFX_H; j++) for (int i = 0; i < GFX_W; i++) {
            bool f = ef[j * GFX_W + i], l = el[j * GFX_W + i];
            if (l && !f) esub = false;
            if (f && !l) {
                const int d[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
                for (auto &k : d) {
                    int ni = i + k[0], nj = j + k[1];
                    if (onScreen(ni, nj) && !ef[nj * GFX_W + ni]) eedge = false;
                }
            }
        }
        check(esub, "ellipse inside fillEllipse");
        check(eedge, "ellipse covers fillEllipse's edge");
    }
}

static void testRows() {
    for (int t = 0; t < 500; t++) {
        int y = rr(-20, 140), h = rr(-5, 150), dx = rr(-140, 140), dy = rr(-40, 40), fill = rr(-1, 15);
        if (t % 4 == 0) dx = rr(-3, 3);
        if (t % 7 == 0) dx = 0;
        snprintf(s_case, sizeof s_case, "rows %d+%d by %d,%d fill %d", y, h, dx, dy, fill);
        vsRef("scroll", [&] { gfx_scroll(y, h, dx, dy, fill); }, [&](Fb &f) {
            Fb old = f;
            int y0 = y < 0 ? 0 : y, y1 = y + h > GFX_H ? GFX_H : y + h;
            if (!dx && !dy) return;
            for (int j = y0; j < y1; j++) for (int i = 0; i < GFX_W; i++) {
                int si = i - dx, sj = j - dy;
                if (si >= 0 && si < GFX_W && sj >= y0 && sj < y1) f[j * GFX_W + i] = old[sj * GFX_W + si];
                else if (fill >= 0) f[j * GFX_W + i] = (uint8_t)fill;
            }
        });
        uint32_t pat[GFX_FB_STRIDE / 4];
        for (auto &w : pat) w = rnd();
        int x0 = rr(-20, 140), x1 = rr(-20, 150), row = rr(-5, 132);
        snprintf(s_case, sizeof s_case, "copyRow %d [%d,%d)", row, x0, x1);
        vsRef("copyRow", [&] { gfx_copyRow(row, (const uint8_t *)pat, x0, x1); }, [&](Fb &f) {
            const uint8_t *p = (const uint8_t *)pat;
            for (int i = x0; i < x1; i++)
                if (i >= 0 && i < GFX_W) rput(f, i, row, (i & 1) ? p[i >> 1] >> 4 : p[i >> 1] & 15);
        });
        vsClip("copyRow", [&] { gfx_copyRow(row, (const uint8_t *)pat, x0, x1); });
    }
}

static void testText() {
    const GFXfont *fonts[3] = { nullptr, &CHGfx_Sans12, &CHGfx_Tiny3x5 };
    for (int t = 0; t < 600; t++) {
        const GFXfont *font = fonts[t % 3];
        std::string s = randomText();
        int x = rr(-40, 130), y = rr(-20, 140), c = rr(0, 15), sc = rr(1, 3);
        snprintf(s_case, sizeof s_case, "font %d \"%s\" at %d,%d x%d", t % 3, s.c_str(), x, y, sc);
        gfx_setFont(font);
        vsRef("text", [&] { gfx_textScaled(x, y, s.c_str(), (uint8_t)c, (uint8_t)sc); },
              [&](Fb &f) { refText(f, font, x, y, s.c_str(), c, sc); });
        vsClip("text", [&] { gfx_textScaled(x, y, s.c_str(), (uint8_t)c, (uint8_t)sc); });

        /* textFx: fill = the plain text's pixels; outline = their 8-way
         * dilation; shadow = the outline shape moved (1,1). Painted
         * shadow, outline, fill. */
        int outline = rr(-1, 15), shadow = rr(-1, 15);
        bool useRamp = t % 4 == 0;
        std::vector<int8_t> dy(s.size() + 1, 0);
        if (t % 5 == 0) for (auto &d : dy) d = (int8_t)rr(-3, 3);
        int bx, by, bw, bh;
        gfx_textBounds(s.c_str(), x, y, (uint8_t)sc, &bx, &by, &bw, &bh);
        int dmin = 0, dmax = 0;
        for (size_t k = 0; k < s.size(); k++) { if (dy[k] < dmin) dmin = dy[k]; if (dy[k] > dmax) dmax = dy[k]; }
        std::vector<uint8_t> ramp(bh + dmax - dmin + 4);
        for (auto &v : ramp) v = (uint8_t)rr(0, 15);
        bool fits = ((bw + 2 + 7) / 8) <= 32 && ((bw + 2 + 7) / 8) * (bh + dmax - dmin + 2) <= 1024;
        auto fx = [&] {
            return gfx_textFx(x, y, s.c_str(), (uint8_t)sc, (uint8_t)c, outline, shadow,
                              useRamp ? ramp.data() : nullptr, (t % 5 == 0) ? dy.data() : nullptr);
        };
        snprintf(s_case, sizeof s_case, "fx font %d \"%s\" at %d,%d x%d o%d s%d%s%s", t % 3, s.c_str(), x, y, sc,
                 outline, shadow, useRamp ? " ramp" : "", (t % 5 == 0) ? " wavy" : "");
        Fb bg = randomFb();
        load(bg);
        bool ok = fx();
        check(ok == fits, "textFx reports whether the mask fits");
        if (!ok) { same(grab(), bg, "textFx draws nothing when it does not fit"); continue; }
        Fb got = grab();
        /* The plain text's pixels, character by character for the
         * offsets, on a canvas M px bigger than the screen all round. */
        const int M = 64, CW = GFX_W + 2 * M, CH = GFX_H + 2 * M;
        std::vector<uint8_t> m(CW * CH, 0);
        Put cput = [&](int i, int j) { i += M; j += M; if (i >= 0 && j >= 0 && i < CW && j < CH) m[j * CW + i] = 1; };
        {
            int px = x, py = y;
            for (size_t k = 0; k < s.size(); k++) {
                char one[2] = { s[k], 0 };
                if (s[k] == '\n') { px = x; py += (font ? font->yAdvance : 8) * sc; continue; }
                refTextPut(cput, font, px, py + dy[k], one, sc);
                px += gfx_textWidthScaled(one, (uint8_t)sc);
            }
        }
        std::vector<uint8_t> d(CW * CH, 0);
        for (int j = 1; j < CH - 1; j++) for (int i = 1; i < CW - 1; i++)
            if (m[j * CW + i]) for (int b = -1; b <= 1; b++) for (int a = -1; a <= 1; a++) d[(j + b) * CW + i + a] = 1;
        const std::vector<uint8_t> &shape = outline >= 0 ? d : m;
        Fb want = bg;
        int top = by + dmin;
        for (int j = 0; j < GFX_H; j++) for (int i = 0; i < GFX_W; i++) {
            int ci = i + M, cj = j + M;
            if (shadow >= 0 && shape[(cj - 1) * CW + ci - 1]) want[j * GFX_W + i] = (uint8_t)shadow;
            if (outline >= 0 && d[cj * CW + ci]) want[j * GFX_W + i] = (uint8_t)outline;
            if (m[cj * CW + ci]) want[j * GFX_W + i] = (uint8_t)(useRamp ? ramp[j - top] & 15 : c);
        }
        same(got, want, "textFx");
        vsClip("textFx", [&] { fx(); });
    }
    gfx_setFont(nullptr);
}

static void testTinyFont() {
    /* Every glyph of the GFXfont draws exactly its source columns: 3 wide
     * (4 for '~'), rows 0-4 above the baseline, row 5 below it. */
    gfx_setFont(&CHGfx_Tiny3x5);
    check(gfx_fontBaseline() == 5, "Tiny3x5 baseline is 5");
    check(gfx_fontLineHeight() == 7, "Tiny3x5 line is 7");
    for (int ch = 32; ch < 127; ch++) {
        char s[2] = { (char)ch, 0 };
        snprintf(s_case, sizeof s_case, "glyph '%c'", ch);
        vsRef("Tiny3x5 glyph", [&] { gfx_text(10, 20, s, 7); }, [&](Fb &f) {
            const TinyCols &g = TINY_COLS[ch - 32];
            for (int col = 0; col < g.n; col++)
                for (int row = 0; row < 6; row++)
                    if (g.c[col] & (1 << row)) rput(f, 10 + col, 20 - 5 + row, 7);
        });
        check(gfx_textWidth(s) == (ch == '~' ? 5 : 4), "Tiny3x5 advance");
    }
    gfx_setFont(nullptr);
}

static void testPalette() {
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) pal[i] = (uint16_t)rnd();
    gfx_setPalette(pal, 16);
    uint16_t out[16];
    gfx_setFade(0);
    check(gfx__paletteTake(out), "setPalette marks the palette dirty");
    check(!gfx__paletteTake(out), "taking it clears the flag");
    bool eq = true;
    for (int i = 0; i < 16; i++) if (out[i] != pal[i]) eq = false;
    check(eq, "fade 0 is the palette as set");
    gfx_setFade(0);
    check(!gfx__paletteTake(out), "setting the same fade again is not a change");
    gfx_setFade(255, 0xFFFF);
    gfx__paletteTake(out);
    eq = true;
    for (int i = 0; i < 16; i++) if (out[i] != 0xFFFF) eq = false;
    check(eq, "fade 255 to white is white");
    gfx_setFade(255);
    gfx__paletteTake(out);
    eq = true;
    for (int i = 0; i < 16; i++) if (out[i] != 0) eq = false;
    check(eq, "fade 255 to black is black");
    /* Monotonic, component by component, toward the target. */
    bool mono = true;
    for (int i = 0; i < 16; i++) {
        int prev[3] = { (pal[i] >> 11) & 31, (pal[i] >> 5) & 63, pal[i] & 31 };
        for (int a = 0; a <= 255; a += 5) {
            gfx_setFade((uint8_t)a);
            uint16_t o = gfx_paletteOut((uint8_t)i);
            int cur[3] = { (o >> 11) & 31, (o >> 5) & 63, o & 31 };
            for (int k = 0; k < 3; k++) if (cur[k] > prev[k]) mono = false;
            for (int k = 0; k < 3; k++) prev[k] = cur[k];
        }
    }
    check(mono, "fading to black never brightens");
    gfx_setFade(0);
    check(gfx_nearest(pal[5]) == 5 || pal[gfx_nearest(pal[5])] == pal[5], "nearest ignores the fade");
}

static void testFlushModel() {
    /* The simulator's flush must behave like the board's, or its tearing
     * reports mean nothing. */
    gfx_begin(GFX_DIV2, GFX_16BPP);
    gfx_clear(3);
    gfx_flushAsync();
    check(gfx_busy(), "async flush is busy");
    check(gfx_flushRow() == 4, "two 2-row chunks are converted before the DMA starts");
    int bugs = s_bugs;
    gfx_waitRow(64);
    check(gfx_flushRow() >= 64, "waitRow(64) returns with rows 0..63 free");
    gfx_setClip(0, 0, GFX_W, 64);
    gfx_clear(5);                              /* allowed: those rows are sent */
    gfx_resetClip();
    gfx_wait();
    check(s_bugs == bugs, "drawing above flushRow() is not a tear");
    check(!gfx_busy() && gfx_flushRow() == GFX_H, "idle after wait");

    gfx_flushAsync();
    gfx_fillRect(0, 100, 128, 4, 9);           /* not sent yet: a tear */
    gfx_wait();
    check(s_bugs == bugs + 1, "drawing below flushRow() is reported");

    bugs = s_bugs;
    gfx_flushAsync();
    gfx_chunkScratch()[10] = 1;
    gfx_wait();
    check(s_bugs == bugs + 1, "writing the scratch during a flush is reported");

    /* Staged palette: a change during the flush does not reach that frame. */
    bugs = s_bugs;
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) pal[i] = 0x0000;
    pal[3] = 0x07E0;
    gfx_setPalette(pal, 16);
    gfx_clear(3);
    gfx_flushAsync();
    gfx_setPaletteEntry(3, 0xF800);
    gfx_wait();
    check(sim_panel[64 * GFX_W + 64] == 0x00FF00, "palette change mid-flush waits for the next frame");
    gfx_flush();
    check(sim_panel[64 * GFX_W + 64] == 0xFF0000, "and lands on the next");
    check(s_bugs == bugs, "palette calls during a flush are not bugs");
}

int main() {
    testBasics();
    testBlit();
    testSprite4();
    testSprite4Rot();
    testShapes();
    testRows();
    testText();
    testTinyFont();
    testPalette();
    testFlushModel();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
