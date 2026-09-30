/*
 * chsim_main.cpp - runs a CHGame sketch on a PC.
 *
 *   sim.exe [options]
 *     --frames N        stop after N presented frames (default 300)
 *     --out FILE        write presented frames to FILE (see below)
 *     --every K         ... every K-th frame (default 1)
 *     --start S         ... starting at frame S (default 0)
 *     --input SPEC      buttons: "F:BTN+BTN,F:,..." - from presented frame
 *                       F on, hold these buttons ("" = none). Buttons: A B
 *                       UP DOWN LEFT RIGHT START SELECT
 *     --cost            charge the sketch's own CPU time, scaled from the
 *                       host to the device with a calibration against the
 *                       benchmark's measured primitives
 *     --max-seconds T   give up after T seconds of virtual time (default 120)
 *
 * The frame file is a sequence of records: uint32 frame number, uint32
 * virtual milliseconds, then 128*128 RGB888 pixels (3 bytes each, row
 * major). chsim.py turns it into PNGs or a GIF.
 *
 * Serial output goes to stdout; simulator reports and BUG lines to
 * stderr. Exit status 3 if any BUG was reported.
 */
#include <Arduino.h>
#include <CHGfx.h>
#include <chrono>
#include "sim.h"

void setup();
void loop();

SimSerial Serial;

static uint32_t s_now = 0;
static int      s_bugs = 0;
static uint32_t s_frames = 0;        /* frames presented */
static uint32_t s_limit = 300, s_every = 1, s_start = 0;
static uint32_t s_maxUs = 120u * 1000000u;
static FILE    *s_out = nullptr;

/* ------------------------------------------------------------------ */
/* Virtual time                                                        */
/* ------------------------------------------------------------------ */
uint32_t sim_now() { return s_now; }

void sim_advance(uint32_t us) {
    /* Whatever the flush converts while time moves on, it converts from
     * the framebuffer as it is now: nothing draws in between. */
    s_now += us;
    sim_flushProgress(s_now);
    if (s_now > s_maxUs) {
        fprintf(stderr, "chsim: %u s of virtual time without reaching %u frames - stopping\n",
                (unsigned)(s_maxUs / 1000000u), (unsigned)s_limit);
        exit(s_bugs ? 3 : 2);
    }
}

void sim_bug(const char *fmt, ...) {
    s_bugs++;
    if (s_bugs > 20) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "BUG (frame %u, t=%u.%03u ms): ", (unsigned)s_frames,
            (unsigned)(s_now / 1000), (unsigned)(s_now % 1000));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* --cost: host nanoseconds since the last sync, times the calibrated
 * device/host ratio. */
static bool     s_cost = false;
static double   s_ratio = 0;
static uint64_t s_hostMark = 0;
static double   s_costCarry = 0, s_costTotalUs = 0;

static uint64_t hostNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sim_sync() {
    uint64_t h = hostNs();
    if (s_cost && s_hostMark) {
        s_costCarry += (double)(h - s_hostMark) / 1000.0 * s_ratio;
        uint32_t us = (uint32_t)s_costCarry;
        s_costCarry -= us;
        s_costTotalUs += us;
        if (us) sim_advance(us);
    }
    s_hostMark = hostNs();
}

/* Device figures from benchmark-results.txt, 16 bpp run: gfx_clear,
 * hline x128, blit 16x16 transparent, text 24 chars, fillCircle r=30. */
static void calibrate() {
    static uint8_t spr[16 * 8];
    for (int i = 0; i < (int)sizeof spr; i++) spr[i] = (uint8_t)(i * 37);
    const double device[5] = { 93, 4, 100, 246, 263 };
    double ratios[5];
    for (int op = 0; op < 5; op++) {
        double best = 1e30;
        for (int rep = 0; rep < 7; rep++) {
            const int N = 400;
            uint64_t t = hostNs();
            for (int i = 0; i < N; i++) {
                switch (op) {
                    case 0: gfx_clear((uint8_t)(i & 15)); break;
                    case 1: gfx_hline(0, i & 127, 128, (uint8_t)(i & 15)); break;
                    case 2: gfx_blit(spr, i & 63, (i >> 3) & 63, 16, 16, 0); break;
                    case 3: gfx_text(2, 2, "The quick brown fox 0123", 4); break;
                    case 4: gfx_fillCircle(64, 64, 30, (uint8_t)(i & 15)); break;
                }
            }
            double us = (double)(hostNs() - t) / 1000.0 / N;
            if (us < best) best = us;
        }
        ratios[op] = device[op] / (best > 1e-4 ? best : 1e-4);
    }
    /* Median: one op the host happens to be very good or bad at does not
     * skew the rest. */
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (ratios[j] < ratios[i]) { double t = ratios[i]; ratios[i] = ratios[j]; ratios[j] = t; }
    s_ratio = ratios[2];
    memset(gfx_fb, 0, GFX_FB_BYTES);
    fprintf(stderr, "chsim: cost model, device/host = %.1f\n", s_ratio);
}

/* ------------------------------------------------------------------ */
/* Frames out                                                          */
/* ------------------------------------------------------------------ */
static void finish() {
    fprintf(stderr, "chsim: %u frames in %u.%03u s virtual (%.1f fps)",
            (unsigned)s_frames, (unsigned)(s_now / 1000000u), (unsigned)(s_now / 1000u % 1000u),
            s_now ? s_frames * 1e6 / s_now : 0.0);
    if (s_cost && s_frames) fprintf(stderr, ", sketch CPU ~%.2f ms/frame", s_costTotalUs / 1000.0 / s_frames);
    fprintf(stderr, ", %d bug%s\n", s_bugs, s_bugs == 1 ? "" : "s");
    if (s_out) fclose(s_out);
    fflush(stdout);
    exit(s_bugs ? 3 : 0);
}

void sim_framePresented() {
    uint32_t n = s_frames++;
    if (s_out && n >= s_start && (n - s_start) % s_every == 0) {
        uint32_t hdr[2] = { n, s_now / 1000u };
        fwrite(hdr, sizeof hdr, 1, s_out);
        static uint8_t rgb[GFX_W * GFX_H * 3];
        for (int i = 0; i < GFX_W * GFX_H; i++) {
            rgb[i * 3 + 0] = (uint8_t)(sim_panel[i] >> 16);
            rgb[i * 3 + 1] = (uint8_t)(sim_panel[i] >> 8);
            rgb[i * 3 + 2] = (uint8_t)sim_panel[i];
        }
        fwrite(rgb, sizeof rgb, 1, s_out);
    }
    if (s_frames >= s_limit) finish();
}

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */
struct Press { uint32_t frame; uint32_t mask; };
static Press s_input[256];
static int   s_inputs = 0;

static uint32_t buttonBit(uint32_t pin) {
    return (pin >= PIN_BTN_UP && pin <= PIN_BTN_START) ? 1u << (pin - PIN_BTN_UP) : 0;
}

static uint32_t parseButtons(const char *s, const char *end) {
    static const char *names[8] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "SELECT", "START" };
    uint32_t m = 0;
    while (s < end) {
        const char *e = s;
        while (e < end && *e != '+') e++;
        bool ok = false;
        for (int i = 0; i < 8; i++)
            if ((size_t)(e - s) == strlen(names[i]) && !strncmp(s, names[i], (size_t)(e - s))) { m |= 1u << i; ok = true; }
        if (!ok && e > s) { fprintf(stderr, "chsim: unknown button '%.*s'\n", (int)(e - s), s); exit(2); }
        s = e < end ? e + 1 : e;
    }
    return m;
}

static void parseInput(const char *spec) {
    const char *p = spec;
    while (*p && s_inputs < 256) {
        const char *e = strchr(p, ',');
        if (!e) e = p + strlen(p);
        const char *colon = (const char *)memchr(p, ':', (size_t)(e - p));
        if (!colon) { fprintf(stderr, "chsim: bad --input item '%.*s'\n", (int)(e - p), p); exit(2); }
        s_input[s_inputs].frame = (uint32_t)atoi(p);
        s_input[s_inputs].mask = parseButtons(colon + 1, e);
        s_inputs++;
        p = *e ? e + 1 : e;
    }
}

bool sim_buttonHeld(uint32_t pin) {
    uint32_t m = 0;
    for (int i = 0; i < s_inputs; i++) if (s_input[i].frame <= s_frames) m = s_input[i].mask;
    return (m & buttonBit(pin)) != 0;
}

/* ------------------------------------------------------------------ */
/* Arduino                                                             */
/* ------------------------------------------------------------------ */
uint32_t micros() { sim_sync(); return s_now; }
uint32_t millis() { sim_sync(); return s_now / 1000; }
void delay(uint32_t ms) { sim_sync(); sim_advance(ms * 1000); }
void delayMicroseconds(uint32_t us) { sim_sync(); sim_advance(us); }
void pinMode(uint32_t, uint32_t) {}
void digitalWrite(uint32_t, uint32_t) {}
int  digitalRead(uint32_t pin) { return sim_buttonHeld(pin) ? LOW : HIGH; }

static uint32_t s_rng = 1;
static uint32_t rng() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
void randomSeed(unsigned long s) { s_rng = s ? (uint32_t)s : 1; }
long random(long hi) { return hi > 0 ? (long)(rng() % (uint32_t)hi) : 0; }
long random(long lo, long hi) { return hi > lo ? lo + random(hi - lo) : lo; }

int SimSerial::available() { return 0; }
int SimSerial::read() { return -1; }
size_t SimSerial::write(uint8_t c) { fputc(c, stdout); return 1; }
size_t SimSerial::write(const uint8_t *p, size_t n) { return fwrite(p, 1, n, stdout); }
size_t SimSerial::print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
int SimSerial::printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if      (!strcmp(a, "--frames"))      { s_limit = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--every"))       { s_every = (uint32_t)atoi(v); if (!s_every) s_every = 1; i++; }
        else if (!strcmp(a, "--start"))       { s_start = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--out"))         { s_out = fopen(v, "wb"); if (!s_out) { perror(v); return 2; } i++; }
        else if (!strcmp(a, "--input"))       { parseInput(v); i++; }
        else if (!strcmp(a, "--cost"))        { s_cost = true; }
        else if (!strcmp(a, "--max-seconds")) { s_maxUs = (uint32_t)(atof(v) * 1e6); i++; }
        else { fprintf(stderr, "chsim: unknown option %s\n", a); return 2; }
    }
    if (s_cost) calibrate();
    sim_sync();
    setup();
    for (;;) {
        loop();
        sim_sync();
        sim_advance(20);          /* loop() overhead on the board, roughly */
    }
}
