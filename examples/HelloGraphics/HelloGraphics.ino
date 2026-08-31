/*
 * HelloGraphics - the smallest useful CHGfx sketch.
 *
 * CHGfx keeps a 16-colour, 4 bpp framebuffer in SRAM. You draw into it,
 * then call display() once and the whole frame goes out over DMA in a
 * single burst. Nothing reaches the panel until you ask.
 *
 * Board: CHGame (CH32X035G8U6) + ST7735 128x128
 * Set Tools > Optimize > Faster (-O2).
 */
#include <CHGfx.h>

/* Colours are PALETTE INDICES, 0..15 - not RGB565. Naming them keeps
 * the drawing code readable. */
enum : uint8_t {
    BLACK = 0, DARKGREY, GREY, LIGHTGREY, WHITE,
    RED, ORANGE, YELLOW, GREEN, DARKGREEN,
    CYAN, BLUE, NAVY, MAGENTA, PURPLE, PINK
};

static const uint16_t palette[16] = {
    0x0000, 0x18E3, 0x4208, 0xC618, 0xFFFF,
    0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x0400,
    0x07FF, 0x001F, 0x0010, 0xF81F, 0x8010, 0xFC9F
};

void setup()
{
    Gfx.begin();                    // 24 MHz SPI, 16 bpp output
    Gfx.setPalette(palette, 16);

    Gfx.clear(NAVY);
    Gfx.drawRect(0, 0, 128, 128, WHITE);
    Gfx.print(8, 10, "Hello, CH32X035", YELLOW);
    Gfx.print(8, 24, "CHGfx", CYAN, 2);          // scale 2

    Gfx.fillCircle(40, 74, 22, RED);
    Gfx.fillCircle(78, 74, 22, GREEN);
    Gfx.drawLine(8, 108, 120, 108, ORANGE);

    /* The 16-colour ramp along the bottom. */
    for (uint8_t i = 0; i < 16; i++)
        Gfx.fillRect(i * 8, 114, 8, 10, i);

    Gfx.display();                  // one DMA burst, ~11 ms
}

void loop()
{
    /* A blinking cursor, redrawing only the 8x12 box it lives in.
     * Sending 96 bytes instead of 32768 is the whole idea behind
     * displayRect(): cost scales with area, not with cleverness. */
    static bool on = false;
    on = !on;
    Gfx.fillRect(112, 10, 8, 12, on ? WHITE : NAVY);
    Gfx.displayRect(112, 10, 8, 12);
    delay(400);
}
