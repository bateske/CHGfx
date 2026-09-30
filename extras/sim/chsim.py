"""chsim - run a CHGfx sketch on a PC.

    python chsim.py build <sketch dir> [-D NAME=VAL ...] [-o sim.exe]
    python chsim.py run   <sketch dir> [options]      build, run, save frames
    python chsim.py test                              the library's own tests

`run` options:
    --frames N       stop after N presented frames (default 300)
    --png DIR        save frames as PNGs (with --every/--start to choose)
    --gif FILE       save frames as an animated GIF
    --every K        keep every K-th frame (default 1)
    --start S        from frame S (default 0)
    --scale N        enlarge saved images N times (default 3)
    --input SPEC     buttons, "F:BTN+BTN,F:" - from presented frame F on,
                     hold these (A B UP DOWN LEFT RIGHT START SELECT)
    --cost           charge the sketch's own CPU time, scaled to the device
    -D NAME=VAL      extra defines

The sketch is compiled with the library's portable sources (everything in
src/ except CHGfx.cpp, which drives the SPI and DMA) and the host files in
host/, which replace it with a simulated panel. The drawing code that runs
is the code that runs on the board.

Exit status 3 means the simulator caught a bug (drawing into a frame that
was still being sent, the chunk scratch used during a flush); the BUG
lines on stderr say where.

Compiler: $CHSIM_CXX (e.g. "zig c++"), else zig on the PATH, else the
ziglang pip package (`pip install ziglang`), else clang++ or g++.
PNG/GIF output needs Pillow (`pip install pillow`).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIB = HERE.parent.parent            # the CHGfx library folder
SRC = LIB / "src"
W = H = 128
FRAME = 8 + W * H * 3


def find_cxx():
    env = os.environ.get("CHSIM_CXX")
    if env:
        return env.split()
    if shutil.which("zig"):
        return ["zig", "c++"]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        pass
    for c in ("clang++", "g++"):
        if shutil.which(c):
            return [c]
    raise SystemExit("chsim: no C++ compiler. Set CHSIM_CXX, put zig/clang++/g++ on the PATH, "
                     "or `pip install ziglang`.")


def sketch_source(sketch, bdir):
    """The sketch as one C++ file. arduino-cli's preprocessor adds the
    function prototypes the Arduino IDE would; without it, the .ino files
    are concatenated as they are."""
    name = sketch.name
    unit = bdir / "sketch_ino.cpp"
    if shutil.which("arduino-cli"):
        r = subprocess.run(["arduino-cli", "compile", "-b", os.environ.get("CHSIM_FQBN", "CHGame:ch32v:CHGame"),
                            "--preprocess", str(sketch)], capture_output=True, text=True)
        if r.returncode == 0 and "void setup" in r.stdout:
            unit.write_text(r.stdout, encoding="utf-8")
            return unit
    inos = sorted(sketch.glob("*.ino"))
    main_ino = sketch / f"{name}.ino"
    if main_ino in inos:
        inos.remove(main_ino)
        inos.insert(0, main_ino)
    with open(unit, "w", encoding="utf-8") as f:
        f.write("#include <Arduino.h>\n")
        for ino in inos:
            f.write(f'#line 1 "{ino.as_posix()}"\n')
            f.write(ino.read_text(encoding="utf-8"))
            f.write("\n")
    return unit


def compile_exe(sources, exe, defines=(), includes=()):
    cmd = find_cxx() + ["-std=gnu++17", "-O1", "-g0", "-w", "-DCHSIM", "-DCH32X035", "-DARDUINO=10800",
                        f"-I{HERE / 'host'}", f"-I{SRC}"]
    cmd += [f"-I{i}" for i in includes]
    cmd += [f"-D{d}" for d in defines]
    cmd += [str(s) for s in sources] + ["-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"chsim: build failed ({exe.name})")
    return exe


def library_sources():
    return sorted(p for p in SRC.glob("*.cpp") if p.name != "CHGfx.cpp")


def build(sketch, defines=(), out=None):
    sketch = Path(sketch).resolve()
    bdir = HERE / "build" / sketch.name
    bdir.mkdir(parents=True, exist_ok=True)
    srcs = [sketch_source(sketch, bdir)]
    srcs += sorted(p for p in (sketch / "src").rglob("*") if p.suffix in (".cpp", ".c")) if (sketch / "src").exists() else []
    srcs += library_sources()
    srcs += [HERE / "host" / "chsim_gfx.cpp", HERE / "host" / "chsim_main.cpp"]
    exe = Path(out) if out else bdir / ("sim.exe" if os.name == "nt" else "sim")
    return compile_exe(srcs, exe, defines, includes=[sketch])


def read_frames(path):
    data = Path(path).read_bytes()
    for off in range(0, len(data) - FRAME + 1, FRAME):
        n, ms = struct.unpack_from("<II", data, off)
        yield n, ms, data[off + 8: off + FRAME]


def to_image(rgb, scale):
    from PIL import Image
    im = Image.frombytes("RGB", (W, H), rgb)
    return im.resize((W * scale, H * scale), Image.NEAREST) if scale != 1 else im


def run(a):
    exe = build(a.sketch, a.D)
    frames = HERE / "build" / Path(a.sketch).resolve().name / "frames.bin"
    cmd = [str(exe), "--frames", str(a.frames), "--every", str(a.every), "--start", str(a.start)]
    if a.png or a.gif:
        cmd += ["--out", str(frames)]
    if a.input:
        cmd += ["--input", a.input]
    if a.cost:
        cmd.append("--cost")
    r = subprocess.run(cmd)
    if a.png or a.gif:
        imgs = [(n, ms, to_image(rgb, a.scale)) for n, ms, rgb in read_frames(frames)]
        if a.png:
            out = Path(a.png)
            out.mkdir(parents=True, exist_ok=True)
            for n, _, im in imgs:
                im.save(out / f"frame{n:05d}.png")
            print(f"chsim: {len(imgs)} PNGs in {out}", file=sys.stderr)
        if a.gif and imgs:
            dur = [max(20, (imgs[i + 1][1] - imgs[i][1])) for i in range(len(imgs) - 1)] + [100]
            imgs[0][2].save(a.gif, save_all=True, append_images=[im for _, _, im in imgs[1:]],
                            duration=dur, loop=0)
            print(f"chsim: {a.gif} ({len(imgs)} frames)", file=sys.stderr)
    return r.returncode


def test():
    tdir = HERE / "tests"
    bdir = HERE / "build" / "tests"
    bdir.mkdir(parents=True, exist_ok=True)
    exe = bdir / ("tests.exe" if os.name == "nt" else "tests")
    srcs = sorted(tdir.glob("*.cpp")) + library_sources() + [HERE / "host" / "chsim_gfx.cpp"]
    compile_exe(srcs, exe, includes=[tdir])
    return subprocess.run([str(exe)]).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("sketch")
    b.add_argument("-D", action="append", default=[])
    b.add_argument("-o", dest="out")
    r = sub.add_parser("run")
    r.add_argument("sketch")
    r.add_argument("-D", action="append", default=[])
    r.add_argument("--frames", type=int, default=300)
    r.add_argument("--every", type=int, default=1)
    r.add_argument("--start", type=int, default=0)
    r.add_argument("--scale", type=int, default=3)
    r.add_argument("--png")
    r.add_argument("--gif")
    r.add_argument("--input")
    r.add_argument("--cost", action="store_true")
    sub.add_parser("test")
    a = ap.parse_args()
    if a.cmd == "build":
        print(build(a.sketch, a.D, a.out))
    elif a.cmd == "run":
        sys.exit(run(a))
    else:
        sys.exit(test())


if __name__ == "__main__":
    main()
