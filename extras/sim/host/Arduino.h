/*
 * chsim - just enough of the Arduino API for a CHGame sketch to run on a
 * PC. Time is virtual: micros() and millis() advance only when the
 * simulator says so (a flush, a delay, a pass of loop()), so a run is
 * exactly reproducible. Serial is the process's stdout.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define PROGMEM
#define F(s) (s)
#define pgm_read_byte(p)  (*(const uint8_t *)(p))
#define pgm_read_word(p)  (*(const uint16_t *)(p))
#define pgm_read_dword(p) (*(const uint32_t *)(p))
#define pgm_read_ptr(p)   (*(void *const *)(p))

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define F_CPU 48000000u

/* The CHGame pins a sketch is likely to name (variant_CHGame.h). The
 * numbers only have to be distinct; buttons read LOW while held. */
#define PIN_BTN_UP      101
#define PIN_BTN_DOWN    102
#define PIN_BTN_LEFT    103
#define PIN_BTN_RIGHT   104
#define PIN_BTN_A       105
#define PIN_BTN_B       106
#define PIN_BTN_SELECT  107
#define PIN_BTN_START   108
#define PIN_LED         109
#define PIN_BUZZER      110
#define LED_BUILTIN     PIN_LED

typedef bool    boolean;
typedef uint8_t byte;

uint32_t micros();
uint32_t millis();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void pinMode(uint32_t pin, uint32_t mode);
void digitalWrite(uint32_t pin, uint32_t v);
int  digitalRead(uint32_t pin);
long random(long howbig);
long random(long lo, long hi);
void randomSeed(unsigned long s);
static inline long map(long x, long a, long b, long c, long d) { return (x - a) * (d - c) / (b - a) + c; }

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
template <class T> static inline T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

class SimSerial {
public:
    void begin(unsigned long = 0) {}
    bool waitForPC(uint32_t = 0) { return true; }
    explicit operator bool() const { return true; }
    int  available();
    int  read();
    void flush() { fflush(stdout); }
    size_t write(uint8_t c);
    size_t write(const uint8_t *p, size_t n);

    size_t print(const char *s);
    size_t print(char c)             { return write((uint8_t)c); }
    size_t print(int v)              { return printf("%d", v); }
    size_t print(unsigned v)         { return printf("%u", v); }
    size_t print(long v)             { return printf("%ld", v); }
    size_t print(unsigned long v)    { return printf("%lu", v); }
    size_t print(double v, int d = 2){ return printf("%.*f", d, v); }
    template <class T> size_t println(T v) { size_t n = print(v); return n + print("\r\n"); }
    size_t println()                 { return print("\r\n"); }
    int printf(const char *fmt, ...);
};
extern SimSerial Serial;
