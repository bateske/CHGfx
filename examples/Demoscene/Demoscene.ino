/*
 * ============================================================
 *   S I L I C O N   D R E A M S
 *   a demo for the CHGame  --  CH32X035 @ 48 MHz + ST7735
 * ============================================================
 *
 * Seven parts, no floating point, no libm, no framebuffer for half of
 * it. Everything you see is computed per pixel per frame.
 *
 * THE TWO RENDERING MODES, AND WHY BOTH EXIST
 * -------------------------------------------
 * CHGfx normally keeps a 4 bpp, 16-colour framebuffer, because a full
 * RGB565 one is 32 KB and this chip has 20 KB. That is the right shape
 * for sprites, geometry and text - things that touch a few pixels and
 * leave the rest alone.
 *
 * It is the wrong shape for a plasma, where every pixel changes every
 * frame. So the procedural parts here use gfx_stream(): the effect
 * writes pixels straight into the buffer DMA is about to transmit,
 * never storing a frame anywhere. No framebuffer means no 16-colour
 * limit, so those parts run at the panel's FULL depth - 65536 colours
 * at 16 bpp, or 262144 at 18 bpp.
 *
 * That is a 4096x increase in colour depth over the library default,
 * at the same frame rate, on the same silicon.
 *
 * THE BUDGET
 * ----------
 * At 24 MHz the wire eats a 16 bpp pixel every 32 CPU cycles. That is
 * the entire per-pixel budget for a full-speed effect, and every inner
 * loop below is written to fit inside it: table lookups instead of
 * arithmetic, incremental indices instead of multiplies, and the hot
 * loops linked into SRAM because flash costs 3 wait states here.
 *
 * 16384 pixels x 90 fps = 1.5 million computed pixels per second, from
 * a 48 MHz RISC-V core with no FPU, no cache and no blitter.
 *
 * MEMORY
 * ------
 * The direct parts have no framebuffer, so gfx_fb's 8 KB is dead
 * weight during them - they borrow it as a scratch arena for lookup
 * tables. See `arena` below. The framebuffer parts use it normally.
 * Nothing else would fit; there are about 6 KB spare.
 *
 * Progress and per-part frame rates print over USB CDC.
 * Build with Tools > Optimize > Faster (-O2).
 */
#include <CHGfx.h>

/* Hot loops go in SRAM. Flash on this part is 3 wait states at 48 MHz
 * and measured 2.2x slower on exactly this kind of tight loop, which at
 * a 32-cycle pixel budget is the difference between 90 fps and 40. */
#define FX __attribute__((section(".srodata.ramfunc"), noinline))

/* ------------------------------------------------------------------ */
/* Scratch arena                                                       */
/* ------------------------------------------------------------------ */
/* gfx_fb is 8192 bytes of framebuffer that the streamed parts do not
 * use. Borrowing it is the only way the tunnel's tables fit. */
static uint8_t *const arena = gfx_fb;

/* ------------------------------------------------------------------ */
/* Integer trig - no libm anywhere in this sketch                      */
/* ------------------------------------------------------------------ */
/*
 * Pulling in sinf/cosf costs 8.5 KB of flash and, with no FPU, more
 * time per call than a whole scanline of plasma. A 64-entry quarter
 * table mirrored into 256 entries is exact enough and one load.
 */
static const uint8_t sinQuarter[64] = {   /* sin(i*pi/128) * 127 */
      0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,  40,
     43,  46,  49,  51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,
     81,  83,  85,  88,  90,  92,  94,  96,  98, 100, 102, 104, 106, 107,
    109, 111, 112, 113, 115, 116, 117, 118, 120, 121, 122, 122, 123, 124,
    125, 125, 126, 126, 126, 127, 127, 127
};

static int8_t sinTab[256];       /* full circle, -127..127 */

static void buildTrig(void)
{
    for (int i = 0; i < 256; i++) {
        int q = i & 63;
        int v = (i & 64) ? sinQuarter[63 - q] : sinQuarter[q];
        if (i & 128) v = -v;
        sinTab[i] = (int8_t)v;
    }
}

/* Unsigned sine, 0..254. A second 256-byte table would be a quarter of
 * a kilobyte to save one addition - not a good trade on a 20 KB part. */
#define SINU(a) ((int)sinTab[(uint8_t)(a)] + 127)

static inline int isin(uint8_t a) { return sinTab[a]; }
static inline int icos(uint8_t a) { return sinTab[(uint8_t)(a + 64)]; }

/* Integer square root, good to 1 LSB. Used once per table build. */
static uint16_t isqrt32(uint32_t n)
{
    uint32_t res = 0, bit = 1UL << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else                {                 res =  res >> 1; }
        bit >>= 2;
    }
    return (uint16_t)res;
}

/*
 * atan2 by brute force against the sine table: find the angle whose
 * direction vector is most parallel to (x,y), maximising the dot
 * product. 64 candidates per call, and it only ever runs while a table
 * is being built, so a smarter CORDIC would buy nothing.
 */
static uint8_t iatan2_q1(int x, int y)          /* x,y >= 0 -> 0..64 */
{
    int32_t bestDot = -0x7FFFFFFF;
    uint8_t best = 0;
    for (int a = 0; a <= 64; a++) {
        /* Parallel means the perpendicular component vanishes, so
         * minimise |y*cos - x*sin|. */
        int32_t perp = (int32_t)y * icos((uint8_t)a) - (int32_t)x * isin((uint8_t)a);
        if (perp < 0) perp = -perp;
        if (-perp > bestDot) { bestDot = -perp; best = (uint8_t)a; }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Colour                                                              */
/* ------------------------------------------------------------------ */
static uint16_t pal256[256];     /* cyclic RGB565 ramp, 512 B of RAM  */
static uint8_t  fadeLevel = 255; /* 0 = black, 255 = full             */

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/*
 * Three phase-shifted sines make a hue wheel that joins seamlessly at
 * both ends, which is what lets a plasma index wrap without a visible
 * seam. `spread` pulls the phases together for a duotone look.
 */
static void buildPal(uint8_t phase, uint8_t spread, uint8_t bright)
{
    for (int i = 0; i < 256; i++) {
        uint8_t a = (uint8_t)(i + phase);
        int r = SINU(a);
        int g = SINU(a + spread);
        int b = SINU(a + 2 * spread);
        pal256[i] = rgb565(r * bright >> 8, g * bright >> 8, b * bright >> 8);
    }
}

/* Fire: black -> red -> orange -> yellow -> white, not cyclic. */
static void buildFirePal(uint8_t bright)
{
    for (int i = 0; i < 256; i++) {
        int r = i * 4;      if (r > 255) r = 255;
        int g = (i - 64) * 4; if (g < 0) g = 0; if (g > 255) g = 255;
        int b = (i - 160) * 6; if (b < 0) b = 0; if (b > 255) b = 255;
        pal256[i] = rgb565(r * bright >> 8, g * bright >> 8, b * bright >> 8);
    }
}

/* 16-entry framebuffer palettes for the buffered parts. */
static void setRampPalette(uint16_t lo, uint16_t hi, uint8_t bright)
{
    uint16_t p[16];
    int r0 = (lo >> 11) << 3, g0 = ((lo >> 5) & 0x3F) << 2, b0 = (lo & 0x1F) << 3;
    int r1 = (hi >> 11) << 3, g1 = ((hi >> 5) & 0x3F) << 2, b1 = (hi & 0x1F) << 3;
    for (int i = 0; i < 16; i++) {
        int r = r0 + (r1 - r0) * i / 15;
        int g = g0 + (g1 - g0) * i / 15;
        int b = b0 + (b1 - b0) * i / 15;
        p[i] = rgb565(r * bright >> 8, g * bright >> 8, b * bright >> 8);
    }
    gfx_setPalette(p, 16);
}

/* ================================================================== */
/* PART 1 - PLASMA, 65536 colours, no framebuffer                      */
/* ================================================================== */
/*
 * Four sine terms per pixel. The trick that makes it fit the budget is
 * that none of them needs a multiply inside the loop:
 *
 *   term A  depends on x only          -> precomputed into rowX[] once
 *                                         per frame, 128 bytes
 *   term B  depends on y only          -> a scalar, hoisted per row
 *   term C  is sin(x + y + t)          -> index walks +1 with x
 *   term D  is sin(x - y + t)          -> index also walks +1 with x,
 *                                         but with a different y phase
 *
 * C and D share the +1 step yet disagree on y, which is what keeps the
 * pattern genuinely two-dimensional instead of a set of diagonal
 * stripes. Sum them, let the byte wrap, and index a cyclic palette:
 * the wrap is invisible precisely because the palette joins up.
 *
 * Cost: 3 loads, 3 adds, a mask, a palette load and a store. Roughly
 * 14 cycles against a budget of 32.
 */
static uint8_t plasRowX[GFX_W];
static uint8_t plasT0, plasT1, plasT2, plasT3;

FX static void plasmaRows(uint8_t *dst, int y0, int rows, void *user)
{
    (void)user;
    uint16_t *d = (uint16_t *)dst;
    const int8_t  *st = sinTab;
    const uint16_t *pal = pal256;
    const uint8_t *rx = plasRowX;

    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        int sy = st[(uint8_t)(y * 3 + plasT1)];
        uint8_t k = (uint8_t)(y + plasT2);          /* x + y + t */
        uint8_t m = (uint8_t)(-y + plasT3);         /* x - y + t */
        for (int x = 0; x < GFX_W; x++) {
            int v = (int)rx[x] + sy + st[k++] + st[m++];
            *d++ = pal[(uint8_t)v];
        }
    }
}

/*
 * The same plasma at 18 bpp, generating RGB666 directly from the sine
 * table instead of going through a 256-entry palette.
 *
 * This is the part that answers "how deep can the pixels go". At 16 bpp
 * a smooth ramp has 32 red steps and 32 blue steps and you can see
 * every one of them as a band. At 18 bpp there are 64 of each, and the
 * colour is computed per pixel rather than quantised into 256 palette
 * slots first - so the gradients are genuinely continuous.
 *
 * It costs 3 bytes per pixel instead of 2: 49152 bytes a frame, 16.4 ms
 * on the wire, 61 fps instead of 90. Two thirds of the frame rate for
 * one more bit of red and one more of blue. Judge for yourself.
 */
FX static void plasmaRows18(uint8_t *dst, int y0, int rows, void *user)
{
    (void)user;
    const int8_t *st = sinTab;
    const int fade = fadeLevel;
    const uint8_t *rx = plasRowX;

    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        int sy = st[(uint8_t)(y * 3 + plasT1)];
        uint8_t k = (uint8_t)(y + plasT2);
        uint8_t m = (uint8_t)(-y + plasT3);
        for (int x = 0; x < GFX_W; x++) {
            uint8_t v = (uint8_t)((int)rx[x] + sy + st[k++] + st[m++]);
            /* Hue wheel straight to 6-bit components, no palette. */
            /* fadeLevel is applied here rather than through a palette,
             * because at 18 bpp there is no palette to fade. */
            dst[0] = (uint8_t)((SINU(v)      * fade >> 8) & 0xFC);
            dst[1] = (uint8_t)((SINU(v + 85) * fade >> 8) & 0xFC);
            dst[2] = (uint8_t)((SINU(v + 170)* fade >> 8) & 0xFC);
            dst += 3;
        }
    }
}

static void plasmaTick(void)
{
    plasT0 += 2; plasT1 += 1; plasT2 -= 1; plasT3 += 3;
    for (int x = 0; x < GFX_W; x++)
        plasRowX[x] = (uint8_t)sinTab[(uint8_t)(x * 2 + plasT0)];
}

/* ================================================================== */
/* PART 2 - TUNNEL, textured and fogged                                */
/* ================================================================== */
/*
 * The classic. For every pixel you need the polar coordinates of that
 * point: distance from centre and angle around it. Both are fixed for a
 * given screen position, so you precompute them once and then the frame
 * loop is two table reads and a palette lookup.
 *
 * A full 128x128 table of two bytes per pixel would be 32 KB. But the
 * screen is symmetric about both axes through the centre, so only one
 * quadrant needs storing: 64x64 for distance and 64x64 for angle, 8192
 * bytes exactly - which is why this borrows gfx_fb.
 *
 * Distance mirrors trivially. Angle needs a per-quadrant fix-up:
 *      right-down  a          left-down  128 - a
 *      right-up   -a          left-up    128 + a
 * All four are hoisted out of the inner loop, so there is no branching
 * per pixel: each row is walked as two runs of 64, one with the table
 * pointer going forward and the destination going backward.
 *
 * Fog comes free. The palette is built as two 128-entry brightness
 * ramps, one per checker colour, indexed by raw distance - so the far
 * end of the tunnel darkens without a single multiply.
 */
static uint8_t *const tunDist = arena;              /* 64*64 */
static uint8_t *const tunAng  = arena + 64 * 64;    /* 64*64 */
static uint8_t tunT1, tunT2;

static void tunnelInit(void)
{
    for (int yi = 0; yi < 64; yi++) {
        for (int xi = 0; xi < 64; xi++) {
            /* Centre sits between pixels 63 and 64, so the half-pixel
             * offset keeps the mirroring exact and avoids a divide by
             * zero at the vanishing point. */
            int dx = xi * 2 + 1;          /* 2x scale, always odd */
            int dy = yi * 2 + 1;
            uint32_t r2 = (uint32_t)dx * dx + (uint32_t)dy * dy;
            uint16_t r  = isqrt32(r2);    /* 2x scale too */
            if (r < 4) r = 4;

            /* Perspective: distance along the tunnel goes as 1/r. */
            uint32_t d = 24000u / r;
            if (d > 255) d = 255;
            /* Store so that BIG value = NEAR, which makes the palette
             * ramp read as brightness directly. */
            tunDist[yi * 64 + xi] = (uint8_t)d;
            tunAng[yi * 64 + xi]  = iatan2_q1(dx, dy);
        }
    }
}

static void buildTunnelPal(uint8_t phase, uint8_t bright)
{
    /* Two 128-entry ramps: [0..127] checker A, [128..255] checker B. */
    int ra = SINU(phase),       ga = SINU(phase + 85),  ba = SINU(phase + 170);
    int rb = SINU(phase + 40),  gb = SINU(phase + 125), bb = SINU(phase + 210);
    for (int i = 0; i < 128; i++) {
        int f = i * 2;                       /* 0..254 fog */
        int s = f * bright >> 8;
        pal256[i]       = rgb565(ra * s >> 8, ga * s >> 8, ba * s >> 8);
        pal256[i + 128] = rgb565((rb * s >> 8) >> 1, (gb * s >> 8) >> 1,
                                 (bb * s >> 8) >> 1);
    }
}

/* One run of 64 pixels. `step` is +1 or -1 on the destination. */
static inline void tunnelRun(uint16_t *d, int step, const uint8_t *dp,
                             const uint8_t *ap, int angMul, int angAdd)
{
    const uint16_t *pal = pal256;
    uint8_t t1 = tunT1, t2 = tunT2;
    for (int i = 0; i < 64; i++) {
        uint8_t dist = dp[i];
        uint8_t ang  = (uint8_t)(angMul * ap[i] + angAdd);
        uint8_t u    = (uint8_t)(ang  + t1);
        uint8_t v    = (uint8_t)(dist + t2);
        /* Checkerboard in tunnel space picks which of the two ramps. */
        uint8_t chk  = (uint8_t)(((u >> 4) ^ (v >> 4)) & 1);
        *d = pal[(dist >> 1) | (chk << 7)];
        d += step;
    }
}

FX static void tunnelRows(uint8_t *dst, int y0, int rows, void *user)
{
    (void)user;
    uint16_t *d = (uint16_t *)dst;
    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        int yi   = (y >= 64) ? (y - 64) : (63 - y);
        int down = (y >= 64);
        const uint8_t *dp = tunDist + yi * 64;
        const uint8_t *ap = tunAng  + yi * 64;

        /* Right half, x = 64..127: table walks forward with x. */
        tunnelRun(d + 64, +1, dp, ap, down ? 1 : -1, 0);
        /* Left half, x = 63..0: table still walks forward, screen
         * walks backward, and the angle reflects about the vertical. */
        tunnelRun(d + 63, -1, dp, ap, down ? -1 : 1, 128);
        d += GFX_W;
    }
}

/* ================================================================== */
/* PART 3 - ROTOZOOMER                                                 */
/* ================================================================== */
/*
 * A texture rotated and scaled by walking it in fixed point. The whole
 * effect is an affine map, so the per-pixel work is two additions:
 * stepping one pixel right always moves the same (du, dv) through the
 * texture, whatever the rotation. Everything expensive - the sin and
 * cos, the scale - happens once per frame.
 *
 * 8.8 fixed point, 64x64 texture, wrapping by mask. Roughly 10 cycles
 * a pixel, so this one is limited purely by the wire.
 */
static uint8_t *const rotoTex = arena;              /* 64*64 = 4096 */
static int32_t rotoDuX, rotoDvX, rotoDuY, rotoDvY, rotoU0, rotoV0;

static void rotoInit(void)
{
    /* A texture with structure at several scales, so both the rotation
     * and the zoom are readable. */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint8_t v = (uint8_t)((x ^ y) * 4);
            if (((x & 7) == 0) || ((y & 7) == 0)) v = 255;
            if (((x >> 3) ^ (y >> 3)) & 1) v = (uint8_t)(v >> 1);
            rotoTex[y * 64 + x] = v;
        }
}

static void rotoSetup(uint8_t ang, int zoom /* 8.8 */)
{
    int c = icos(ang) * zoom >> 7;      /* cos * zoom, 8.8-ish */
    int s = isin(ang) * zoom >> 7;
    rotoDuX =  c; rotoDvX = s;
    rotoDuY = -s; rotoDvY = c;
    /* Put the origin in the middle of the screen. */
    rotoU0 = (32 << 8) - (rotoDuX * (GFX_W / 2) + rotoDuY * (GFX_H / 2));
    rotoV0 = (32 << 8) - (rotoDvX * (GFX_W / 2) + rotoDvY * (GFX_H / 2));
}

FX static void rotoRows(uint8_t *dst, int y0, int rows, void *user)
{
    (void)user;
    uint16_t *d = (uint16_t *)dst;
    const uint8_t *tex = rotoTex;
    const uint16_t *pal = pal256;

    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        int32_t u = rotoU0 + rotoDuY * y;
        int32_t v = rotoV0 + rotoDvY * y;
        for (int x = 0; x < GFX_W; x++) {
            uint32_t tu = ((uint32_t)u >> 8) & 63;
            uint32_t tv = ((uint32_t)v >> 8) & 63;
            *d++ = pal[tex[(tv << 6) | tu]];
            u += rotoDuX;
            v += rotoDvX;
        }
    }
}

/* ================================================================== */
/* PART 4 - FIRE                                                       */
/* ================================================================== */
/*
 * The 1993 classic: seed the bottom row with noise, then every frame
 * each cell becomes the average of the three below it minus a decay,
 * and the heat crawls upward on its own.
 *
 * The simulation buffer is 128 wide by 64 tall - full horizontal
 * resolution, half vertical, which is 8192 bytes and therefore exactly
 * the whole arena. It is doubled vertically on the way out, and that
 * costs nothing because the streamer emits two identical rows from one
 * source row.
 *
 * Simulation is about 8 cycles a cell over 8192 cells, so under a
 * millisecond - it disappears into the 11 ms the frame spends on the
 * wire.
 */
#define FIRE_W GFX_W
#define FIRE_H 64
static uint8_t *const fire = arena;
static uint32_t fireRnd = 0x1234567u;

static inline uint32_t xorshift(void)
{
    fireRnd ^= fireRnd << 13;
    fireRnd ^= fireRnd >> 17;
    fireRnd ^= fireRnd << 5;
    return fireRnd;
}

FX static void fireStep(uint8_t cool)
{
    /* Bottom row: a moving band of embers rather than uniform noise,
     * so the flames wander instead of sitting still. */
    uint8_t *bot = fire + (FIRE_H - 1) * FIRE_W;
    for (int x = 0; x < FIRE_W; x++)
        bot[x] = (uint8_t)((xorshift() & 0x7F) + 128);

    for (int y = FIRE_H - 2; y >= 0; y--) {
        uint8_t *row = fire + y * FIRE_W;
        const uint8_t *bel = row + FIRE_W;
        for (int x = 1; x < FIRE_W - 1; x++) {
            int v = (bel[x - 1] + bel[x] + bel[x + 1] + bel[x]) >> 2;
            v -= (int)(xorshift() & cool);
            row[x] = (uint8_t)(v < 0 ? 0 : v);
        }
        row[0] = 0;
        row[FIRE_W - 1] = 0;
    }
}

FX static void fireRows(uint8_t *dst, int y0, int rows, void *user)
{
    (void)user;
    uint16_t *d = (uint16_t *)dst;
    const uint16_t *pal = pal256;
    for (int r = 0; r < rows; r++) {
        const uint8_t *src = fire + ((y0 + r) >> 1) * FIRE_W;
        for (int x = 0; x < GFX_W; x++) *d++ = pal[src[x]];
    }
}

/* ================================================================== */
/* PART 5 - STARFIELD (framebuffer)                                    */
/* ================================================================== */
/*
 * Back to the 4 bpp framebuffer, because a starfield touches a few
 * hundred pixels and leaves 16000 alone - streaming would mean
 * computing every background pixel just to write black to it.
 *
 * Perspective divide per star per frame. 192 divides at 48 MHz is
 * nothing; the point is that the brightness comes free from the same
 * z, so near stars are white and far ones are barely there.
 */
#define NSTARS 128
static int16_t starX[NSTARS], starY[NSTARS], starZ[NSTARS];

static void starsInit(void)
{
    for (int i = 0; i < NSTARS; i++) {
        starX[i] = (int16_t)((xorshift() & 2047) - 1024);
        starY[i] = (int16_t)((xorshift() & 2047) - 1024);
        starZ[i] = (int16_t)((xorshift() & 1023) + 1);
    }
}

static void starsFrame(int speed)
{
    gfx_clear(0);
    for (int i = 0; i < NSTARS; i++) {
        starZ[i] = (int16_t)(starZ[i] - speed);
        if (starZ[i] < 1) {
            starZ[i] = 1023;
            starX[i] = (int16_t)((xorshift() & 2047) - 1024);
            starY[i] = (int16_t)((xorshift() & 2047) - 1024);
        }
        int z = starZ[i];
        int sx = 64 + (starX[i] * 96) / (z + 32);
        int sy = 64 + (starY[i] * 96) / (z + 32);
        if ((unsigned)sx >= GFX_W || (unsigned)sy >= GFX_H) continue;
        int shade = 15 - (z >> 6);
        if (shade < 1) shade = 1;
        gfx_pixel(sx, sy, (uint8_t)shade);
        /* Near stars get a second pixel so they read as streaks. */
        if (z < 200) gfx_pixel(sx + 1, sy, (uint8_t)shade);
    }
}

/* ================================================================== */
/* PART 6 - SHADED SOLID (framebuffer)                                 */
/* ================================================================== */
/*
 * A rotating icosahedron, flat shaded, filled triangles, backface
 * culled. It is convex, so culling alone gives the right answer and no
 * depth sort is needed.
 *
 * This is the part that leans on the CPU rather than the wire: 12
 * vertices through a 3x3 rotation, 20 face normals, 20 lighting dot
 * products and a scanline fill, every frame. The fill runs through
 * gfx_hline, which paints 8 pixels per 32-bit store because the
 * framebuffer is 4 bpp - a paletted buffer is not just smaller, it is
 * genuinely faster to fill.
 */
static const int8_t icoV[12][3] = {
    {-61, 100,   0}, { 61, 100,   0}, {-61,-100,   0}, { 61,-100,   0},
    {  0, -61, 100}, {  0,  61, 100}, {  0, -61,-100}, {  0,  61,-100},
    {100,   0, -61}, {100,   0,  61}, {-100,  0, -61}, {-100,  0,  61}
};
static const uint8_t icoF[20][3] = {
    {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
    {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
    {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
    {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
};
static int16_t vx[12], vy[12], vz[12];

/* Flat-shaded triangle: sort by y, walk both edges, one hline a row. */
static void triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t c)
{
    int t;
    if (y0 > y1) { t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
    if (y0 > y2) { t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; }
    if (y1 > y2) { t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
    if (y2 == y0) return;

    for (int y = y0; y <= y2; y++) {
        if ((unsigned)y >= GFX_H) continue;
        /* Long edge y0->y2, short edge y0->y1 then y1->y2. */
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1) {
            if (y1 == y0) continue;
            xb = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        } else {
            if (y2 == y1) continue;
            xb = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        }
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        gfx_hline(xa, y, xb - xa + 1, c);
    }
}

static void solidFrame(uint8_t ax, uint8_t ay, int scale)
{
    int ca = icos(ax), sa = isin(ax);
    int cb = icos(ay), sb = isin(ay);

    for (int i = 0; i < 12; i++) {
        int x = icoV[i][0], y = icoV[i][1], z = icoV[i][2];
        /* Rotate about X, then about Y. 7-bit fixed point. */
        int y1 = (y * ca - z * sa) >> 7;
        int z1 = (y * sa + z * ca) >> 7;
        int x1 = (x * cb + z1 * sb) >> 7;
        int z2 = (-x * sb + z1 * cb) >> 7;
        vx[i] = (int16_t)(64 + (x1 * scale >> 8));
        vy[i] = (int16_t)(64 + (y1 * scale >> 8));
        vz[i] = (int16_t)z2;
    }

    /*
     * Painter's algorithm: sort faces back to front and draw all 20.
     *
     * Backface culling would be half the fill, but it depends on the
     * winding of the face table matching the handedness of the screen
     * (y grows downward here), and getting that sign backwards renders
     * the solid inside-out in a way that still looks plausible at a
     * glance. Sorting is correct whichever way the table winds, and for
     * a convex solid it gives exactly the same silhouette. 20 faces is
     * an insertion sort nobody can measure.
     */
    int order[20], depth[20];
    for (int f = 0; f < 20; f++) {
        order[f] = f;
        depth[f] = (vz[icoF[f][0]] + vz[icoF[f][1]] + vz[icoF[f][2]]);
    }
    for (int i = 1; i < 20; i++) {
        int k = order[i], d = depth[k], j = i - 1;
        while (j >= 0 && depth[order[j]] > d) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }

    gfx_clear(0);
    for (int i = 0; i < 20; i++) {
        int f = order[i];
        int a = icoF[f][0], b = icoF[f][1], c = icoF[f][2];
        /* Nearer faces are brighter, which for a convex solid lit from
         * the camera is what flat shading would give you anyway. */
        int shade = 8 + (depth[f] / 3 >> 5);
        if (shade < 1)  shade = 1;
        if (shade > 15) shade = 15;
        triangle(vx[a], vy[a], vx[b], vy[b], vx[c], vy[c], (uint8_t)shade);
    }
}

/* ================================================================== */
/* PART 7 - COPPER BARS + SCROLLER (hybrid)                            */
/* ================================================================== */
/*
 * The finale uses both halves of the library at once, which is the
 * thing neither mode can do alone.
 *
 * The background is a copper bar stack: a per-scanline colour computed
 * from three sines, at full 16-bit depth, generated on the fly with no
 * framebuffer. On an Amiga this was a coprocessor rewriting the palette
 * between scanlines; here it is just arithmetic in the stream callback,
 * and it is not limited to 16 colours or even to one colour per line.
 *
 * The text is drawn into the 4 bpp framebuffer with the normal library
 * calls, and the stream callback reads it back as a stencil: non-zero
 * means text, zero means show the copper. So the geometry engine and
 * the procedural renderer composite in a single pass, at wire speed.
 */
static const char *scrollMsg =
    "   SILICON DREAMS   ...   CH32X035 AT 48 MEGAHERTZ   ...   "
    "SIXTY TWO KILOBYTES OF FLASH   ...   TWENTY KILOBYTES OF RAM   ...   "
    "NO FPU   ...   NO CACHE   ...   NO BLITTER   ...   "
    "ONE POINT FIVE MILLION COMPUTED PIXELS PER SECOND   ...   "
    "SIXTY FIVE THOUSAND COLOURS   ...   GREETINGS TO EVERYONE STILL "
    "COUNTING CYCLES   ...   ";

static int scrollX;
static uint8_t copT;

static void scrollPrepare(void)
{
    /* Text into the framebuffer; the streamer uses it as a stencil. */
    gfx_clear(0);
    int len = 0; while (scrollMsg[len]) len++;
    int cw = 6 * 3;                                /* 5x7 at scale 3 */
    int first = scrollX / cw;
    int off   = -(scrollX % cw);
    for (int i = 0; i < 10; i++) {
        int ci = first + i;
        if (ci >= len) ci -= len;
        char ch = scrollMsg[ci];
        char s[2] = { ch, 0 };
        gfx_textScaled(off + i * cw, 52, s, 15, 3);
    }
}

FX static void copperRows(uint8_t *dst, int y0, int rows, void *user)
{
    (void)user;
    uint16_t *d = (uint16_t *)dst;
    const uint8_t *fb = gfx_fb;

    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        /* Three sines of different rates give a stack of bars that
         * drift through each other instead of marching in step. */
        uint8_t a = (uint8_t)(y * 3 + copT);
        uint8_t b = (uint8_t)(y * 5 - copT * 2);
        uint8_t c = (uint8_t)(y * 2 + copT * 3);
        int rr = (SINU(a)       + SINU(b + 60))  >> 1;
        int gg = (SINU(a + 85)  + SINU(c))       >> 1;
        int bb = (SINU(a + 170) + SINU(c + 128)) >> 1;
        rr = rr * fadeLevel >> 8;
        gg = gg * fadeLevel >> 8;
        bb = bb * fadeLevel >> 8;
        uint16_t bg = rgb565(rr, gg, bb);

        /* Text stencil: white core, and the bar colour inverted at the
         * edges so the letters stay legible over any background. */
        uint16_t fgA = rgb565(255 * fadeLevel >> 8, 255 * fadeLevel >> 8,
                              255 * fadeLevel >> 8);
        uint16_t fgB = rgb565((255 - rr) * fadeLevel >> 8,
                              (255 - gg) * fadeLevel >> 8,
                              (255 - bb) * fadeLevel >> 8);

        const uint8_t *frow = fb + y * GFX_FB_STRIDE;
        for (int x = 0; x < GFX_W; x += 2) {
            uint8_t pair = frow[x >> 1];
            d[x]     = (pair & 0x0F) ? ((y & 2) ? fgA : fgB) : bg;
            d[x + 1] = (pair & 0xF0) ? ((y & 2) ? fgA : fgB) : bg;
        }
        d += GFX_W;
    }
}

/* ================================================================== */
/* Sequencer                                                           */
/* ================================================================== */
static uint32_t partStart;
static uint8_t  part = 0;
static uint32_t partFrames;

static const char *partName[] = {
    "1 starfield          (framebuffer, 16 col)",
    "2 plasma             (direct, 65536 col)",
    "3 plasma 18bpp       (direct, 262144 col)",
    "4 tunnel             (direct, 65536 col)",
    "5 rotozoomer         (direct, 65536 col)",
    "6 fire               (direct, 65536 col)",
    "7 shaded solid       (framebuffer, 16 col)",
    "8 copper + scroller  (hybrid)"
};
static const uint16_t partMs[] = { 7000, 9000, 6000, 10000, 8000, 8000, 9000, 12000 };
#define NPARTS 8

/* Fade in over the first 600 ms of a part and out over the last 600. */
static uint8_t fadeFor(uint32_t el, uint16_t dur)
{
    if (el < 600)        return (uint8_t)(el * 255 / 600);
    if (el > dur - 600u) return (uint8_t)((dur - el) * 255 / 600);
    return 255;
}

static void startPart(uint8_t p)
{
    if (partFrames) {
        uint32_t ms = millis() - partStart;
        Serial.print("  ");
        Serial.print(partName[part]);
        Serial.print("   ");
        Serial.print(partFrames * 1000u / (ms ? ms : 1));
        Serial.println(" fps");
    }
    part = p;
    partStart = millis();
    partFrames = 0;

    switch (p) {
        case 0: gfx_setColorMode(GFX_16BPP); starsInit();  break;
        case 1: gfx_setColorMode(GFX_16BPP);               break;
        case 2: gfx_setColorMode(GFX_18BPP);               break;
        case 3: gfx_setColorMode(GFX_16BPP); tunnelInit(); break;
        case 4: gfx_setColorMode(GFX_16BPP); rotoInit();   break;
        case 5: gfx_setColorMode(GFX_16BPP);
                for (int i = 0; i < FIRE_W * FIRE_H; i++) fire[i] = 0;
                break;
        case 6: gfx_setColorMode(GFX_16BPP);               break;
        case 7: gfx_setColorMode(GFX_16BPP); scrollX = 0;  break;
    }
}

void setup()
{
    Serial.begin();
    Serial.waitForPC(3000);

    buildTrig();
    gfx_begin(GFX_DIV2, GFX_16BPP);

    Serial.println();
    Serial.println("=========================================");
    Serial.println("  SILICON DREAMS");
    Serial.println("  CH32X035 @ 48 MHz + ST7735 128x128");
    Serial.println("=========================================");
    Serial.println("part                                       rate");

    partFrames = 0;
    startPart(0);
}

/*
 * Serial control, because a demo you cannot pause is a demo you cannot
 * debug. '1'..'8' jump to a part and hold it there; ' ' or 'r' resumes
 * the sequence; 'n' steps to the next part.
 */
static bool holding = false;

static void pollSerial(void)
{
    while (Serial.available()) {
        int c = Serial.read();
        if (c >= '1' && c <= '0' + NPARTS) {
            startPart((uint8_t)(c - '1'));
            holding = true;
            Serial.print("holding part ");
            Serial.println(c - '0');
        } else if (c == 'n') {
            startPart((uint8_t)((part + 1) % NPARTS));
        } else if (c == ' ' || c == 'r') {
            holding = false;
            Serial.println("running");
        }
    }
}

void loop()
{
    pollSerial();

    uint32_t el = millis() - partStart;
    if (holding) {
        /* Freeze the clock just short of the fade-out so a held part
         * stays at full brightness and keeps animating. */
        if (el > partMs[part] - 700u) { partStart = millis() - 700; el = 700; }
    } else if (el >= partMs[part]) {
        startPart((uint8_t)((part + 1) % NPARTS));
        el = 0;
    }
    fadeLevel = fadeFor(el, partMs[part]);
    uint8_t t = (uint8_t)(el >> 4);

    switch (part) {
        case 0:                                   /* starfield */
            setRampPalette(0x0000, 0xFFFF, fadeLevel);
            starsFrame(6);
            gfx_textScaled(14, 8, "SILICON", 14, 2);
            gfx_textScaled(20, 26, "DREAMS", 12, 2);
            gfx_text(10, 112, "CH32X035 . 48MHZ", 9);
            gfx_flush();
            break;

        case 1:                                   /* plasma 16 bpp */
            buildPal((uint8_t)(el >> 5), 85, fadeLevel);
            plasmaTick();
            gfx_stream(plasmaRows, nullptr);
            break;

        case 2:                                   /* plasma 18 bpp */
            plasmaTick();
            gfx_stream(plasmaRows18, nullptr);
            break;

        case 3:                                   /* tunnel */
            buildTunnelPal((uint8_t)(el >> 6), fadeLevel);
            tunT1 = (uint8_t)(t * 2);
            tunT2 = (uint8_t)(-t * 3);
            gfx_stream(tunnelRows, nullptr);
            break;

        case 4:                                   /* rotozoomer */
            buildPal((uint8_t)(el >> 6), 60, fadeLevel);
            rotoSetup(t, 200 + (isin((uint8_t)(t * 2)) * 3) / 2);
            gfx_stream(rotoRows, nullptr);
            break;

        case 5:                                   /* fire */
            buildFirePal(fadeLevel);
            fireStep(3);
            gfx_stream(fireRows, nullptr);
            break;

        case 6:                                   /* shaded solid */
            setRampPalette(0x0008, 0xFFFF, fadeLevel);
            solidFrame(t, (uint8_t)(t * 2 / 3), 150);
            gfx_text(24, 116, "20 TRIS FLAT LIT", 6);
            gfx_flush();
            break;

        case 7:                                   /* copper + scroller */
            copT = t;
            scrollX += 3;
            {   int len = 0; while (scrollMsg[len]) len++;
                if (scrollX > len * 18) scrollX = 0; }
            scrollPrepare();
            gfx_stream(copperRows, nullptr);
            break;
    }
    partFrames++;
}
