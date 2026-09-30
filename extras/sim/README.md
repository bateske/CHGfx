# chsim — CHGfx on a PC

Runs a CHGame sketch on your computer: the library's real drawing code,
compiled for the PC, with a simulated panel where the SPI and DMA would
be. Screenshots and GIFs in seconds, no upload, no cable — and a couple
of bugs caught that are almost invisible on the glass.

```bash
python chsim.py run ../../examples/GameKit --gif kit.gif
python chsim.py run MyGame --png shots --every 10 --frames 600
python chsim.py run MyGame --input "30:A,34:,90:RIGHT+B,120:" --gif play.gif
python chsim.py test
```

## What is real and what is simulated

Everything in `src/` except `CHGfx.cpp` is compiled as is: the drawing
primitives, text, sprites, the palette and fade. `host/chsim_gfx.cpp`
replaces `CHGfx.cpp`, the transport:

* A flush takes the time it takes on the board (the throughput and setup
  cost measured by the `Benchmark` example, scaled by the SPI divider),
  and converts rows chunk by chunk the way the DMA interrupt does.
  `gfx_busy()`, `gfx_flushRow()` and `gfx_waitRow()` report the same
  progress the board would.
* Each row lands on the simulated panel at the moment it is converted,
  with whatever the framebuffer holds then — so a frame torn on the board
  is torn in the simulator too, and reported.
* The panel shows 12 bpp output as 12 bits: what you see is what the
  palette really produces.
* `gfx_stream()` works: direct-mode effects render in full colour.

`host/Arduino.h` is just enough Arduino: virtual `millis()`/`micros()`
that advance only with the simulation (a run is exactly reproducible),
`Serial` on stdout, `digitalRead()` on the CHGame's button pins
(`PIN_BTN_A` …) from the `--input` script.

## The bugs it catches

The simulator exits with status **3** and prints a `BUG` line for:

* **Drawing into a frame that is still being sent.** After
  `gfx_flushAsync()`, rows the flush has not converted yet belong to it.
  Draw into one and the panel shows half of each frame. The report names
  the row, and the `gfx_waitRow()` that would have made it safe.
* **Using `gfx_chunkScratch()` during a flush.** The scratch *is* the
  flush's chunk buffers.

Palette calls during a flush used to be the third; since CHGfx 1.3 they
are staged until the next flush, so they are safe.

## `run` options

| Option | |
|---|---|
| `--frames N` | Stop after N presented frames (default 300) |
| `--png DIR` | Save frames as PNGs |
| `--gif FILE` | Save frames as an animated GIF, timed as they were shown |
| `--every K`, `--start S` | Keep every K-th frame, from frame S |
| `--scale N` | Enlarge images N times (default 3) |
| `--input SPEC` | Buttons: `"F:BTN+BTN,F:"` holds those buttons from presented frame F on; an empty list releases. `A B UP DOWN LEFT RIGHT START SELECT` |
| `--cost` | Charge the sketch's own CPU time too, estimated for the board (below) |
| `-D NAME=VAL` | Extra defines |

`--cost` times the library's primitives on your PC at startup, compares
them with the board's benchmark figures, and scales the host time your
sketch spends between library calls by that ratio. It is an estimate —
good for "is this frame 5 ms or 25 ms", not for the last millisecond —
and it makes runs depend on your PC's timing, which is why it is off by
default. The summary line reports the estimated CPU per frame.

## Building

The sketch is compiled as the Arduino IDE would see it: if `arduino-cli`
is installed its preprocessor adds the function prototypes; otherwise the
`.ino` files are concatenated. `.cpp` files under the sketch's `src/` are
included.

Any C++17 compiler works. `chsim.py` uses `$CHSIM_CXX` if set (e.g.
`zig c++`), then `zig` on the PATH, then the `ziglang` pip package
(`pip install ziglang` — the easy route on Windows), then `clang++` or
`g++`. Images need Pillow (`pip install pillow`).

## `test`

`python chsim.py test` builds `tests/` against the library and runs
about 20,000 checks: every primitive against a pixel-at-a-time reference
on random arguments that run off every edge, the clip rectangle against
the rule that a clipped call must match an unclipped one inside the
rectangle and touch nothing outside it, the 3×5 font glyph by glyph, the
fade, and this simulator's own flush model. Run it after changing
anything in `src/`.
