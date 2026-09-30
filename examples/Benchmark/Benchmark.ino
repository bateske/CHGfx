/*
 * CHGfxBench - graphics throughput benchmark for the CHGame board.
 *   CH32X035G8U6 @ 48 MHz + ST7735 1.44" 128x128 over SPI1.
 *
 * Open the Serial Monitor at any baud (it is USB CDC, the rate is
 * ignored) and results print there. A summary also lands on the panel,
 * so you can run this untethered.
 *
 * Build it with the FAST optimisation setting. In the Arduino IDE:
 *   Tools > Optimize > Faster (-O2)
 * The board's default is -Os, which costs roughly 10-20% on the drawing
 * primitives. From the command line:
 *   arduino-cli compile -b CHGame:ch32v:CHGame:opt=o2std .
 *
 * PRINTF WARNING
 * ---------------
 * This board links WCH's cut-down printf (boards.txt sets
 * ch_extra_lib=-lprintf). It handles %u/%s/%d but NOT the 'l' length
 * modifier: "%u" prints nothing AND swallows the argument, which
 * silently shifts every later field in the same format string. Use %u
 * with an (unsigned) cast - on rv32 unsigned int is already 32 bits.
 *
 * WHAT EACH NUMBER MEANS
 * ----------------------
 * "wire" tests measure raw SPI transport with no pixel generation, so
 * they bound everything else. "frame" tests include the 4 bpp -> panel
 * format conversion. "draw" tests touch only SRAM and tell you how much
 * CPU budget is left for the game after the display is fed.
 */
#include <CHGfx.h>
#include <fonts/CHGfx_Tiny3x5.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */
static uint32_t t0;
static inline void tic(void) { t0 = micros(); }
static inline uint32_t toc(void) { return micros() - t0; }

static char line[96];

static void report(const char *name, uint32_t us, uint32_t iterations,
                   uint32_t bytesEach)
{
    /* Guard against a zero measurement on very fast loops. */
    if (us == 0) us = 1;
    uint32_t usEach = us / (iterations ? iterations : 1);
    uint32_t fps    = usEach ? (1000000u / usEach) : 0;
    uint32_t kbps   = (uint32_t)(((uint64_t)bytesEach * iterations * 1000u) / us);

    if (bytesEach)
        snprintf(line, sizeof line, "%-34s %7u us  %5u fps  %5u KB/s",
                 name, (unsigned)usEach, (unsigned)fps,
                 (unsigned)kbps);
    else
        snprintf(line, sizeof line, "%-34s %7u us  %5u /s",
                 name, (unsigned)usEach, (unsigned)fps);
    Serial.println(line);
}

/* ------------------------------------------------------------------ */
/* A 16-colour palette that looks like something                       */
/* ------------------------------------------------------------------ */
static const uint16_t palette[16] = {
    0x0000, /*  0 black       */
    0x18E3, /*  1 dark grey   */
    0x4208, /*  2 grey        */
    0xC618, /*  3 light grey  */
    0xFFFF, /*  4 white       */
    0xF800, /*  5 red         */
    0xFD20, /*  6 orange      */
    0xFFE0, /*  7 yellow      */
    0x07E0, /*  8 green       */
    0x0400, /*  9 dark green  */
    0x07FF, /* 10 cyan        */
    0x001F, /* 11 blue        */
    0x0010, /* 12 navy        */
    0xF81F, /* 13 magenta     */
    0x8010, /* 14 purple      */
    0xFC9F  /* 15 pink        */
};

/* A 16x16 4 bpp test sprite, generated at boot. */
static uint8_t sprite[16 * 8];

static void makeSprite(void) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x += 2) {
            int d = (x - 8) * (x - 8) + (y - 8) * (y - 8);
            int e = (x + 1 - 8) * (x + 1 - 8) + (y - 8) * (y - 8);
            uint8_t lo = d < 49 ? (uint8_t)(5 + (d >> 3)) : 0;
            uint8_t hi = e < 49 ? (uint8_t)(5 + (e >> 3)) : 0;
            sprite[y * 8 + (x >> 1)] = (uint8_t)(lo | (hi << 4));
        }
    }
}

/* ================================================================== */
/* Benchmarks                                                          */
/* ================================================================== */

/*
 * 1. The baseline you are running today.
 *
 * Adafruit_GFX::drawPixel on an ST7735 issues setAddrWindow (CASET with
 * 4 args, RASET with 4 args, RAMWR) and then two data bytes. That is 13
 * SPI bytes plus 3 DC transitions per pixel. This reproduces it exactly
 * so the comparison is honest rather than rhetorical.
 */
static void benchNaivePixels(uint32_t count)
{
    gfx_select();
    tic();
    for (uint32_t i = 0; i < count; i++) {
        uint8_t x = i & 127, y = (i >> 7) & 127;
        gfx_setWindow(x, y, 1, 1);
        gfx_data8(0xF8);
        gfx_data8(0x00);
    }
    uint32_t us = toc();
    gfx_deselect();
    /* Cost is reported per full 128x128 screen's worth of pixels. */
    uint32_t perScreen = (uint32_t)((uint64_t)us * (GFX_W * GFX_H) / count);
    if (perScreen == 0) perScreen = 1;
    snprintf(line, sizeof line,
             "%-34s %7u us  %5u fps  (%u us/px)",
             "1 naive drawPixel (Adafruit-like)",
             (unsigned)perScreen,
             (unsigned)(1000000u / perScreen),
             (unsigned)(us / count));
    Serial.println(line);
}

/*
 * 2. Blocking byte-at-a-time streaming into one window.
 *    This is what Adafruit_ST7735::fillScreen costs: the window overhead
 *    is gone but the CPU still hand-feeds every byte.
 */
static void benchBlockingStream(uint32_t frames)
{
    uint8_t *scratch = gfx_chunkScratch();
    for (uint32_t i = 0; i < GFX_CHUNK_BYTES; i++) scratch[i] = (uint8_t)i;

    uint32_t bytes = gfx_frameBytes();
    gfx_select();
    tic();
    for (uint32_t f = 0; f < frames; f++) {
        gfx_setWindow(0, 0, GFX_W, GFX_H);
        uint32_t left = bytes;
        while (left) {
            uint32_t n = left > GFX_CHUNK_BYTES ? GFX_CHUNK_BYTES : left;
            gfx_blockingWrite(scratch, n);
            left -= n;
        }
    }
    uint32_t us = toc();
    gfx_deselect();
    report("2 blocking SPI stream", us, frames, bytes);
}

/*
 * 3. Pure DMA wire speed - the hard ceiling. No pixel generation, the
 *    DMA engine just re-reads one scratch buffer. Anything above this
 *    number is impossible on this hardware.
 */
static void benchWireDma(uint32_t frames)
{
    uint8_t *scratch = gfx_chunkScratch();
    uint32_t bytes = gfx_frameBytes();
    bool halfword = (gfx_colorMode() == GFX_16BPP);

    gfx_select();
    tic();
    for (uint32_t f = 0; f < frames; f++) {
        gfx_setWindow(0, 0, GFX_W, GFX_H);
        uint32_t left = bytes;
        while (left) {
            uint32_t n = left > GFX_CHUNK_BYTES ? GFX_CHUNK_BYTES : left;
            gfx_directBlit(scratch, n, halfword);
            left -= n;
        }
    }
    uint32_t us = toc();
    gfx_deselect();
    report("3 DMA wire ceiling", us, frames, bytes);
}

/*
 * 4. The zero-RAM fill: DMA with memory-increment disabled re-reads a
 *    single halfword for every pixel. Costs one variable and no CPU.
 */
static void benchDirectFill(uint32_t frames)
{
    if (gfx_colorMode() != GFX_16BPP) {
        Serial.println("4 DMA MINC=0 screen fill        (16 bpp only, skipped)");
        return;
    }
    tic();
    for (uint32_t f = 0; f < frames; f++)
        gfx_directFillRect(0, 0, GFX_W, GFX_H, (f & 1) ? 0x001F : 0xF800);
    report("4 DMA MINC=0 screen fill", toc(), frames, GFX_W * GFX_H * 2u);
}

/*
 * 5. Conversion cost alone, RAM-resident vs flash-resident.
 *    The delta is the CH32X035's 3-wait-state flash showing up.
 */
static void benchConvert(uint32_t frames)
{
    uint8_t *scratch = gfx_chunkScratch();

    tic();
    for (uint32_t f = 0; f < frames; f++)
        for (uint16_t r = 0; r < GFX_H; r += GFX_CHUNK_ROWS)
            gfx_convertRows_ram(scratch, r, GFX_CHUNK_ROWS);
    report("5a convert only, code in SRAM", toc(), frames, gfx_frameBytes());

    tic();
    for (uint32_t f = 0; f < frames; f++)
        for (uint16_t r = 0; r < GFX_H; r += GFX_CHUNK_ROWS)
            gfx_convertRows_flash(scratch, r, GFX_CHUNK_ROWS);
    report("5b convert only, code in flash", toc(), frames, gfx_frameBytes());
}

/*
 * 6. The real number: framebuffer -> panel, blocking.
 */
static void benchFlush(uint32_t frames)
{
    tic();
    for (uint32_t f = 0; f < frames; f++) gfx_flush();
    report("6 gfx_flush (fb -> panel)", toc(), frames, gfx_frameBytes());
}

/*
 * 7. Async flush: how much CPU time the main loop gets back while the
 *    frame is on the wire. We spin a countable workload during the
 *    transfer and report what fraction of a solo run we completed.
 */
static volatile uint32_t sink;

static uint32_t soloWork(uint32_t iters) {
    uint32_t a = 1;
    for (uint32_t i = 0; i < iters; i++) a = a * 1664525u + 1013904223u;
    sink = a;
    return a;
}

/*
 * 7c. What an async flush costs the CPU. The conversion runs inside the
 *     DMA interrupt, so a main loop running during the flush loses that
 *     much of its time. Measure how much work gets done while a flush is
 *     in flight, against the same work alone.
 */
static uint32_t flushCpuCost(int h)
{
    const uint32_t CAL = 20000;
    tic();
    soloWork(CAL);
    uint32_t calUs = toc();

    gfx_wait();
    uint32_t iters = 0;
    tic();
    gfx_flushRectAsync(0, 0, GFX_W, h);
    while (gfx_busy()) { soloWork(50); iters += 50; }
    uint32_t elapsed = toc();
    uint32_t worked = (uint32_t)((uint64_t)iters * calUs / CAL);
    return elapsed > worked ? elapsed - worked : 0;
}

static void benchFlushCost(void)
{
    uint32_t full = 0, half = 0;
    for (int i = 0; i < 8; i++) { full += flushCpuCost(GFX_H); half += flushCpuCost(GFX_H / 2); }
    snprintf(line, sizeof line, "7c async flush CPU cost: %u us/full frame, %u us/half",
             (unsigned)(full / 8), (unsigned)(half / 8));
    Serial.println(line);
}

static void benchAsync(void)
{
    const uint32_t WORK = 20000;

    tic();
    soloWork(WORK);
    uint32_t soloUs = toc();
    if (soloUs == 0) soloUs = 1;

    tic();
    gfx_flushAsync();
    soloWork(WORK);
    uint32_t withWorkUs = toc();
    gfx_wait();
    uint32_t totalUs = toc();

    tic();
    gfx_flush();
    uint32_t blockingUs = toc();

    snprintf(line, sizeof line,
             "7 async: %u us work solo -> %u us during flush (%u%% CPU kept)",
             (unsigned)soloUs, (unsigned)withWorkUs,
             (unsigned)(soloUs * 100u / (withWorkUs ? withWorkUs : 1)));
    Serial.println(line);
    snprintf(line, sizeof line,
             "  async frame %u us vs blocking %u us",
             (unsigned)totalUs, (unsigned)blockingUs);
    Serial.println(line);
}

/*
 * 8. Drawing primitives. These never touch SPI - they are the CPU side
 *    of your frame budget.
 */
static void benchDraw(void)
{
    const uint32_t N = 200;

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_clear(i & 15);
    report("8a gfx_clear", toc(), N, GFX_FB_BYTES);

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_fillRect(4, 4, 120, 120, i & 15);
    report("8b fillRect 120x120", toc(), N, 0);

    tic();
    for (uint32_t i = 0; i < N * 10; i++) gfx_hline(0, i & 127, 128, i & 15);
    report("8c hline x128 (per line)", toc(), N * 10, 0);

    tic();
    for (uint32_t i = 0; i < N * 4; i++)
        gfx_blit(sprite, (int)(i & 63), (int)((i >> 3) & 63), 16, 16, -1);
    report("8d blit 16x16 opaque (even x)", toc(), N * 4, 0);

    tic();
    for (uint32_t i = 0; i < N * 4; i++)
        gfx_blit(sprite, (int)(i & 63), (int)((i >> 3) & 63), 16, 16, 0);
    report("8e blit 16x16 transparent", toc(), N * 4, 0);

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_text(2, 2, "The quick brown fox 0123", 4);
    report("8f text, 24 chars", toc(), N, 0);

    tic();
    for (uint32_t i = 0; i < N * 2; i++)
        gfx_line(0, 0, 127, (int)(i & 127), (uint8_t)(i & 15));
    report("8g line 128px diagonal", toc(), N * 2, 0);

    tic();
    for (uint32_t i = 0; i < N * 2; i++) gfx_fillCircle(64, 64, 30, i & 15);
    report("8h fillCircle r=30", toc(), N * 2, 0);
}

/*
 * 13. The 1.3 additions: span sprites, shapes, text effects, row
 *     operations. CPU only, like test 8.
 */
static const uint8_t SLIME[77] = {        /* 16x12 span sprite, see examples/GameKit */
    0x10, 0x0C, 0x02, 0x5F, 0x30, 0x04, 0x3F, 0x10, 0x35, 0x10, 0x06, 0x2F,
    0x00, 0x15, 0x11, 0x35, 0x00, 0x06, 0x1F, 0x00, 0x15, 0x11, 0x55, 0x00,
    0x04, 0x1F, 0x00, 0x95, 0x00, 0x08, 0x0F, 0x00, 0x25, 0x10, 0x15, 0x10,
    0x25, 0x00, 0x0A, 0x0F, 0x00, 0x25, 0x00, 0x01, 0x15, 0x00, 0x01, 0x25,
    0x00, 0x08, 0x0F, 0x00, 0x25, 0x10, 0x15, 0x10, 0x25, 0x00, 0x03, 0x00,
    0xD5, 0x00, 0x05, 0x00, 0x45, 0x36, 0x45, 0x00, 0x05, 0x00, 0x06, 0xB5,
    0x06, 0x00, 0x02, 0x0F, 0xD0
};

static void benchExtras(void)
{
    const uint32_t N = 200;
    static uint8_t remap[16];
    for (int i = 0; i < 16; i++) remap[i] = (uint8_t)(15 - i);
    gfx_wait();

    tic();
    for (uint32_t i = 0; i < N * 4; i++) gfx_sprite4(SLIME, (int)(i & 63), (int)((i >> 3) & 63), remap);
    report("13a sprite4 16x12, remapped", toc(), N * 4, 0);

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_sprite4(SLIME, (int)(i & 31), 20, remap, 512);
    report("13b sprite4 scaled 2x", toc(), N, 0);

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_sprite4Rot(SLIME, 8, 6, 64, 64, (uint8_t)i, 256, remap);
    report("13c sprite4Rot 16x12", toc(), N, 0);

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_fillRoundRect(14, 100, 100, 22, 4, i & 15);
    report("13d fillRoundRect 100x22 r4", toc(), N, 0);

    tic();
    for (uint32_t i = 0; i < N * 4; i++) gfx_fillEllipse(64, 64, 7, 2, i & 15);
    report("13e fillEllipse 15x5 (shadow)", toc(), N * 4, 0);

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_dither(0, 0, 128, 32, i & 15, (uint8_t)(i & 1));
    report("13f dither 128x32", toc(), N, 0);

    tic();
    for (uint32_t i = 0; i < N; i++) gfx_remapRect(0, 0, 128, 32, remap);
    report("13g remapRect 128x32", toc(), N, 0);

    gfx_setFont(&CHGfx_Tiny3x5);
    tic();
    for (uint32_t i = 0; i < N; i++) gfx_text(2, 8, "SCORE 001234  LIVES 3", 4);
    report("13h text Tiny3x5, 21 chars", toc(), N, 0);

    tic();
    for (uint32_t i = 0; i < N / 4; i++) gfx_textFx(10, 60, "GAME KIT!", 3, 7, 0, 5);
    report("13i textFx 3x, outline+shadow", toc(), N / 4, 0);
    gfx_setFont(nullptr);

    uint32_t patRow[GFX_FB_STRIDE / 4];
    memset(patRow, 0x5A, sizeof patRow);
    tic();
    for (uint32_t i = 0; i < N / 4; i++)
        for (int y = 10; y < GFX_H; y++) gfx_copyRow(y, (const uint8_t *)patRow, 0, GFX_W);
    report("13j copyRow x118 (a floor)", toc(), N / 4, 0);

    /* The screen shake CHChess started with, and the replacement. */
    tic();
    for (uint32_t i = 0; i < N / 10; i++)
        memmove(gfx_fb + 11 * GFX_FB_STRIDE + 1, gfx_fb + 10 * GFX_FB_STRIDE, 117 * GFX_FB_STRIDE - 1);
    report("13k shake by memmove (118 rows)", toc(), N / 10, 0);
    tic();
    for (uint32_t i = 0; i < N / 10; i++) gfx_scroll(10, 118, 2, 1);
    report("13l shake by gfx_scroll", toc(), N / 10, 0);
    tic();
    for (uint32_t i = 0; i < N / 10; i++) gfx_scroll(10, 118, 1, 1);
    report("13m gfx_scroll, odd dx (nibbles)", toc(), N / 10, 0);
}

/*
 * 14. Racing the beam. The frame is cleared and drawn in two halves;
 *     "racing" starts the top half as soon as the flush has sent it,
 *     instead of waiting for the whole frame.
 */
static void drawHalf(int y0)
{
    gfx_setClip(0, y0, GFX_W, GFX_H / 2);
    gfx_clear(12);
    for (int i = 0; i < 16; i++) gfx_sprite4(SLIME, (i * 29) & 111, y0 + ((i * 13) & 51), nullptr);
    gfx_resetClip();
}

static void benchRacing(uint32_t frames)
{
    tic();
    for (uint32_t f = 0; f < frames; f++) { gfx_wait(); drawHalf(0); drawHalf(64); gfx_flushAsync(); }
    gfx_wait();
    report("14a wait, then draw", toc(), frames, 0);
    tic();
    for (uint32_t f = 0; f < frames; f++) {
        gfx_waitRow(64); drawHalf(0);
        gfx_wait();      drawHalf(64);
        gfx_flushAsync();
    }
    gfx_wait();
    report("14b racing the beam", toc(), frames, 0);
}

/*
 * 9. End to end: a plausible game frame (clear + tile background +
 *    32 sprites + HUD text) plus the flush. This is the number that
 *    actually predicts your framerate.
 */
static uint32_t gameFrame(void)
{
    static uint16_t tick = 0;
    tick++;
    gfx_clear(12);
    for (int y = 0; y < 128; y += 16)
        for (int x = 0; x < 128; x += 16)
            if (((x ^ y) >> 4) & 1) gfx_fillRect(x, y, 16, 16, 9);
    for (int i = 0; i < 32; i++) {
        int sx = ((i * 37 + tick) & 111) & ~1;
        int sy = ((i * 53 + (tick >> 1)) & 111);
        gfx_blit(sprite, sx, sy, 16, 16, 0);
    }
    gfx_text(2, 2, "SCORE 000000", 4);
    gfx_text(2, 118, "CH32X035 @ 48MHz", 7);
    return 0;
}

static void benchGame(uint32_t frames)
{
    tic();
    for (uint32_t f = 0; f < frames; f++) { gameFrame(); gfx_flush(); }
    report("9a game frame, blocking flush", toc(), frames, gfx_frameBytes());

    tic();
    for (uint32_t f = 0; f < frames; f++) { gfx_wait(); gameFrame(); gfx_flushAsync(); }
    gfx_wait();
    report("9b game frame, async flush", toc(), frames, gfx_frameBytes());
}

/*
 * 10. Partial updates. The wire is the bottleneck, so cost scales with
 *     area, not with how clever the drawing code is. This is the lever
 *     that takes a real game from 60 fps to "whatever you want".
 */
static void benchPartial(void)
{
    static const uint8_t sizes[] = { 128, 96, 64, 32, 16 };
    for (uint8_t i = 0; i < sizeof sizes; i++) {
        uint8_t n = sizes[i];
        uint32_t reps = 30;
        tic();
        for (uint32_t f = 0; f < reps; f++) gfx_flushRect(0, 0, n, n);
        uint32_t us = toc() / reps;
        if (us == 0) us = 1;
        uint32_t bytes = (uint32_t)n * n *
                         (gfx_colorMode() == GFX_12BPP ? 3u : 4u) / 2u;
        snprintf(line, sizeof line,
                 "10 flushRect %3ux%-3u (%5u B)      %7u us  %5u fps",
                 n, n, (unsigned)bytes, (unsigned)us,
                 (unsigned)(1000000u / us));
        Serial.println(line);
    }
}

/*
 * 11. A dirty-rect game loop against the same scene as test 9. Only the
 *     band the sprites live in gets sent. Same drawing work, a fraction
 *     of the wire time.
 */
static void benchDirtyGame(uint32_t frames)
{
    tic();
    for (uint32_t f = 0; f < frames; f++) {
        gfx_wait();
        gameFrame();
        gfx_flushRectAsync(0, 32, 128, 64);   /* only the middle band */
    }
    gfx_wait();
    report("11 game frame, 128x64 dirty band", toc(), frames, 0);
}

/* ------------------------------------------------------------------ */
/* SPI clock sweep - VISUAL test                                       */
/* ------------------------------------------------------------------ */
/*
 * The ST7735S datasheet gives tSCYCW(min) = 66 ns, i.e. 15.1 MHz. We run
 * at 24 MHz. There is no read-back path on this 14-pin flex (no SDO), so
 * the only way to validate is to look at the panel. Each step draws a
 * high-contrast dither that makes bit errors obvious as speckle or as a
 * horizontal shear.
 */
static void spiSweep(void)
{
    static const uint8_t divs[] = { GFX_DIV8, GFX_DIV4, GFX_DIV2 };
    Serial.println();
    Serial.println("-- SPI clock sweep: WATCH THE PANEL --");
    Serial.println("   Clean checkerboard = good. Speckle/shear = too fast.");

    for (uint8_t i = 0; i < sizeof divs; i++) {
        gfx_setSpiDiv(divs[i]);
        /* 1-pixel vertical stripes: the worst case for SPI setup/hold,
         * because every clock edge has to flip MOSI. If the panel is
         * being overclocked past what it can latch, this is where it
         * shows up first. */
        for (int y = 0; y < GFX_H; y++) {
            for (int x = 0; x < GFX_W; x++) {
                bool on = ((x ^ y) & 1) != 0;
                gfx_pixel(x, y, on ? 4 : 0);
            }
        }
        gfx_fillRect(20, 50, 88, 24, 12);
        char buf[24];
        snprintf(buf, sizeof buf, "%u MHz", (unsigned)(gfx_spiHz() / 1000000u));
        gfx_text(28, 58, buf, 7);

        tic();
        for (int k = 0; k < 20; k++) gfx_flush();
        uint32_t us = toc() / 20;

        snprintf(line, sizeof line, "   HCLK/%-2u = %2u MHz -> %5u us/frame, %3u fps",
                 divs[i], (unsigned)(gfx_spiHz() / 1000000u),
                 (unsigned)us, (unsigned)(1000000u / (us ? us : 1)));
        Serial.println(line);
        delay(1500);
    }
    gfx_setSpiDiv(GFX_DIV2);
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */
static void runSuite(const char *label, bool includeDraw)
{
    Serial.println();
    Serial.print("================ ");
    Serial.print(label);
    Serial.println(" ================");
    snprintf(line, sizeof line, "SPI %u MHz, %u bpp, %u bytes/frame",
             (unsigned)(gfx_spiHz() / 1000000u),
             gfx_colorMode() == GFX_12BPP ? 12 : 16,
             (unsigned)gfx_frameBytes());
    Serial.println(line);
    Serial.println("-------------------------------------------------------------------");

    benchNaivePixels(2000);
    benchBlockingStream(10);
    benchWireDma(20);
    benchDirectFill(20);
    benchConvert(20);
    benchFlush(30);
    benchAsync();
    benchFlushCost();
    if (includeDraw) { benchDraw(); benchExtras(); }   /* CPU-only, identical in both modes */
    benchGame(20);
    benchPartial();
    benchDirtyGame(30);
    benchRacing(30);
}

static void showSummaryOnPanel(uint32_t us16, uint32_t us12)
{
    gfx_clear(0);
    gfx_text(4, 4,   "CHGfxBench", 7);
    gfx_hline(4, 13, 120, 2);

    char buf[26];
    snprintf(buf, sizeof buf, "SPI %u MHz", (unsigned)(gfx_spiHz() / 1000000u));
    gfx_text(4, 20, buf, 4);

    snprintf(buf, sizeof buf, "16bpp %u fps", (unsigned)(1000000u / (us16 ? us16 : 1)));
    gfx_text(4, 32, buf, 8);
    snprintf(buf, sizeof buf, "12bpp %u fps", (unsigned)(1000000u / (us12 ? us12 : 1)));
    gfx_text(4, 42, buf, 10);

    gfx_text(4, 58, "16 colours, 8KB fb", 3);
    gfx_text(4, 68, "DMA + LUT expand", 3);

    for (int i = 0; i < 16; i++) gfx_fillRect(i * 8, 90, 8, 16, (uint8_t)i);
    gfx_text(4, 112, "moving on to demo", 2);
    gfx_flush();
    delay(2500);
}

/*
 * A small animated demo so the board does something after the numbers.
 *
 * NOTE: an earlier version used cosf()/sinf() here and ran at 29 fps -
 * this chip has no FPU, so 80 soft-float transcendentals per frame cost
 * more than the entire graphics pipeline. A 64-entry integer quarter-
 * sine table makes the demo measure graphics instead of libm.
 */
static const int8_t sinTab[64] = {   /* sin(i * pi/128) * 127, i = 0..63 */
      0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,  40,
     43,  46,  49,  51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,
     81,  83,  85,  88,  90,  92,  94,  96,  98, 100, 102, 104, 106, 107,
    109, 111, 112, 113, 115, 116, 117, 118, 120, 121, 122, 122, 123, 124,
    125, 125, 126, 126, 126, 127, 127, 127
};

/* Full-circle sine, angle in 1/256 turn, result -127..127. */
static inline int isin(uint8_t a) {
    uint8_t q = a & 63;
    int v = (a & 64) ? sinTab[63 - q] : sinTab[q];
    return (a & 128) ? -v : v;
}
static inline int icos(uint8_t a) { return isin((uint8_t)(a + 64)); }

static void demo(void)
{
    static uint16_t t = 0;
    gfx_wait();
    t++;
    gfx_clear(12);
    for (int i = 0; i < 40; i++) {
        uint8_t a = (uint8_t)(t + i * 9);
        int sx = 56 + (icos(a) * 52) / 127;
        int sy = 56 + (isin((uint8_t)(a * 2 + i)) * 48) / 127;
        gfx_blit(sprite, sx & ~1, sy, 16, 16, 0);
    }
    gfx_rect(0, 0, 128, 128, 4);
    gfx_text(4, 4, "CH32X035 + ST7735", 7);
    gfx_flushAsync();
}

void setup()
{
    /* NOTE: on this core Serial is USB CDC and begin() takes an optional
     * FIFO size, NOT a baud rate. Passing 115200 here silently truncates
     * to a 49664-byte FIFO request. Call it with no argument. */
    Serial.begin();
    /* USB CDC: this core exposes waitForPC() rather than operator bool.
     * Give the host 4 s to open the port, then run regardless so the
     * board still benchmarks and draws when untethered. */
    Serial.waitForPC(4000);
    delay(300);

    makeSprite();

    gfx_begin(GFX_DIV2, GFX_16BPP);
    gfx_setPalette(palette, 16);

    Serial.println();
    Serial.println("###################################################################");
    Serial.println("# CHGfxBench - CH32X035G8U6 @ 48 MHz + ST7735 128x128");
    snprintf(line, sizeof line,
             "# framebuffer %u B (4 bpp), chunk %u B x2, SPI ceiling %u KB/s",
             GFX_FB_BYTES, GFX_CHUNK_BYTES, (unsigned)(gfx_spiHz() / 8 / 1000));
    Serial.println(line);
    Serial.println("###################################################################");

    runSuite("16 bpp / RGB565", true);
    uint32_t us16;
    { tic(); for (int i = 0; i < 30; i++) gfx_flush(); us16 = toc() / 30; }

    gfx_setColorMode(GFX_12BPP);
    gfx_setPalette(palette, 16);
    runSuite("12 bpp / RGB444", false);
    uint32_t us12;
    { tic(); for (int i = 0; i < 30; i++) gfx_flush(); us12 = toc() / 30; }

    Serial.println();
    snprintf(line, sizeof line,
             ">> 12 bpp is %u%% of 16 bpp frame time (theory: 75%%)",
             (unsigned)(us12 * 100u / (us16 ? us16 : 1)));
    Serial.println(line);

    gfx_setColorMode(GFX_16BPP);
    gfx_setPalette(palette, 16);
    spiSweep();

    showSummaryOnPanel(us16, us12);

    Serial.println();
    Serial.println("Benchmarks done - running the animated demo.");
}

void loop()
{
    static uint32_t frames = 0, last = 0;

    /* Send any character to re-run the whole suite. The first run
     * happens before the host has the port open, so this is how you get
     * the numbers into a terminal. */
    if (Serial.available()) {
        while (Serial.available()) Serial.read();
        runSuite("16 bpp / RGB565", true);
        gfx_setColorMode(GFX_12BPP);
        gfx_setPalette(palette, 16);
        runSuite("12 bpp / RGB444", false);
        gfx_setColorMode(GFX_16BPP);
        gfx_setPalette(palette, 16);
        Serial.println("Re-run complete. Send another character to repeat.");
    }

    demo();
    frames++;
    if (millis() - last >= 2000) {
        snprintf(line, sizeof line, "demo: %u fps", (unsigned)(frames / 2));
        Serial.println(line);
        frames = 0;
        last = millis();
    }
}
