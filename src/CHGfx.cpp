/*
 * chgfx.cpp - SPI1 + DMA1_CH3 + ST7735 transport for the CHGame board.
 *
 * Pin roles come from variants/CH32X035/CHGame/variant_CHGame.h:
 *   PA5 = SCK, PA7 = MOSI, PA6 = MISO (unused by the panel)
 *   PA4 = LCD_CS, PB0 = LCD_DC, PB12 = LCD_RST, PB11 = SD_CS
 * The panel and the microSD slot share SPI1, so SD_CS is parked high.
 */
#include "CHGfx.h"

extern "C" {
#include "ch32x035.h"
}

/* ------------------------------------------------------------------ */
/* Raw pin definitions - we bypass the Arduino pin layer entirely on   */
/* the hot path, because digitalWrite() on this core walks a pin map.  */
/* The ports and pin numbers come from CHGfx.h so a different board    */
/* can remap them with -D flags.                                       */
/* ------------------------------------------------------------------ */
#define PIN_CS_PORT   CHGFX_CS_PORT
#define PIN_CS_BIT    (1u << CHGFX_CS_PIN)
#define PIN_DC_PORT   CHGFX_DC_PORT
#define PIN_DC_BIT    (1u << CHGFX_DC_PIN)
#define PIN_RST_PORT  CHGFX_RST_PORT
#define PIN_RST_BIT   (1u << CHGFX_RST_PIN)
#define PIN_SDCS_PORT CHGFX_SDCS_PORT
#define PIN_SDCS_BIT  (1u << CHGFX_SDCS_PIN)

/* Configure one pin's 4-bit CFGLR/CFGHR field without disturbing the
 * other 7 pins in that register. */
static inline void cfgPin(GPIO_TypeDef *port, uint8_t pin, uint32_t nibble) {
    volatile uint32_t *reg = (pin < 8) ? &port->CFGLR : &port->CFGHR;
    uint8_t sh = (uint8_t)((pin & 7) * 4);
    *reg = (*reg & ~(0xFu << sh)) | (nibble << sh);
}

/* BSHR sets, BCR clears - single-cycle, no read-modify-write. */
#define CS_LOW()    (PIN_CS_PORT->BCR  = PIN_CS_BIT)
#define CS_HIGH()   (PIN_CS_PORT->BSHR = PIN_CS_BIT)
#define DC_CMD()    (PIN_DC_PORT->BCR  = PIN_DC_BIT)
#define DC_DATA()   (PIN_DC_PORT->BSHR = PIN_DC_BIT)

/* GPIO CFGx nibble encodings (RM 8.3.1.1):
 *   CNF=00 MODE=11 -> 0x3 general-purpose push-pull, max drive
 *   CNF=10 MODE=11 -> 0xB alternate-function push-pull, max drive     */
#define CFG_GP_PP    0x3u
#define CFG_AF_PP    0xBu

/* SPI1 CTLR1 bits */
#define SPI_CPHA     (1u << 0)
#define SPI_CPOL     (1u << 1)
#define SPI_MSTR     (1u << 2)
#define SPI_SPE      (1u << 6)
#define SPI_SSI      (1u << 8)
#define SPI_SSM      (1u << 9)
#define SPI_DFF16    (1u << 11)
#define SPI_BIDIMODE (1u << 15)
#define SPI_BIDIOE   (1u << 14)

/* SPI1 STATR bits */
#define SPI_TXE      (1u << 1)
#define SPI_BSY      (1u << 7)

/* DMA channel CFGR bits (RM ch.9) */
#define DMA_EN       (1u << 0)
#define DMA_TCIE     (1u << 1)
#define DMA_DIR_M2P  (1u << 4)
#define DMA_MINC     (1u << 7)
#define DMA_PSIZE_16 (1u << 8)
#define DMA_MSIZE_16 (1u << 10)
#define DMA_PL_VHIGH (3u << 12)

/* DMA1 interrupt flags for channel 3 live at bits 8..11 of INTFR/INTFCR. */
#define DMA_CH3_GIF  (1u << 8)
#define DMA_CH3_TCIF (1u << 9)
#define DMA_CH3_ALL  (0xFu << 8)

/* ------------------------------------------------------------------ */
/* ST7735 command set (only what we use)                               */
/* ------------------------------------------------------------------ */
#define ST_SWRESET 0x01
#define ST_SLPOUT  0x11
#define ST_NORON   0x13
#define ST_INVOFF  0x20
#define ST_INVON   0x21
#define ST_DISPON  0x29
#define ST_CASET   0x2A
#define ST_RASET   0x2B
#define ST_RAMWR   0x2C
#define ST_MADCTL  0x36
#define ST_COLMOD  0x3A
#define ST_FRMCTR1 0xB1
#define ST_FRMCTR2 0xB2
#define ST_FRMCTR3 0xB3
#define ST_INVCTR  0xB4
#define ST_PWCTR1  0xC0
#define ST_PWCTR2  0xC1
#define ST_PWCTR3  0xC2
#define ST_PWCTR4  0xC3
#define ST_PWCTR5  0xC4
#define ST_VMCTR1  0xC5
#define ST_GMCTRP1 0xE0
#define ST_GMCTRN1 0xE1

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
uint8_t  gfx_fb[GFX_FB_BYTES];
uint16_t gfx_pal[16];

static uint8_t  s_div      = GFX_DIV2;
static uint8_t  s_mode     = GFX_16BPP;
static uint8_t  s_madctl   = 0xC8;   /* INITR_144GREENTAB, rotation 0 */
static uint8_t  s_colStart = 2;
static uint8_t  s_rowStart = 3;

/* Expansion LUT: one framebuffer byte (2 px) -> packed output.
 *   16 bpp: entry = pal565[lo] | pal565[hi] << 16   (one uint32 store)
 *   12 bpp: entry = 3 packed RGB444 bytes in the low 24 bits          */
static uint32_t s_lut[256];

/* Ping-pong chunk buffers. GFX_CHUNK_BYTES is sized for the 16 bpp
 * worst case; 12 bpp uses 3/4 of it. */
static uint8_t s_chunk[2][GFX_CHUNK_BYTES] __attribute__((aligned(4)));

static volatile int8_t   s_sending;   /* buffer index in flight, -1 idle   */
static volatile int8_t   s_ready;     /* converted buffer waiting, -1 none */
static volatile uint32_t s_readyLen;  /* bytes in the waiting buffer       */

/* Anything the DMA ISR touches must not be a flash constant on a hot
 * path; keep the hot converters in SRAM (see RAMFUNC below). */
/* Put the hot loops in SRAM. The section name looks odd on purpose:
 * the core's link script routes *(.srodata .srodata.*) into .data,
 * which has a RAM VMA and a flash LMA, so startup copies it for us.
 * A ".data.*" name works identically but makes the assembler warn
 * about section attributes, and ".gnu.linkonce.*" would let the
 * linker discard all but one of these as duplicates. */
#define RAMFUNC __attribute__((section(".srodata.ramfunc"), noinline))

/* ================================================================== */
/* Low-level SPI                                                       */
/* ================================================================== */

static inline void spiWait(void) {
    while (!(SPI1->STATR & SPI_TXE)) { }
    while (SPI1->STATR & SPI_BSY)    { }
}

static inline void spiSet8(void) {
    if (SPI1->CTLR1 & SPI_DFF16) {
        SPI1->CTLR1 &= ~SPI_SPE;
        SPI1->CTLR1 &= ~SPI_DFF16;
        SPI1->CTLR1 |= SPI_SPE;
    }
}

static inline void spiSet16(void) {
    if (!(SPI1->CTLR1 & SPI_DFF16)) {
        SPI1->CTLR1 &= ~SPI_SPE;
        SPI1->CTLR1 |= SPI_DFF16;
        SPI1->CTLR1 |= SPI_SPE;
    }
}

static inline void spiByte(uint8_t b) {
    while (!(SPI1->STATR & SPI_TXE)) { }
    SPI1->DATAR = b;
}

/* Chip select is exposed rather than wrapped around every command,
 * because the whole point of this driver is to assert it once and stream
 * a frame. The panel latches nothing while CS is high. */
void gfx_select(void)   { CS_LOW(); }
void gfx_deselect(void) { spiWait(); CS_HIGH(); }

void gfx_cmd(uint8_t c) {
    spiWait();
    spiSet8();
    DC_CMD();
    spiByte(c);
    spiWait();
    DC_DATA();
}

void gfx_data8(uint8_t d) {
    spiSet8();
    spiByte(d);
}

/* ================================================================== */
/* Bring-up                                                            */
/* ================================================================== */

static void gpioInit(void) {
    /* Clock all three GPIO ports: the control pins are remappable and we
     * do not know which port they landed on. Costs a few microamps. */
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                      RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1;
    RCC->AHBPCENR  |= RCC_AHBPeriph_DMA1;

    /* SCK and MOSI are SPI1's fixed pins: PA5 and PA7, alternate-function
     * push-pull. PA6 (MISO) is left at its reset default - the panel has
     * no SDO line on this flex. */
    cfgPin(GPIOA, 5, CFG_AF_PP);
    cfgPin(GPIOA, 7, CFG_AF_PP);

    /* Control pins, general-purpose push-pull. */
    cfgPin(PIN_CS_PORT,  CHGFX_CS_PIN,  CFG_GP_PP);
    cfgPin(PIN_DC_PORT,  CHGFX_DC_PIN,  CFG_GP_PP);
    cfgPin(PIN_RST_PORT, CHGFX_RST_PIN, CFG_GP_PP);

    CS_HIGH();
    DC_DATA();
    PIN_RST_PORT->BSHR = PIN_RST_BIT;

#ifndef CHGFX_NO_SD_PARK
    /* The microSD slot shares SPI1; park its chip select high so it does
     * not answer the traffic meant for the panel. */
    cfgPin(PIN_SDCS_PORT, CHGFX_SDCS_PIN, CFG_GP_PP);
    PIN_SDCS_PORT->BSHR = PIN_SDCS_BIT;
#endif
}

/* BR[2:0] in CTLR1[5:3]: 000 = /2, 001 = /4, 010 = /8, ... */
static uint16_t brBits(uint8_t div) {
    switch (div) {
        case 2:  return 0u << 3;
        case 4:  return 1u << 3;
        case 8:  return 2u << 3;
        case 16: return 3u << 3;
        case 32: return 4u << 3;
        default: return 0u << 3;
    }
}

static void spiInit(uint8_t div) {
    SPI1->CTLR1 = 0;
    /* Master, mode 0 (ST7735 samples on the rising edge), software NSS,
     * 8-bit frames to start with, MSB first. */
    SPI1->CTLR1 = SPI_MSTR | SPI_SSM | SPI_SSI | brBits(div);
    SPI1->CTLR2 = 0;
    SPI1->CTLR1 |= SPI_SPE;
    s_div = div;
}

static void dmaInit(void) {
    /* Channel 3 is the SPI1_TX request line (RM 9.2.3). Peripheral
     * address never changes, so set it once. */
    DMA1_Channel3->CFGR  = 0;
    DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
    NVIC_SetPriority(DMA1_Channel3_IRQn, 0);
    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

static void panelReset(void) {
    PIN_RST_PORT->BSHR = PIN_RST_BIT; delay(5);
    PIN_RST_PORT->BCR  = PIN_RST_BIT; delay(20);
    PIN_RST_PORT->BSHR = PIN_RST_BIT; delay(150);
}

/* FRMCTR values. The ST7735 frame rate is
 *   fps = 850kHz / ((RTNA*2+40) * (LINE + FPA + BPA))
 * The stock Adafruit table uses 0x01,0x2C,0x2D which is a lazy ~60 Hz.
 * 0x05,0x3A,0x3A scans the glass considerably faster, which shortens the
 * gap between a GRAM write landing and the pixel actually changing. */
static uint8_t s_frm[3] = { 0x05, 0x3A, 0x3A };

void gfx_setPanelFrameRate(uint8_t rtna, uint8_t fpa, uint8_t bpa) {
    s_frm[0] = rtna; s_frm[1] = fpa; s_frm[2] = bpa;
    gfx_wait();
    CS_LOW();
    gfx_cmd(ST_FRMCTR1);
    gfx_data8(rtna); gfx_data8(fpa); gfx_data8(bpa);
    spiWait();
    CS_HIGH();
}

static void panelInit(void) {
    CS_LOW();

    gfx_cmd(ST_SWRESET); spiWait(); delay(150);
    gfx_cmd(ST_SLPOUT);  spiWait(); delay(255);

    gfx_cmd(ST_FRMCTR1); gfx_data8(s_frm[0]); gfx_data8(s_frm[1]); gfx_data8(s_frm[2]);
    gfx_cmd(ST_FRMCTR2); gfx_data8(s_frm[0]); gfx_data8(s_frm[1]); gfx_data8(s_frm[2]);
    gfx_cmd(ST_FRMCTR3);
    gfx_data8(s_frm[0]); gfx_data8(s_frm[1]); gfx_data8(s_frm[2]);
    gfx_data8(s_frm[0]); gfx_data8(s_frm[1]); gfx_data8(s_frm[2]);

    gfx_cmd(ST_INVCTR);  gfx_data8(0x07);

    gfx_cmd(ST_PWCTR1);  gfx_data8(0xA2); gfx_data8(0x02); gfx_data8(0x84);
    gfx_cmd(ST_PWCTR2);  gfx_data8(0xC5);
    gfx_cmd(ST_PWCTR3);  gfx_data8(0x0A); gfx_data8(0x00);
    gfx_cmd(ST_PWCTR4);  gfx_data8(0x8A); gfx_data8(0x2A);
    gfx_cmd(ST_PWCTR5);  gfx_data8(0x8A); gfx_data8(0xEE);
    gfx_cmd(ST_VMCTR1);  gfx_data8(0x0E);

    gfx_cmd(ST_INVOFF);
    gfx_cmd(ST_MADCTL);  gfx_data8(s_madctl);

    /* Gamma. Purely cosmetic, but the panel looks washed out without it. */
    static const uint8_t gp[16] = { 0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,
                                    0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10 };
    static const uint8_t gn[16] = { 0x03,0x1d,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                                    0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10 };
    gfx_cmd(ST_GMCTRP1); for (uint8_t i = 0; i < 16; i++) gfx_data8(gp[i]);
    gfx_cmd(ST_GMCTRN1); for (uint8_t i = 0; i < 16; i++) gfx_data8(gn[i]);

    gfx_cmd(ST_NORON);  spiWait(); delay(10);
    gfx_cmd(ST_DISPON); spiWait(); delay(100);

    CS_HIGH();
}

/*
 * RGBSET (2Dh) - the colour-depth conversion LUT.
 *
 * The ST7735S datasheet (10.1.23) lists the default contents of this
 * table as "Random" after both power-on and hardware reset, and says
 * "128-Bytes must be written to the LUT regardless of the color mode".
 * Every ST7735 driver in the wild ignores that and 16 bpp works anyway,
 * so the silicon evidently ships a sane 5-6-5 default - but 12 bpp is
 * exercised by almost nobody, so we do not assume the 4-bit half of the
 * table is populated. Writing it costs 129 bytes once.
 *
 * Layout (datasheet 9.18.2, 128 parameters):
 *   params   1..32  RED,   indexed by the input red value
 *   params  33..96  GREEN
 *   params  97..128 BLUE
 * Each parameter is a 6-bit frame-memory value. In 4 bpp-input (12 bit)
 * mode only the first 16 entries of each colour are consulted; in 5-6-5
 * mode red/blue use 32 and green uses 64.
 */
static bool s_writeLut12 = true;

void gfx_writeColorLut(void) {
    CS_LOW();
    gfx_cmd(0x2D);
    if (s_mode == GFX_12BPP) {
        /* 4-bit input -> 6-bit output: replicate the top 2 bits down. */
        for (uint8_t i = 0; i < 32; i++)
            gfx_data8(i < 16 ? (uint8_t)((i << 2) | (i >> 2)) : 0x3F);   /* R */
        for (uint8_t i = 0; i < 64; i++)
            gfx_data8(i < 16 ? (uint8_t)((i << 2) | (i >> 2)) : 0x3F);   /* G */
        for (uint8_t i = 0; i < 32; i++)
            gfx_data8(i < 16 ? (uint8_t)((i << 2) | (i >> 2)) : 0x3F);   /* B */
    } else {
        for (uint8_t i = 0; i < 32; i++)
            gfx_data8((uint8_t)((i << 1) | (i >> 4)));                   /* R, 5-bit */
        for (uint8_t i = 0; i < 64; i++)
            gfx_data8(i);                                                /* G, 6-bit */
        for (uint8_t i = 0; i < 32; i++)
            gfx_data8((uint8_t)((i << 1) | (i >> 4)));                   /* B, 5-bit */
    }
    spiWait();
    CS_HIGH();
}

void gfx_setWriteColorLut(bool on) { s_writeLut12 = on; }

static void applyColorMode(void) {
    CS_LOW();
    gfx_cmd(ST_COLMOD);
    gfx_data8(s_mode == GFX_12BPP ? 0x03 : (s_mode == GFX_18BPP ? 0x06 : 0x05));
    spiWait();
    CS_HIGH();
    /* Only for 12 bpp: 16 bpp demonstrably works on the factory default
     * and there is no reason to poke a table that is already right. */
    if (s_mode == GFX_12BPP && s_writeLut12) gfx_writeColorLut();
}

void gfx_begin(uint8_t spiDiv, uint8_t colorMode) {
    gpioInit();
    spiInit(spiDiv);
    dmaInit();
    panelReset();
    s_mode = colorMode;
    panelInit();
    applyColorMode();

    /* A sane default 16-colour ramp so a fresh sketch shows something. */
    static const uint16_t defpal[16] = {
        0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
        0x8410, 0xC618, 0x7800, 0x03E0, 0x000F, 0x8400, 0x0410, 0x4208
    };
    gfx_setPalette(defpal, 16);

    s_sending = -1;
    s_ready   = -1;
}

void gfx_setSpiDiv(uint8_t div) {
    gfx_wait();
    SPI1->CTLR1 &= ~SPI_SPE;
    SPI1->CTLR1 = (SPI1->CTLR1 & ~(7u << 3)) | brBits(div);
    SPI1->CTLR1 |= SPI_SPE;
    s_div = div;
}

void gfx_setColorMode(uint8_t mode) {
    if (mode == s_mode) return;
    gfx_wait();
    s_mode = mode;
    applyColorMode();
    gfx_setPalette(gfx_pal, 16);   /* LUT layout depends on the mode */
}

uint8_t  gfx_colorMode(void) { return s_mode; }
uint8_t  gfx_spiDiv(void)    { return s_div; }
uint32_t gfx_spiHz(void)     { return F_CPU / s_div; }

uint32_t gfx_frameBytes(void) {
    if (s_mode == GFX_12BPP) return (GFX_W * GFX_H * 3u) / 2u;
    if (s_mode == GFX_18BPP) return  GFX_W * GFX_H * 3u;
    return GFX_W * GFX_H * 2u;
}

uint8_t gfx_bytesPerPixel(void) { return s_mode == GFX_18BPP ? 3 : 2; }

uint8_t *gfx_chunkScratch(void) { return &s_chunk[0][0]; }

void gfx_setPanelOffsets(uint8_t madctl, uint8_t colStart, uint8_t rowStart) {
    s_madctl = madctl; s_colStart = colStart; s_rowStart = rowStart;
    CS_LOW();
    gfx_cmd(ST_MADCTL); gfx_data8(s_madctl); spiWait();
    CS_HIGH();
}

void gfx_setInverted(bool on) {
    CS_LOW();
    gfx_cmd(on ? ST_INVON : ST_INVOFF);
    spiWait();
    CS_HIGH();
}

/* ================================================================== */
/* Palette + expansion LUT                                             */
/* ================================================================== */

static void buildLut(void) {
    if (s_mode == GFX_16BPP) {
        /* Entry holds two RGB565 pixels. A single 32-bit store writes
         * both; on this little-endian core the low halfword lands first,
         * which is the even-x pixel, which is what the panel wants. */
        for (uint32_t b = 0; b < 256; b++) {
            uint32_t lo = gfx_pal[b & 0x0F];
            uint32_t hi = gfx_pal[b >> 4];
            s_lut[b] = lo | (hi << 16);
        }
    } else if (s_mode == GFX_18BPP) {
        /* RGB666 is three bytes per PIXEL, so a 256-entry byte->2px
         * table would need six bytes an entry. Use a 16-entry palette
         * indexed by nibble instead and do two lookups per source byte;
         * the budget at 18 bpp is 48 cycles/px, so it costs nothing. */
        for (uint8_t i = 0; i < 16; i++) {
            uint16_t c = gfx_pal[i];
            uint32_t r = (c >> 11) & 0x1F;          /* 5 bits -> 6 */
            uint32_t g = (c >> 5)  & 0x3F;          /* already 6   */
            uint32_t b =  c        & 0x1F;
            r = (r << 1) | (r >> 4);
            b = (b << 1) | (b >> 4);
            /* Component in bits 7:2 of its byte, byte 0 in the LSB. */
            s_lut[i] = ((r << 2) & 0xFF) | (((g << 2) & 0xFF) << 8)
                                         | (((b << 2) & 0xFF) << 16);
        }
    } else {
        /* RGB444: two pixels pack into exactly three bytes.
         *   byte0 = R0G0        byte1 = B0R1        byte2 = G1B1
         * Store them in the low 24 bits, byte 0 in the LSB. */
        uint16_t p12[16];
        for (uint8_t i = 0; i < 16; i++) {
            uint16_t c = gfx_pal[i];
            uint8_t r = (c >> 12) & 0x0F;            /* RGB565 R[4:1] */
            uint8_t g = (c >> 7)  & 0x0F;            /* RGB565 G[5:2] */
            uint8_t bl = (c >> 1) & 0x0F;            /* RGB565 B[4:1] */
            p12[i] = (uint16_t)((r << 8) | (g << 4) | bl);
        }
        for (uint32_t b = 0; b < 256; b++) {
            uint32_t p0 = p12[b & 0x0F];
            uint32_t p1 = p12[b >> 4];
            uint32_t b0 = p0 >> 4;
            uint32_t b1 = ((p0 & 0x0F) << 4) | (p1 >> 8);
            uint32_t b2 = p1 & 0xFF;
            s_lut[b] = b0 | (b1 << 8) | (b2 << 16);
        }
    }
}

void gfx_setPalette(const uint16_t *rgb565, uint8_t count) {
    if (rgb565 != gfx_pal) {
        for (uint8_t i = 0; i < count && i < 16; i++) gfx_pal[i] = rgb565[i];
    }
    buildLut();
}

/* Nearest palette entry, by squared distance in RGB565's own component
 * space. Weighted 2:4:1 after the usual luminance rule of thumb, scaled
 * so the 5/6/5 bit widths do not skew the result toward green. */
uint8_t gfx_nearest(uint16_t rgb565) {
    int r = (rgb565 >> 11) & 0x1F;
    int g = (rgb565 >> 5)  & 0x3F;
    int b =  rgb565        & 0x1F;
    uint8_t best = 0;
    int32_t bestD = 0x7FFFFFFF;
    for (uint8_t i = 0; i < 16; i++) {
        int dr = r - ((gfx_pal[i] >> 11) & 0x1F);
        int dg = g - ((gfx_pal[i] >> 5)  & 0x3F);
        int db = b - ( gfx_pal[i]        & 0x1F);
        int32_t d = 8 * dr * dr + 4 * dg * dg + 4 * db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

/* The single global instance behind the class API. */
CHGfx Gfx;

void gfx_setPaletteEntry(uint8_t index, uint16_t rgb565) {
    gfx_pal[index & 0x0F] = rgb565;
    buildLut();
}

/* ================================================================== */
/* The conversion inner loops                                          */
/* ================================================================== */
/*
 * These run out of SRAM. Flash on the CH32X035 costs 3 wait states at
 * 48 MHz (RM 20.3.1), and while the prefetch buffer hides most of that
 * for straight-line code, a 4-instruction loop body pays for every
 * refill. The benchmark reports both variants so you can see the gap on
 * your own silicon rather than taking my word for it.
 */

/* 16 bpp: one LUT load + one word store per two pixels. */
static inline uint32_t conv565(uint8_t *dst, const uint8_t *src, uint32_t srcBytes)
{
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *lut = s_lut;
    while (srcBytes >= 4) {
        d[0] = lut[src[0]];
        d[1] = lut[src[1]];
        d[2] = lut[src[2]];
        d[3] = lut[src[3]];
        d += 4; src += 4; srcBytes -= 4;
    }
    while (srcBytes--) *d++ = lut[*src++];
    return (uint32_t)((uint8_t *)d - dst);
}

/* 12 bpp: four source bytes (8 px) become twelve output bytes, which is
 * three naturally aligned word stores. Doing it in groups of four keeps
 * every store word-aligned, which matters because the QingKe core does
 * not guarantee cheap unaligned access. */
static inline uint32_t conv444(uint8_t *dst, const uint8_t *src, uint32_t srcBytes)
{
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *lut = s_lut;
    uint32_t groups = srcBytes >> 2;
    while (groups--) {
        uint32_t e0 = lut[src[0]];
        uint32_t e1 = lut[src[1]];
        uint32_t e2 = lut[src[2]];
        uint32_t e3 = lut[src[3]];
        src += 4;
        d[0] = e0 | (e1 << 24);
        d[1] = (e1 >> 8) | (e2 << 16);
        d[2] = (e2 >> 16) | (e3 << 8);
        d += 3;
    }
    return (uint32_t)((uint8_t *)d - dst);
}

/* 18 bpp: two source bytes (4 px) become twelve output bytes, i.e.
 * three aligned word stores - the same packing shuffle as conv444, just
 * fed one pixel per nibble instead of one pixel per half-nibble-pair. */
static inline uint32_t conv666(uint8_t *dst, const uint8_t *src, uint32_t srcBytes)
{
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *pal = s_lut;          /* only [0..15] used here */
    uint32_t pairs = srcBytes >> 1;
    while (pairs--) {
        uint32_t s0 = src[0], s1 = src[1];
        src += 2;
        uint32_t e0 = pal[s0 & 0x0F];
        uint32_t e1 = pal[s0 >> 4];
        uint32_t e2 = pal[s1 & 0x0F];
        uint32_t e3 = pal[s1 >> 4];
        d[0] = e0 | (e1 << 24);
        d[1] = (e1 >> 8) | (e2 << 16);
        d[2] = (e2 >> 16) | (e3 << 8);
        d += 3;
    }
    return (uint32_t)((uint8_t *)d - dst);
}

RAMFUNC uint32_t gfx_convertRows_ram(uint8_t *dst, uint16_t row, uint16_t rows)
{
    const uint8_t *src = gfx_fb + (uint32_t)row * GFX_FB_STRIDE;
    uint32_t n = (uint32_t)rows * GFX_FB_STRIDE;
    if (s_mode == GFX_12BPP) return conv444(dst, src, n);
    if (s_mode == GFX_18BPP) return conv666(dst, src, n);
    return conv565(dst, src, n);
}

/* Arbitrary run of framebuffer bytes -> panel format. This is what the
 * partial-rectangle path uses, since a sub-rectangle's rows are not
 * contiguous in the framebuffer. */
RAMFUNC uint32_t gfx_convertSpan_ram(uint8_t *dst, const uint8_t *src, uint32_t srcBytes)
{
    if (s_mode == GFX_12BPP) return conv444(dst, src, srcBytes);
    if (s_mode == GFX_18BPP) return conv666(dst, src, srcBytes);
    return conv565(dst, src, srcBytes);
}

__attribute__((noinline))
uint32_t gfx_convertRows_flash(uint8_t *dst, uint16_t row, uint16_t rows)
{
    const uint8_t *src = gfx_fb + (uint32_t)row * GFX_FB_STRIDE;
    uint32_t n = (uint32_t)rows * GFX_FB_STRIDE;
    if (s_mode == GFX_12BPP) return conv444(dst, src, n);
    if (s_mode == GFX_18BPP) return conv666(dst, src, n);
    return conv565(dst, src, n);
}

/* ================================================================== */
/* DMA transmit                                                        */
/* ================================================================== */

/* Kick a transfer. `halfword` selects 16-bit SPI frames, which halves
 * the number of DMA bus cycles for 16 bpp data. */
static inline void dmaStart(const void *src, uint32_t bytes, bool halfword,
                            bool memInc, bool irq)
{
    SPI1->CTLR2 &= ~(1u << 1);                 /* TXDMAEN off while we poke */
    DMA1_Channel3->CFGR = 0;
    DMA1->INTFCR = DMA_CH3_ALL;

    if (halfword) { spiSet16(); }
    else          { spiSet8();  }

    DMA1_Channel3->MADDR = (uint32_t)src;
    DMA1_Channel3->CNTR  = halfword ? (bytes >> 1) : bytes;

    uint32_t cfg = DMA_DIR_M2P | DMA_PL_VHIGH;
    if (memInc)   cfg |= DMA_MINC;
    if (halfword) cfg |= DMA_PSIZE_16 | DMA_MSIZE_16;
    if (irq)      cfg |= DMA_TCIE;
    DMA1_Channel3->CFGR = cfg;

    SPI1->CTLR2 |= (1u << 1);                  /* TXDMAEN */
    DMA1_Channel3->CFGR |= DMA_EN;
}

static inline void dmaStopAndDrain(void) {
    while (!(DMA1->INTFR & DMA_CH3_TCIF)) { }
    DMA1_Channel3->CFGR &= ~DMA_EN;
    DMA1->INTFCR = DMA_CH3_ALL;
    SPI1->CTLR2 &= ~(1u << 1);
    spiWait();
}

void gfx_directBlit(const void *data, uint32_t bytes, bool halfword) {
    dmaStart(data, bytes, halfword, true, false);
    dmaStopAndDrain();
}

void gfx_blockingWrite(const uint8_t *data, uint32_t bytes) {
    spiSet8();
    while (bytes--) {
        while (!(SPI1->STATR & SPI_TXE)) { }
        SPI1->DATAR = *data++;
    }
    spiWait();
}

/* ================================================================== */
/* Window addressing                                                   */
/* ================================================================== */

void gfx_setWindow(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    uint16_t x0 = x + s_colStart, x1 = x0 + w - 1;
    uint16_t y0 = y + s_rowStart, y1 = y0 + h - 1;

    gfx_cmd(ST_CASET);
    gfx_data8(x0 >> 8); gfx_data8(x0 & 0xFF);
    gfx_data8(x1 >> 8); gfx_data8(x1 & 0xFF);

    gfx_cmd(ST_RASET);
    gfx_data8(y0 >> 8); gfx_data8(y0 & 0xFF);
    gfx_data8(y1 >> 8); gfx_data8(y1 & 0xFF);

    gfx_cmd(ST_RAMWR);
    spiWait();
}

void gfx_directFillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565) {
    /* The whole point of this path: MINC off, so DMA re-reads the same
     * halfword for every pixel. No buffer, no CPU, full wire speed. */
    static volatile uint16_t colour;
    colour = rgb565;
    gfx_wait();
    CS_LOW();
    gfx_setWindow(x, y, w, h);
    dmaStart((const void *)&colour, (uint32_t)w * h * 2u, true, false, false);
    dmaStopAndDrain();
    CS_HIGH();
}

/* ================================================================== */
/* Frame presentation                                                  */
/* ================================================================== */
/*
 * One engine handles both the full frame and a sub-rectangle, because a
 * full frame IS the rectangle (0,0,128,128). Partial updates are the
 * single biggest lever left after DMA: the wire is the bottleneck, so
 * pushing a quarter of the screen takes a quarter of the time. A game
 * with a static background and a few moving sprites can run 4-8x the
 * full-frame rate by declaring what actually changed.
 *
 * Constraint: the source is 4 bpp, so a partial row has to start and end
 * on a byte. x and w are rounded outward to a multiple of 2 (16 bpp) or
 * 8 (12 bpp, where the converter works in groups of 8 pixels). Rounding
 * out is always safe - you send slightly more than you had to.
 */
struct FlushJob {
    uint16_t x0;        /* left edge, already aligned            */
    uint16_t wpx;       /* width in pixels, already aligned      */
    uint16_t rowsPer;   /* rows per DMA chunk                    */
    uint16_t row;       /* next row to convert                   */
    uint16_t endRow;    /* one past the last row                 */
    uint32_t chunkLen;  /* bytes a full chunk produces           */
};
static volatile FlushJob s_job;

/* Convert `rows` rows of the job's column span into dst. */
static RAMFUNC uint32_t convertJobRows(uint8_t *dst, uint16_t row, uint16_t rows)
{
    uint16_t x0  = s_job.x0;
    uint16_t wpx = s_job.wpx;
    uint32_t srcBytesPerRow = wpx >> 1;

    if (x0 == 0 && wpx == GFX_W)
        return gfx_convertRows_ram(dst, row, rows);   /* contiguous fast path */

    const uint8_t *src = gfx_fb + (uint32_t)row * GFX_FB_STRIDE + (x0 >> 1);
    uint8_t *d = dst;
    for (uint16_t r = 0; r < rows; r++) {
        d += gfx_convertSpan_ram(d, src, srcBytesPerRow);
        src += GFX_FB_STRIDE;
    }
    return (uint32_t)(d - dst);
}

static uint32_t setupJob(int x, int y, int w, int h)
{
    /* Wait FIRST. An async flush still in flight has the DMA ISR reading
     * s_job every chunk; overwriting it underneath would corrupt the
     * transfer in progress. */
    gfx_wait();

    /* Clip, then align outward. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0) return 0;

    /* 12 bpp packs 8 px into 12 bytes, 18 bpp packs 4 px into 12 bytes,
     * and 16 bpp only needs whole source bytes. */
    int align = (s_mode == GFX_12BPP) ? 8 : (s_mode == GFX_18BPP ? 4 : 2);
    int x1 = x + w;
    x  = x & ~(align - 1);
    x1 = (x1 + align - 1) & ~(align - 1);
    if (x1 > GFX_W) x1 = GFX_W;
    w = x1 - x;

    uint32_t bytesPerRow = (s_mode == GFX_12BPP) ? (uint32_t)w * 3u / 2u
                         : (s_mode == GFX_18BPP) ? (uint32_t)w * 3u
                                                 : (uint32_t)w * 2u;
    uint16_t rowsPer = (uint16_t)(GFX_CHUNK_BYTES / bytesPerRow);
    if (rowsPer == 0) rowsPer = 1;
    if (rowsPer > h)  rowsPer = (uint16_t)h;

    s_job.x0       = (uint16_t)x;
    s_job.wpx      = (uint16_t)w;
    s_job.rowsPer  = rowsPer;
    s_job.row      = (uint16_t)y;
    s_job.endRow   = (uint16_t)(y + h);
    s_job.chunkLen = bytesPerRow * rowsPer;

    CS_LOW();
    gfx_setWindow((uint8_t)x, (uint8_t)y, (uint8_t)w, (uint8_t)h);
    return 1;
}

/* How many rows the next chunk should carry (the last one may be short). */
static inline uint16_t rowsThisChunk(void) {
    uint16_t left = s_job.endRow - s_job.row;
    return left < s_job.rowsPer ? left : s_job.rowsPer;
}

void gfx_flushRect(int x, int y, int w, int h)
{
    if (!setupJob(x, y, w, h)) return;

    uint8_t cur = 0;
    uint16_t r = rowsThisChunk();
    uint32_t n = convertJobRows(s_chunk[cur], s_job.row, r);
    s_job.row += r;

    for (;;) {
        dmaStart(s_chunk[cur], n, s_mode == GFX_16BPP, true, false);
        if (s_job.row < s_job.endRow) {
            /* Convert the next chunk while this one is on the wire.
             * Conversion is ~10x faster than the transfer, so the SPI
             * bus never goes idle and the frame costs exactly its bytes. */
            uint8_t nxt = cur ^ 1;
            uint16_t r2 = rowsThisChunk();
            uint32_t n2 = convertJobRows(s_chunk[nxt], s_job.row, r2);
            s_job.row += r2;
            dmaStopAndDrain();
            cur = nxt; n = n2;
        } else {
            dmaStopAndDrain();
            break;
        }
    }
    CS_HIGH();
}

void gfx_flushRectAsync(int x, int y, int w, int h)
{
    if (!setupJob(x, y, w, h)) return;

    /* Both buffers are converted before the DMA is armed. Publishing
     * s_ready after starting the transfer would leave a window where the
     * completion ISR fires first and the frame stalls after one chunk. */
    uint16_t r = rowsThisChunk();
    uint32_t n = convertJobRows(s_chunk[0], s_job.row, r);
    s_job.row += r;

    if (s_job.row < s_job.endRow) {
        uint16_t r2 = rowsThisChunk();
        s_readyLen = convertJobRows(s_chunk[1], s_job.row, r2);
        s_job.row += r2;
        s_ready = 1;
    } else {
        s_ready = -1;
    }

    s_sending = 0;
    dmaStart(s_chunk[0], n, s_mode == GFX_16BPP, true, true);
}

/* ------------------------------------------------------------------ */
/* Direct streaming                                                    */
/* ------------------------------------------------------------------ */
/*
 * Same ping-pong as gfx_flushRect, with the framebuffer conversion
 * swapped for a caller-supplied generator. The generator fills the
 * chunk that is NOT currently being transmitted, so as long as it
 * stays inside 32 cycles/pixel (16 bpp) or 48 (18 bpp) the SPI bus
 * never goes idle and a full-colour procedural frame costs exactly its
 * bytes on the wire.
 */
void gfx_stream(gfx_streamFn fn, void *user)
{
    /* 12 bpp straddles byte boundaries between pixels, which is useless
     * to a per-pixel generator. Move to 16 bpp rather than hand the
     * callback a packing it cannot write. */
    if (s_mode == GFX_12BPP) gfx_setColorMode(GFX_16BPP);

    const uint32_t bpp         = (s_mode == GFX_18BPP) ? 3u : 2u;
    const uint32_t bytesPerRow = GFX_W * bpp;
    uint16_t rowsPer = (uint16_t)(GFX_CHUNK_BYTES / bytesPerRow);
    if (rowsPer == 0) rowsPer = 1;

    gfx_wait();
    CS_LOW();
    gfx_setWindow(0, 0, GFX_W, GFX_H);

    uint8_t  cur = 0;
    uint16_t y   = 0;
    uint16_t r   = (uint16_t)((GFX_H - y) < rowsPer ? (GFX_H - y) : rowsPer);
    fn(s_chunk[cur], y, r, user);
    y = (uint16_t)(y + r);
    uint32_t n = r * bytesPerRow;

    for (;;) {
        dmaStart(s_chunk[cur], n, s_mode == GFX_16BPP, true, false);
        if (y < GFX_H) {
            uint8_t  nxt = cur ^ 1;
            uint16_t r2  = (uint16_t)((GFX_H - y) < rowsPer ? (GFX_H - y) : rowsPer);
            fn(s_chunk[nxt], y, r2, user);      /* runs during the DMA */
            y = (uint16_t)(y + r2);
            dmaStopAndDrain();
            cur = nxt;
            n = r2 * bytesPerRow;
        } else {
            dmaStopAndDrain();
            break;
        }
    }
    CS_HIGH();
}

void gfx_flush(void)      { gfx_flushRect(0, 0, GFX_W, GFX_H); }
void gfx_flushAsync(void) { gfx_flushRectAsync(0, 0, GFX_W, GFX_H); }

bool gfx_busy(void) { return s_sending >= 0; }
void gfx_wait(void) { while (s_sending >= 0) { } }

extern "C" void DMA1_Channel3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
extern "C" void DMA1_Channel3_IRQHandler(void)
{
    DMA1_Channel3->CFGR &= ~DMA_EN;
    DMA1->INTFCR = DMA_CH3_ALL;

    int8_t ready = s_ready;
    if (ready >= 0) {
        /* Arm the already-converted buffer FIRST so the wire never idles,
         * then refill the buffer that just drained. */
        int8_t justDone = s_sending;
        uint32_t n = s_readyLen;
        s_sending = ready;
        s_ready   = -1;
        dmaStart(s_chunk[ready], n, s_mode == GFX_16BPP, true, true);

        if (s_job.row < s_job.endRow) {
            uint16_t r = rowsThisChunk();
            s_readyLen = convertJobRows(s_chunk[justDone], s_job.row, r);
            s_job.row += r;
            s_ready = justDone;
        }
    } else {
        /* Last chunk: DMA has handed over every byte, but the SPI shift
         * register still holds one frame. Do not drop CS before it lands. */
        SPI1->CTLR2 &= ~(1u << 1);
        spiWait();
        CS_HIGH();
        s_sending = -1;
    }
}
