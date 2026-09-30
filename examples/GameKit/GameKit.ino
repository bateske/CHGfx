/*
 * GameKit - the CHGfx 1.3 game-making pieces on one screen.
 *
 *   * a clip rectangle: the playfield scrolls and shakes under a HUD it
 *     never touches
 *   * span sprites (gfx_sprite4): one slime, four teams by palette remap,
 *     a white hit flash, a zoomed one and a tumbling one (gfx_sprite4Rot)
 *   * row operations: the floor is two pattern rows stamped with
 *     gfx_copyRow, the shake is gfx_scroll
 *   * shapes: ellipse shadows, a rounded panel over a dithered backdrop
 *   * the 3x5 font for the HUD, and outlined, gradient, wavy banner text
 *   * palette animation: two slots cycle every frame, and the whole
 *     screen fades in and out, without redrawing anything for it
 *
 * Press A (or wait) for a banner and a shake.
 *
 * Runs on the PC too:  python extras/sim/chsim.py run examples/GameKit --gif kit.gif
 */
#include <CHGfx.h>
#include <fonts/CHGfx_Tiny3x5.h>

enum : uint8_t {
    INK, WHITE, NAVY, FELT_DK, FELT, RED, RED_DK, BLUE,
    BLUE_DK, GREEN, GREEN_DK, GOLD, GOLD_DK, GREY, FX_A, FX_B
};

static const uint16_t palette[16] = {
    0x0000, 0xFFFF, 0x10AB, 0x0220, 0x0B45, 0xE8A4, 0x7800, 0x2A7F,
    0x0010, 0x2F24, 0x03A0, 0xFE00, 0xA3C0, 0x9CF3, 0xF81F, 0xFE00
};

/* 16x12, drawn in RED / RED_DK / WHITE / INK. Packed from this with
 * extras/sprite4.py ('.' = transparent):
 *
 *     ......kkkk......     k INK   w WHITE
 *     ....kkbbbbkk....     b RED   d RED_DK
 *     ...kbbwwbbbbk...
 *     ..kbbwwbbbbbbk..
 *     ..kbbbbbbbbbbk..
 *     .kbbbkkbbkkbbbk.
 *     .kbbbkwbbkwbbbk.
 *     .kbbbkkbbkkbbbk.
 *     kbbbbbbbbbbbbbbk
 *     kbbbbbddddbbbbbk
 *     kdbbbbbbbbbbbbdk
 *     .kkkkkkkkkkkkkk.
 *
 * 77 bytes, against 96 as a plain 4 bpp bitmap. */
static const uint8_t SLIME[77] = {
    0x10, 0x0C, 0x02, 0x5F, 0x30, 0x04, 0x3F, 0x10, 0x35, 0x10, 0x06, 0x2F,
    0x00, 0x15, 0x11, 0x35, 0x00, 0x06, 0x1F, 0x00, 0x15, 0x11, 0x55, 0x00,
    0x04, 0x1F, 0x00, 0x95, 0x00, 0x08, 0x0F, 0x00, 0x25, 0x10, 0x15, 0x10,
    0x25, 0x00, 0x0A, 0x0F, 0x00, 0x25, 0x00, 0x01, 0x15, 0x00, 0x01, 0x25,
    0x00, 0x08, 0x0F, 0x00, 0x25, 0x10, 0x15, 0x10, 0x25, 0x00, 0x03, 0x00,
    0xD5, 0x00, 0x05, 0x00, 0x45, 0x36, 0x45, 0x00, 0x05, 0x00, 0x06, 0xB5,
    0x06, 0x00, 0x02, 0x0F, 0xD0
};

/* One image, four teams: swap the two body colours. And a flash that
 * turns every opaque pixel white. */
static uint8_t team[4][16], flash[16];

static const uint16_t RAINBOW[12] = {
    0xF904, 0xFC04, 0xFF04, 0x87E4, 0x27E8, 0x27F8, 0x273F, 0x241F,
    0x411F, 0xA11F, 0xF91C, 0xF910
};

struct Blob { int16_t x, vx; int16_t hop; uint8_t team, hit; };
static Blob blobs[5];

static const int TOP = 10;                  /* the HUD's rows */
static uint8_t floorA[GFX_FB_STRIDE] __attribute__((aligned(4)));
static uint8_t floorB[GFX_FB_STRIDE] __attribute__((aligned(4)));

static uint32_t frame = 0, bannerAt = 0, lastFps = 0, fpsCount = 0, fps = 0;
static uint8_t shake = 0;

static void makeFloor() {
    /* Two pattern rows, 8 px squares, one the other's inverse. */
    for (int x = 0; x < GFX_W; x += 2) {
        uint8_t a = ((x >> 3) & 1) ? FELT : FELT_DK;
        uint8_t b = ((x >> 3) & 1) ? FELT_DK : FELT;
        floorA[x >> 1] = (uint8_t)(a | a << 4);
        floorB[x >> 1] = (uint8_t)(b | b << 4);
    }
}

static int isin(int a) {                    /* coarse integer sine, -64..64 */
    a &= 255;
    int q = a & 63, v = q * (128 - q) / 64;  /* a parabola: close enough for bobbing */
    if (a & 64) v = (64 - q) * (64 + q) / 64;
    return (a & 128) ? -v : v;
}

void setup() {
    Gfx.begin(GFX_DIV2, GFX_12BPP);
    Gfx.setPalette(palette, 16);
    Gfx.setFade(255);                       /* start black, fade in */
    makeFloor();

    static const uint8_t body[4][2] = { {RED, RED_DK}, {BLUE, BLUE_DK}, {GREEN, GREEN_DK}, {GOLD, GOLD_DK} };
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 16; i++) team[t][i] = (uint8_t)i;
        team[t][RED] = body[t][0];
        team[t][RED_DK] = body[t][1];
    }
    for (int i = 0; i < 16; i++) flash[i] = WHITE;

    for (int i = 0; i < 5; i++)
        blobs[i] = { (int16_t)(8 + i * 22), (int16_t)((i & 1) ? 1 : -1), (int16_t)(i * 40), (uint8_t)(i & 3), 0 };
}

static void drawPlayfield() {
    Gfx.setClip(0, TOP, GFX_W, GFX_H - TOP);

    /* Floor: a pattern row per 8-row band, stamped at word speed. */
    int scroll = frame >> 1;
    for (int y = TOP; y < GFX_H; y++)
        Gfx.copyRow(y, ((y + scroll) >> 3) & 1 ? floorA : floorB, 0, GFX_W);

    /* Hopping slimes: shadow first, on the ground, then the slime. */
    for (Blob &b : blobs) {
        int lift = isin(b.hop) / 3;
        if (lift < 0) lift = -lift;
        int ground = 84;
        Gfx.fillEllipse(b.x + 8, ground + 11, 7 - lift / 4, 2, INK);
        Gfx.drawSprite4(SLIME, b.x, ground - lift, b.hit ? flash : team[b.team]);
    }

    /* One zooms, one tumbles. */
    int s = 384 + isin((int)(frame * 3)) * 2;         /* 1.0x .. 2.0x, Q8 */
    int w = (16 * s) >> 8, h = (12 * s) >> 8;
    Gfx.drawSprite4(SLIME, 34 - w / 2, 44 - h / 2, team[1], s);
    Gfx.drawSprite4Rot(SLIME, 8, 6, 96, 42, (uint8_t)(frame * 2), 320, team[3]);

    if (shake) {
        /* Post-process: move everything below the HUD. */
        int d = (shake & 2) ? 2 : -2;
        Gfx.scroll(TOP, GFX_H - TOP, d, (shake & 1) ? 1 : -1, INK);
    }
    Gfx.resetClip();
}

static void drawHud() {
    Gfx.fillRect(0, 0, GFX_W, TOP, NAVY);
    Gfx.setFont(&CHGfx_Tiny3x5);
    char buf[24];
    snprintf(buf, sizeof buf, "SCORE %06u", (unsigned)(frame * 7));
    Gfx.print(2, 2 + Gfx.fontBaseline(), buf, WHITE);
    snprintf(buf, sizeof buf, "%u FPS", (unsigned)fps);
    Gfx.print(GFX_W - 2 - Gfx.textWidth(buf), 2 + Gfx.fontBaseline(), buf, FX_B);
}

static void drawPanel() {
    Gfx.dither(0, 100, GFX_W, 28, INK, 0);
    Gfx.fillRoundRect(16, 104, 96, 20, 4, NAVY);
    Gfx.drawRoundRect(16, 104, 96, 20, 4, FX_B);
    Gfx.setFont(&CHGfx_Tiny3x5);
    const char *msg = "PRESS A";
    Gfx.print(64 - Gfx.textWidth(msg, 2) / 2, 108 + 2 * Gfx.fontBaseline(), msg, FX_A, 2);
}

static void drawBanner(uint32_t age) {
    /* Outlined, shadowed, a gold-to-white ramp down the letters, and each
     * letter bobbing on its own phase. */
    static const char text[] = "GAME KIT!";
    static uint8_t ramp[32];
    int8_t dy[sizeof text];
    for (unsigned i = 0; i < sizeof text; i++) dy[i] = (int8_t)(isin((int)(age * 8 + i * 28)) / 16);
    for (int i = 0; i < 32; i++) ramp[i] = i < 6 ? WHITE : (i < 11 ? FX_B : GOLD_DK);
    Gfx.setFont(&CHGfx_Tiny3x5);
    const uint8_t scale = 3;
    int w = Gfx.textWidth(text, scale);
    Gfx.printFx(64 - w / 2, 52 + scale * Gfx.fontBaseline() / 2, text, scale, GOLD, INK, RED_DK, ramp, dy);
}

void loop() {
    /* Everything that does not touch the framebuffer happens while the
     * previous frame is still going out. */
    frame++;
    for (Blob &b : blobs) {
        b.x += b.vx;
        if (b.x < 0 || b.x > GFX_W - 16) { b.vx = -b.vx; b.x += 2 * b.vx; }
        b.hop += 6;
        if (b.hit) b.hit--;
    }
    if (digitalRead(PIN_BTN_A) == LOW || frame - bannerAt > 240) {
        if (frame - bannerAt > 60) {
            bannerAt = frame;
            shake = 12;
            blobs[frame % 5].hit = 8;
        }
    }
    if (shake) shake--;

    /* Palette animation: two slots, recoloured every frame. Staged, so it
     * is safe even though the last frame may still be going out. */
    Gfx.setPaletteEntry(FX_A, RAINBOW[(frame / 4) % 12]);
    int t = frame & 31, tri = t > 15 ? 31 - t : t;
    Gfx.setPaletteEntry(FX_B, Gfx.rgb(255, (uint8_t)(200 + tri * 3), (uint8_t)(tri * 16)));

    /* Fade in at the start, and out and back in every 12 seconds. */
    uint32_t cyc = frame % 720;
    int fade = frame < 32 ? 255 - (int)frame * 8 : (cyc >= 680 ? (int)(cyc - 680) * 6 : 0);
    if (cyc >= 700) fade = 255 - (int)(cyc - 700) * 12;
    Gfx.setFade((uint8_t)(fade < 0 ? 0 : (fade > 255 ? 255 : fade)));

    fpsCount++;
    if (millis() - lastFps >= 1000) { fps = fpsCount; fpsCount = 0; lastFps = millis(); }

    Gfx.wait();                             /* the framebuffer is ours again */
    drawPlayfield();
    drawHud();
    drawPanel();
    if (frame - bannerAt < 90) drawBanner(frame - bannerAt);
    Gfx.displayAsync();
}
