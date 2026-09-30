/* chsim internals shared by the host files. */
#pragma once
#include <stdint.h>

uint32_t sim_now();                  /* virtual microseconds                    */
void     sim_advance(uint32_t us);   /* move virtual time on (captures rows)    */
void     sim_sync();                 /* charge the host time since the last sync,
                                        scaled to the device (--cost only)      */
void     sim_bug(const char *fmt, ...);
bool     sim_buttonHeld(uint32_t pin);

/* The panel: what the ST7735 is showing, RGB888. */
extern uint32_t sim_panel[];
void sim_framePresented();           /* a flush has fully landed on the panel   */
void sim_flushProgress(uint32_t now);   /* chsim_gfx.cpp: capture rows converted by now */
