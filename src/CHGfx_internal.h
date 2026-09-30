/*
 * CHGfx_internal.h - shared between the library's own .cpp files. Not
 * part of the public API; sketches should include CHGfx.h only.
 */
#pragma once
#include "CHGfx.h"

/* Put a hot loop in SRAM. Flash on this part runs at 3 wait states at
 * 48 MHz (RM 20.3.1); SRAM runs at zero.
 *
 * The section name looks odd on purpose. The core's link script puts
 * *(.gnu.linkonce.r.*) first in .data, which has a RAM VMA and a flash
 * LMA, so startup copies it for us - and FIRST matters: .sdata and .sbss
 * come after, inside the 4 KB window the global pointer reaches, where a
 * small variable costs one instruction to load instead of two. Code
 * placed after them (1.2 used .srodata.*, which lands there) pushes the
 * sketch's variables out of that window, and each access to one of them
 * grows: most of the 2.3 KB CHChess's first 1.3 build grew by was that.
 * A ".data.*" name would sit in the right place too but
 * makes the assembler warn about section attributes. The linkonce part is
 * harmless: it only merges sections of the SAME name, and every function
 * here has its own.
 *
 * Every function gets a section of its OWN for a second reason: with one
 * shared name the linker sees a single input section and --gc-sections
 * can only keep or drop all of them together, so a sketch that never
 * blits still paid ~500 bytes of SRAM for gfx_blit. */
#ifdef CHSIM
#define GFX_RAMFUNC(name)
#else
#define GFX_RAMFUNC(name) __attribute__((section(".gnu.linkonce.r.chgfx." #name), noinline))
#endif

/* A helper that must be inlined into its SRAM caller. Plain `inline` is
 * only a hint: at -Os (the board default) GCC kept conv565 & co. as
 * out-of-line functions in .text, so the "SRAM" flush loop spent its time
 * calling back into flash. */
#define GFX_INLINE inline __attribute__((always_inline))

/* Clip rectangle, half-open: [x0, x1) x [y0, y1), always inside the
 * screen. Eight bytes, so -msmall-data-limit=8 puts it in .sdata and every
 * access is one gp-relative load. */
struct GfxClip { int16_t x0, y0, x1, y1; };
extern GfxClip gfx__clip;

static GFX_INLINE uint8_t *gfx__row(int y) { return gfx_fb + y * GFX_FB_STRIDE; }

/* One pixel into a row, no clipping. */
static GFX_INLINE void gfx__plot(uint8_t *row, int x, uint8_t c) {
    uint8_t *p = row + (x >> 1);
    if (x & 1) *p = (uint8_t)((*p & 0x0F) | (c << 4));
    else       *p = (uint8_t)((*p & 0xF0) | c);
}

/* Pixels [x0, x1) of one row in colour c, no clipping: ragged nibble
 * ends, word stores in the middle. c must already be 0..15. One copy, in
 * SRAM, shared by every fill in the library - inlining it into each
 * caller cost ~200 bytes of SRAM apiece. */
void gfx__span(uint8_t *row, int x0, int x1, uint8_t c);

/* The same for a short run (a sprite's, at most 16 px): no word path,
 * small enough to inline into a hot loop. */
static GFX_INLINE void gfx__run(uint8_t *row, int x0, int x1, uint8_t c) {
    int w = x1 - x0;
    if (w <= 0) return;
    uint8_t *p = row + (x0 >> 1);
    if (x0 & 1) { *p = (uint8_t)((*p & 0x0F) | (c << 4)); p++; w--; }
    uint8_t pair = (uint8_t)(c | (c << 4));
    while (w >= 2) { *p++ = pair; w -= 2; }
    if (w) *p = (uint8_t)((*p & 0xF0) | c);
}

/* Palette handoff between CHGfx_palette.cpp (portable) and whatever
 * presents frames (CHGfx.cpp on the board, the simulator on a PC).
 * Writes the 16 colours as they should reach the panel - fade applied -
 * and returns whether anything changed since the last call. */
bool gfx__paletteTake(uint16_t out[16]);
void gfx__paletteTouch(void);          /* force the next take to report a change */

/* Quarter-wave sine table lookup, angle in 1/256 turn, Q14 result. */
int gfx__sin14(uint8_t angle);

/* The built-in 5x7 font's five column bytes for ch ('?' if outside
 * 32..126). One copy of the table, in CHGfx_draw.cpp. */
const uint8_t *gfx__builtinGlyph(char ch);
