/*
 * AdafruitGFXCompat - keep your Adafruit_GFX drawing code, drop the
 * slow transport.
 *
 * Adafruit_GFX is not what makes an ST7735 sketch slow. Adafruit_ST7735
 * underneath it is: it pushes every pixel down a polled SPI link with a
 * fresh address window each time, which measures about 2.4 fps for a
 * full screen on this chip.
 *
 * CHGfx_GFX subclasses Adafruit_GFX and points it at CHGfx's 4 bpp
 * framebuffer. You keep print(), setTextSize(), custom GFXfonts,
 * drawBitmap(), triangles, rounded rects - all of it - and get the DMA
 * transport underneath. Same API, ~37x the frame rate.
 *
 * THE ONE DIFFERENCE: colours are palette indices 0..15, not RGB565.
 * Map real colours once at setup with nearest(), or define
 * CHGFX_GFX_AUTOMAP to have it done on every call (convenient, but it
 * costs a 16-entry search per drawing call).
 *
 * Requires the Adafruit GFX Library to be installed.
 *
 * Board: CHGame (CH32X035G8U6) + ST7735 128x128
 * Set Tools > Optimize > Faster (-O2).
 */
/* Adafruit_GFX.h must be included here in the .ino, not just inside the
 * bridge header. The Arduino builder decides which library include
 * paths to add by scanning the sketch itself, so a header that is only
 * reachable through another header does not get resolved. */
#include <Adafruit_GFX.h>
#include <CHGfx.h>
#include <CHGfx_AdafruitGFX.h>

CHGfx_GFX tft;

static const uint16_t palette[16] = {
    0x0000, 0x18E3, 0x4208, 0xC618, 0xFFFF,
    0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x0400,
    0x07FF, 0x001F, 0x0010, 0xF81F, 0x8010, 0xFC9F
};

/* Map the RGB565 constants you already use onto palette slots, once. */
static uint8_t BLACK, WHITE, RED, YELLOW, CYAN, NAVY, GREEN;

void setup()
{
    Serial.begin();

    tft.begin();
    tft.setPalette(palette, 16);

    BLACK  = tft.nearest(0x0000);
    WHITE  = tft.nearest(0xFFFF);
    RED    = tft.nearest(0xF800);
    YELLOW = tft.nearest(0xFFE0);
    CYAN   = tft.nearest(0x07FF);
    NAVY   = tft.nearest(0x000F);
    GREEN  = tft.nearest(0x07E0);

    /* From here down this is ordinary Adafruit_GFX code. */
    tft.fillScreen(NAVY);
    tft.drawRoundRect(2, 2, 124, 124, 8, WHITE);

    tft.setTextColor(YELLOW);
    tft.setTextSize(1);
    tft.setCursor(10, 12);
    tft.print("Adafruit_GFX API");

    tft.setTextColor(CYAN);
    tft.setCursor(10, 24);
    tft.print("CHGfx transport");

    tft.fillTriangle(20, 60, 44, 100, 64, 60, RED);
    tft.drawCircle(92, 76, 22, GREEN);
    tft.fillRect(76, 104, 40, 12, WHITE);

    tft.setTextColor(BLACK);
    tft.setCursor(80, 106);
    tft.print("fast");

    tft.display();          // nothing reached the panel until now
    delay(1500);
}

void loop()
{
    /* A spinning line, timed. Note the shape of the loop: draw the whole
     * frame into RAM, then present once. With Adafruit_ST7735 every one
     * of these calls would have gone down the wire immediately. */
    static uint16_t a = 0;
    static uint32_t frames = 0, last = 0;

    a += 4;
    float rad = a * 0.0174533f;

    tft.fillScreen(NAVY);
    tft.drawRoundRect(2, 2, 124, 124, 8, WHITE);
    for (int i = 0; i < 6; i++) {
        float r2 = rad + i * 1.0472f;
        tft.drawLine(64, 64,
                     64 + (int)(56.0f * cosf(r2)),
                     64 + (int)(56.0f * sinf(r2)),
                     (uint8_t)(RED + i));
    }
    tft.setTextColor(YELLOW);
    tft.setCursor(6, 6);
    tft.print("CHGfx_GFX");
    tft.display();

    frames++;
    if (millis() - last >= 1000) {
        Serial.print(frames);
        Serial.println(" fps");
        frames = 0;
        last = millis();
    }
}
