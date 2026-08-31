/*
 * PartialUpdate - the technique that actually buys you frame rate.
 *
 * At 24 MHz a full 128x128 RGB565 frame is 32768 bytes and takes 11 ms
 * to shift out, so 90 fps is the hard ceiling no matter how fast your
 * drawing code is. But the panel does not care which rectangle you
 * send. Push only what moved and the cost scales with AREA:
 *
 *      128x128   32768 B   11.1 ms    90 fps
 *       64x64     8192 B    2.8 ms   354 fps
 *       32x32     2048 B    0.75 ms 1336 fps
 *
 * This sketch bounces a sprite over a static background and sends only
 * the union of where it was and where it is now. Press any key in the
 * Serial Monitor to toggle between full-frame and dirty-rect presenting
 * and watch the reported frame rate change.
 *
 * Board: CHGame (CH32X035G8U6) + ST7735 128x128
 * Set Tools > Optimize > Faster (-O2).
 */
#include <CHGfx.h>

enum : uint8_t { BLACK = 0, DARKGREY, GREY, LIGHTGREY, WHITE,
                 RED, ORANGE, YELLOW, GREEN, DARKGREEN,
                 CYAN, BLUE, NAVY, MAGENTA, PURPLE, PINK };

static const uint16_t palette[16] = {
    0x0000, 0x18E3, 0x4208, 0xC618, 0xFFFF,
    0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x0400,
    0x07FF, 0x001F, 0x0010, 0xF81F, 0x8010, 0xFC9F
};

/* 16x16 4 bpp sprite: 2 px per byte, even x in the low nibble. */
static uint8_t ball[16 * 8];

static void makeBall(void)
{
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x += 2) {
            int d0 = (x - 8) * (x - 8) + (y - 8) * (y - 8);
            int d1 = (x - 7) * (x - 7) + (y - 8) * (y - 8);
            uint8_t lo = d0 < 49 ? (uint8_t)(RED + (d0 >> 4)) : 0;
            uint8_t hi = d1 < 49 ? (uint8_t)(RED + (d1 >> 4)) : 0;
            ball[y * 8 + (x >> 1)] = (uint8_t)(lo | (hi << 4));
        }
    }
}

/* The static scene the sprite moves over. Redrawn from scratch each
 * frame - it is only ~100 us, and it keeps the example simple. */
static void drawBackground(void)
{
    Gfx.clear(NAVY);
    for (int y = 0; y < 128; y += 16)
        for (int x = 0; x < 128; x += 16)
            if (((x ^ y) >> 4) & 1) Gfx.fillRect(x, y, 16, 16, DARKGREEN);
    Gfx.drawRect(0, 0, 128, 128, GREY);
}

static bool useDirtyRect = true;
static int  bx = 20, by = 30, vx = 2, vy = 3;

void setup()
{
    Serial.begin();
    makeBall();
    Gfx.begin(GFX_DIV2, GFX_16BPP);
    Gfx.setPalette(palette, 16);
    drawBackground();
    Gfx.display();
}

void loop()
{
    static uint32_t frames = 0, last = 0;

    if (Serial.available()) {
        while (Serial.available()) Serial.read();
        useDirtyRect = !useDirtyRect;
        Serial.println(useDirtyRect ? "-> dirty rect" : "-> full frame");
    }

    /* Remember where the sprite was, then move it. */
    int ox = bx, oy = by;
    bx += vx; by += vy;
    if (bx < 1 || bx > 111) { vx = -vx; bx += vx; }
    if (by < 1 || by > 111) { vy = -vy; by += vy; }

    drawBackground();
    Gfx.drawSprite(ball, bx & ~1, by, 16, 16, /*transparent=*/0);

    if (useDirtyRect) {
        /* Union of the old and new sprite boxes, one pixel of slack on
         * each side. x and width get rounded outward to a multiple of 2
         * inside displayRect(), because two pixels share a byte. */
        int x0 = (ox < bx ? ox : bx) - 1;
        int y0 = (oy < by ? oy : by) - 1;
        int x1 = (ox > bx ? ox : bx) + 17;
        int y1 = (oy > by ? oy : by) + 17;
        Gfx.displayRect(x0, y0, x1 - x0, y1 - y0);
    } else {
        Gfx.display();
    }

    frames++;
    if (millis() - last >= 1000) {
        Serial.print(useDirtyRect ? "dirty rect: " : "full frame: ");
        Serial.print(frames);
        Serial.println(" fps");
        frames = 0;
        last = millis();
    }
}
