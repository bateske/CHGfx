/*
 * chsim_gfx.cpp - the transport half of CHGfx, on a PC.
 *
 * Replaces src/CHGfx.cpp, the one file that talks to SPI and DMA. All the
 * drawing code, the palette and the fade are the library's own files,
 * compiled unmodified.
 *
 * A flush here behaves like the board's in time: the wire takes what the
 * benchmark measured, rows are converted a chunk at a time as the DMA
 * would convert them, and gfx_flushRow() / gfx_waitRow() report the same
 * progress. Each row lands on the simulated panel when it is converted,
 * with whatever the framebuffer held at that moment - so the simulator
 * can catch the classic bugs exactly:
 *
 *   * drawing into rows an async flush has not sent yet (tearing),
 *   * using gfx_chunkScratch() while a flush is in flight.
 */
#include <CHGfx.h>
#include "../../../src/CHGfx_internal.h"
#include "sim.h"

uint8_t gfx_fb[GFX_FB_BYTES] __attribute__((aligned(4)));
CHGfx Gfx;
uint32_t sim_panel[GFX_W * GFX_H];

static uint8_t s_mode = GFX_16BPP, s_div = GFX_DIV2;
static uint8_t s_scratch[2 * GFX_CHUNK_BYTES] __attribute__((aligned(4)));

/* Measured on the board (benchmark-results.txt): 2947 KB/s through the
 * whole pipeline at 24 MHz, and ~45 us of setup per flush. */
static const double   WIRE_BYTES_PER_US = 2.947;
static const uint32_t FLUSH_SETUP_US    = 45;

static double wireRate() { return WIRE_BYTES_PER_US * 2.0 / (s_div ? s_div : 2); }

/* ------------------------------------------------------------------ */
/* Colour, as the panel shows it                                       */
/* ------------------------------------------------------------------ */
static uint32_t panelColour(uint16_t c) {
    uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    if (s_mode == GFX_12BPP) {                 /* the LUT keeps the top 4 bits */
        r >>= 1; g >>= 2; b >>= 1;
        return (r * 17) << 16 | (g * 17) << 8 | (b * 17);
    }
    return ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
}

/* ------------------------------------------------------------------ */
/* The flush in flight                                                 */
/* ------------------------------------------------------------------ */
struct Job {
    bool     active;
    int      x, y, w, h;          /* aligned outward, as the board does */
    int      rowsPer, chunks;
    uint32_t t0;                  /* wire starts */
    double   chunkUs;
    uint32_t end;
    int      captured;            /* rows of the rect on the panel so far */
    bool     tore, scratchBug;
    uint32_t pal[16];
    uint8_t  snap[GFX_FB_BYTES];  /* the framebuffer the frame promised to show */
};
static Job s_job;
static const uint8_t CANARY = 0xA5;

static void landRow(int r) {
    const uint8_t *row = gfx_fb + r * GFX_FB_STRIDE;
    const uint8_t *was = s_job.snap + r * GFX_FB_STRIDE;
    for (int x = s_job.x; x < s_job.x + s_job.w; x++) {
        uint8_t b = row[x >> 1];
        uint8_t v = (x & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 15);
        sim_panel[r * GFX_W + x] = s_job.pal[v];
    }
    if (!s_job.tore && memcmp(row + (s_job.x >> 1), was + (s_job.x >> 1), (size_t)s_job.w >> 1)) {
        s_job.tore = true;
        sim_bug("row %d was drawn into after gfx_flushAsync() but before the flush sent it "
                "(the panel shows a torn frame). Draw after gfx_wait(), or after "
                "gfx_waitRow(%d) for this row.", r, r + 1);
    }
}

static int convertedRows(uint32_t now) {
    /* The first two chunks are converted before the DMA starts; after that
     * the ISR converts chunk k as chunk k-2 finishes sending. */
    int chunks = 2;
    if (now >= s_job.t0 && s_job.chunkUs > 0)
        chunks += (int)((now - s_job.t0) / s_job.chunkUs);
    if (chunks > s_job.chunks) chunks = s_job.chunks;
    int rows = chunks * s_job.rowsPer;
    return rows > s_job.h ? s_job.h : rows;
}

void sim_flushProgress(uint32_t now) {
    if (!s_job.active) return;
    if (!s_job.scratchBug)
        for (size_t i = 0; i < sizeof s_scratch; i++)
            if (s_scratch[i] != CANARY) {
                s_job.scratchBug = true;
                sim_bug("gfx_chunkScratch() was written while a flush was using it. "
                        "Call gfx_wait() first.");
                break;
            }
    int rows = convertedRows(now);
    for (; s_job.captured < rows; s_job.captured++) landRow(s_job.y + s_job.captured);
    if (now >= s_job.end && s_job.captured >= s_job.h) {
        s_job.active = false;
        sim_framePresented();
    }
}

static void startJob(int x, int y, int w, int h) {
    gfx_wait();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0) return;
    int align = (s_mode == GFX_12BPP) ? 8 : (s_mode == GFX_18BPP ? 4 : 2);
    int x1 = x + w;
    x = x & ~(align - 1);
    x1 = (x1 + align - 1) & ~(align - 1);
    if (x1 > GFX_W) x1 = GFX_W;
    w = x1 - x;

    uint32_t bpr = (s_mode == GFX_12BPP) ? (uint32_t)w * 3 / 2
                 : (s_mode == GFX_18BPP) ? (uint32_t)w * 3 : (uint32_t)w * 2;
    int rowsPer = (int)(GFX_CHUNK_BYTES / bpr);
    if (rowsPer < 1) rowsPer = 1;
    if (rowsPer > h) rowsPer = h;

    uint16_t out[16];
    gfx__paletteTake(out);
    for (int i = 0; i < 16; i++) s_job.pal[i] = panelColour(out[i]);
    memcpy(s_job.snap, gfx_fb, sizeof gfx_fb);
    memset(s_scratch, CANARY, sizeof s_scratch);

    uint32_t now = sim_now();
    s_job.active = true;
    s_job.x = x; s_job.y = y; s_job.w = w; s_job.h = h;
    s_job.rowsPer = rowsPer;
    s_job.chunks = (h + rowsPer - 1) / rowsPer;
    s_job.t0 = now + FLUSH_SETUP_US;
    s_job.chunkUs = bpr * rowsPer / wireRate();
    s_job.end = s_job.t0 + (uint32_t)(bpr * h / wireRate() + 0.5);
    s_job.captured = 0;
    s_job.tore = s_job.scratchBug = false;
    sim_flushProgress(now);            /* the first two chunks, converted up front */
}

/* Every entry point charges the sketch's own time first (--cost), then
 * lands whatever the flush has converted by now. */
struct Entry {
    Entry()  { sim_sync(); sim_flushProgress(sim_now()); }
    ~Entry() { sim_sync(); }
};

void gfx_flushRectAsync(int x, int y, int w, int h) { Entry e; startJob(x, y, w, h); }
void gfx_flushRect(int x, int y, int w, int h)      { Entry e; startJob(x, y, w, h); gfx_wait(); }
void gfx_flush(void)      { gfx_flushRect(0, 0, GFX_W, GFX_H); }
void gfx_flushAsync(void) { gfx_flushRectAsync(0, 0, GFX_W, GFX_H); }

/* A sketch may spin on these, so polling costs a little virtual time -
 * otherwise a busy-wait would never see the flush finish. */
bool gfx_busy(void) {
    Entry e;
    if (s_job.active) sim_advance(2);
    return s_job.active;
}

void gfx_wait(void) {
    Entry e;
    if (s_job.active) sim_advance(s_job.end > sim_now() ? s_job.end - sim_now() : 0);
}

int gfx_flushRow(void) {
    Entry e;
    if (s_job.active) sim_advance(1);
    if (!s_job.active || s_job.captured >= s_job.h) return GFX_H;
    return s_job.y + s_job.captured;
}

void gfx_waitRow(int y) {
    while (gfx_flushRow() < y) {}
}

/* ------------------------------------------------------------------ */
/* Direct streaming                                                    */
/* ------------------------------------------------------------------ */
void gfx_stream(gfx_streamFn fn, void *user) {
    Entry e;
    gfx_wait();
    if (s_mode == GFX_12BPP) s_mode = GFX_16BPP;
    uint32_t bpp = s_mode == GFX_18BPP ? 3 : 2, bpr = GFX_W * bpp;
    int rowsPer = (int)(GFX_CHUNK_BYTES / bpr);
    if (rowsPer < 1) rowsPer = 1;
    static uint8_t buf[GFX_CHUNK_BYTES] __attribute__((aligned(4)));
    for (int y = 0; y < GFX_H; y += rowsPer) {
        int rows = GFX_H - y < rowsPer ? GFX_H - y : rowsPer;
        fn(buf, y, rows, user);
        for (int r = 0; r < rows; r++)
            for (int x = 0; x < GFX_W; x++) {
                const uint8_t *p = buf + (size_t)r * bpr + (size_t)x * bpp;
                uint32_t c;
                if (bpp == 2) c = panelColour((uint16_t)(p[0] | p[1] << 8));
                else c = (uint32_t)(p[0] | p[0] >> 6) << 16 | (uint32_t)(p[1] | p[1] >> 6) << 8 | (p[2] | p[2] >> 6);
                sim_panel[(y + r) * GFX_W + x] = c;
            }
        sim_advance((uint32_t)(rows * bpr / wireRate()));
    }
    sim_framePresented();
}

/* ------------------------------------------------------------------ */
/* Configuration and the rest of the API                               */
/* ------------------------------------------------------------------ */
void gfx_begin(uint8_t spiDiv, uint8_t colorMode) {
    s_div = spiDiv; s_mode = colorMode;
    static const uint16_t defpal[16] = {
        0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
        0x8410, 0xC618, 0x7800, 0x03E0, 0x000F, 0x8400, 0x0410, 0x4208
    };
    gfx_setPalette(defpal, 16);
    memset(gfx_fb, 0, sizeof gfx_fb);
}
void gfx_setSpiDiv(uint8_t d)           { gfx_wait(); s_div = d; }
void gfx_setColorMode(uint8_t m)        { gfx_wait(); s_mode = m; gfx__paletteTouch(); }
uint8_t gfx_colorMode(void)             { return s_mode; }
uint8_t gfx_spiDiv(void)                { return s_div; }
uint32_t gfx_spiHz(void)                { return F_CPU / s_div; }
void gfx_setPanelOffsets(uint8_t, uint8_t, uint8_t) {}
void gfx_setInverted(bool) {}
void gfx_setPanelFrameRate(uint8_t, uint8_t, uint8_t) {}
uint8_t *gfx_chunkScratch(void)         { return s_scratch; }
uint8_t gfx_bytesPerPixel(void)         { return s_mode == GFX_18BPP ? 3 : 2; }
uint32_t gfx_frameBytes(void) {
    if (s_mode == GFX_12BPP) return GFX_W * GFX_H * 3u / 2u;
    if (s_mode == GFX_18BPP) return GFX_W * GFX_H * 3u;
    return GFX_W * GFX_H * 2u;
}

/* Direct-to-panel paths: time only. */
void gfx_select(void) {}
void gfx_deselect(void) {}
void gfx_setWindow(uint8_t, uint8_t, uint8_t, uint8_t) { sim_advance(5); }
void gfx_cmd(uint8_t) {}
void gfx_data8(uint8_t) {}
void gfx_writeColorLut(void) {}
void gfx_setWriteColorLut(bool) {}
void gfx_directFillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565) {
    gfx_wait();
    uint32_t c = panelColour(rgb565);
    for (int j = y; j < y + h && j < GFX_H; j++)
        for (int i = x; i < x + w && i < GFX_W; i++) sim_panel[j * GFX_W + i] = c;
    sim_advance((uint32_t)(w * h * 2 / wireRate()));
    sim_framePresented();
}
void gfx_directBlit(const void *, uint32_t bytes, bool) { sim_advance((uint32_t)(bytes / wireRate())); }
void gfx_blockingWrite(const uint8_t *, uint32_t bytes) { sim_advance((uint32_t)(bytes / 1.98)); }

/* Conversion internals, for the benchmark: 16 bpp only is plenty here. */
uint32_t gfx_convertSpan_ram(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint16_t *d = (uint16_t *)dst;
    for (uint32_t i = 0; i < n; i++) {
        *d++ = gfx_paletteOut(src[i] & 15);
        *d++ = gfx_paletteOut(src[i] >> 4);
    }
    return n * 4;
}
uint32_t gfx_convertRows_ram(uint8_t *dst, uint16_t row, uint16_t rows) {
    return gfx_convertSpan_ram(dst, gfx_fb + row * GFX_FB_STRIDE, (uint32_t)rows * GFX_FB_STRIDE);
}
uint32_t gfx_convertRows_flash(uint8_t *dst, uint16_t row, uint16_t rows) {
    return gfx_convertRows_ram(dst, row, rows);
}
