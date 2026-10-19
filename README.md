# ki-recomp

Exploratory static recompilation of **Killer Instinct** (Rare/Midway arcade,
1994; MIPS R4600 little-endian) to C++, with macOS Apple Silicon as the first
host and a portable runtime (C++20, CMake; SDL2 only for the windowed player).

Current state: the whole game is recompiled and plays natively, with sound, in
a window or headless. It starts from the player's MAME set like the arcade
board: the boot ROM is recompiled too and unpacks the game. Attract mode, coin,
character select, fights, game over and high scores all work. The DCS sound
board (an ADSP-2105 DSP) is emulated; its output matches MAME sample for sample. Real game code is translated automatically
from MIPS to C++. The validated code is six functions (FPU code, a `switch` with
a jump table, integer code) and the whole call subtree of a drawing routine,
which writes to the framebuffer. All of it matches MAME bit for bit (CPU and FPU
registers and all of RAM) on 52 captured calls, 35 of them made by the game
itself. The translator handles 562 of the 567 functions
reached by `jal`.

## Play a release build (macOS, Apple Silicon)

1. Download `KIRecompiled-macOS-arm64.zip` from the
   [releases](https://github.com/kilnan/ki-recomp/releases) and unzip it.
2. On first launch macOS blocks the unsigned app: open **System Settings →
   Privacy & Security → Open Anyway**, or run
   `xattr -dr com.apple.quarantine /path/to/KIRecompiled.app`.
3. When asked, drag onto the window the folder with your MAME `kinst` set:
   `kinst.zip` (BIOS v1.5d) and `kinst.chd`. The app checks every file
   against MAME's SHA1 and remembers the folder.

No game data is included: the app contains the recompiled program code and
starts it from your ROM like the arcade board, from the reset vector. Settings
and high scores the game writes to its disk are kept in
`~/Library/Application Support/kilnan/KIRecompiled/`. Keyboard and gamepad
controls are listed under [Play it](#play-it-sdl2-window).

Unofficial fan project, not affiliated with or endorsed by Rare, Midway,
Nintendo or Microsoft.

## Development

- Findings, sources, open questions and next step: [docs/TECHNICAL_NOTE.md](docs/TECHNICAL_NOTE.md)
- Proof results per case: [poc/SUMMARY.md](poc/SUMMARY.md). The full report with the original
  instructions and the generated code is produced locally in `poc/out/REPORT.md` (ignored by Git).

## Layout

| path | what |
|---|---|
| `tools/kirecomp/kirecomp.py` | MIPS III (LE) -> C++ translator, decoder: rabbitizer |
| `runtime/` | guest runtime: CPU context, memory map, arithmetic/unaligned helpers; board model (`kinst_board`), DCS sound board (`dcs`) |
| `third_party/adsp2100/` | ADSP-2105 DSP interpreter derived from MAME (BSD-3-Clause) |
| `src/ki_run.cpp`, `src/ki_play.cpp` | headless runner and SDL2 player of the whole recompiled program |
| `tests/dcs_replay.cpp` | DCS model against a MAME capture of sound commands and audio |
| `tools/mame_capture.py` | captures CPU state and RAM around one call of a function in MAME |
| `tests/compare_case.cpp` | replays a capture through the recompiled code and diffs everything |
| `tests/mutation_check.py` | negative control: injects translation bugs, checks they are caught |
| `tests/runtime_selftest.cpp` | helper semantics on synthetic values (no game data) |
| `poc/cases.txt`, `poc/run_captures.sh` | the capture cases of the proof |
| `poc/make_report.py` | regenerates `poc/SUMMARY.md` and the local `poc/out/REPORT.md` |
| `tools/check_data.py` | verifies your ROM files against MAME's hashes |
| `tools/extract_game_code.sh` | unpacks the game code from the U98 boot EPROM (kipack) |
| `tools/mame_verify_extraction.py` | checks the unpacked code against DRAM in MAME |
| `tools/mame_code_coverage.py` | which guest code MAME executes (DRC block log), optionally while playing |
| `tools/mame_inputs.py` | scripted inputs (coin, start, random buttons), timed on emulated frames |
| `tools/mame_run.py` | runs MAME with a wall-clock timeout; never leaves orphaned emulators |
| `data/`, `traces/`, `poc/out/`, `build/` | **ignored by Git**: game data, captures, game-derived reports and generated code stay local |

## Requirements

### macOS (arm64)

```sh
xcode-select --install                 # Apple clang (C++20)
brew install cmake ninja python mame rom-tools   # rom-tools provides chdman
python3 -m venv .venv && .venv/bin/pip install rabbitizer==1.16.2
```

`mips-linux-gnu-binutils` (Homebrew) is optional, for `objdump` listings.
Tested with Apple clang 21, CMake 4.4, MAME 0.289, rabbitizer 1.16.2.

### Linux

Debian/Ubuntu: `apt install g++ cmake ninja-build python3-venv libsdl2-dev`, the
same venv, and configure with `-DKI_PYTHON=.venv/bin/python`. Tested on Debian
12 arm64 (GCC 12, CMake 3.25) in Docker, image definition in
`tools/docker/Dockerfile.linux`:

```sh
docker build -t ki-linux -f tools/docker/Dockerfile.linux tools/docker
docker run --rm -v "$PWD":/src:ro ki-linux sh -c \
  'cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKI_PYTHON=/venv/bin/python && cmake --build /build && cd /build && ctest'
```

The Linux build gives the same audio and framebuffers as the macOS one, bit
for bit (1200 frames of attract mode compared).

## Game data (not included, never committed)

Put the MAME `kinst` set (BIOS v1.5d) in `data/roms/kinst/`:

| file | size | CRC32 | SHA1 | needed now |
|---|---|---|---|---|
| `ki-l15d.u98` | 512 KiB | 7b65ca3d | 607394d4ba1713f38c2cb5159303cace9cde991e | yes |
| `u10-l1` ... `u36-l1` (8 files) | 512 KiB each | see `tools/check_data.py` | | for sound (and for MAME to start) |
| `kinst.chd` | 131 MB disk | content SHA1 81d833236e994528d1482979261401b198d1ca53 | | for the gameplay cases |

Hashes are from MAME `src/mame/rare/kinst.cpp` (`ROM_START(kinst)`). Verify the
CHD with `chdman info -i kinst.chd`. `tools/make_blank_chd.sh` creates an empty
disk with the right geometry. MAME boots it, with a checksum warning, far enough
for the boot-time cases, which are marked `--blank-disk` in `poc/cases.txt`
and always use it.

```sh
python3 tools/check_data.py            # verify the files you supplied
tools/extract_game_code.sh             # -> data/kipack-out/rom-{0,1,2}.bin
tools/make_blank_chd.sh                # needed by the --blank-disk cases
python3 tools/mame_verify_extraction.py   # optional: rom-0 == DRAM in MAME
```

## Build and run the proof

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build                     # recompiles the functions in KI_FUNCTIONS
poc/run_captures.sh                     # MAME captures -> traces/poc/ (see note on timing)
cmake -S . -B build && ctest --test-dir build   # one test per capture + self-test
python3 tests/mutation_check.py build traces/poc
python3 poc/make_report.py              # -> poc/SUMMARY.md, poc/out/REPORT.md
```

Capture times:
- **Boot-time DRC cases:** seconds each.
- **Gameplay cases** (`--play`, real disk): 1-5 minutes each.
- **Interpreter cases:** cases that set registers (`syn-*`, except `syn-fpu-*`, which
  inject memory only) and the `*-interp` cross-checks use MAME's MIPS interpreter,
  whose boot is much slower.

The whole set takes roughly 30-40 minutes. `ONLY=<regex>` selects cases, `JOBS`
sets parallelism (default 3; each MAME uses a full core) and `OUT_DIR` writes
elsewhere, for example to check that captures reproduce. The function list is
the CMake cache variable `KI_FUNCTIONS`.

Code reached by MAME during scripted play (needs the real `kinst.chd`):

```sh
.venv/bin/python tools/mame_code_coverage.py --out traces/coverage --seconds 180
```

Survey of the whole code image (translation coverage, no validation):

```sh
.venv/bin/python tools/kirecomp/kirecomp.py --image data/kipack-out/rom-0.bin \
    --vram 88000000 --discover --out build/survey/all.cpp
```

## Run the whole program (headless)

`cmake --build build` also builds `ki_run`, the whole game recompiled (every
entry point the translator finds) on a model of the board. It covers I/O, the
IDE drive on a raw disk image, VBLANK, the COP0 timer and interrupts. It starts
from the state the original boot ROM leaves when it enters the game, captured
once in MAME:

```sh
.venv/bin/python tools/mame_capture.py --func 88000000 --seconds 30 capture --hit 1 \
    --entry-only --out traces/bootstate
chdman extracthd -i data/roms/kinst/kinst.chd -o data/kinst.img
build/ki_run --boot traces/bootstate --disk data/kinst.img --rom data/roms/kinst/ki-l15d.u98 \
    --frames 600 --dump-every 60 --out traces/run     # framebuffers as PPM
```

`--boot rom` instead starts at the reset vector: the recompiled boot ROM
unpacks the game itself (no snapshot needed). Its state at the game's entry
equals the MAME snapshot byte for byte. `KI_TRACE_IDE=1` logs disk commands,
`KI_TRACE_VRAM=1` framebuffer flips.

### Play it (SDL2 window)

```sh
build/ki_play --data data/roms/kinst     # folder with kinst.zip or its files, and kinst.chd
```

This is the release mode: start at the reset vector from the MAME set, with
sound. Without `--data`, the last folder is used, or the window asks for one
to be dropped onto it. For development, the old form still works:
`ki_play --boot traces/bootstate|rom --disk data/kinst.img --rom data/roms/kinst/ki-l15d.u98
[--sound-roms DIR] [--mute]`. `ki_run --sound-roms DIR --wav-out FILE` records the
audio of a headless run.

`tools/package_macos.sh VERSION` builds the portable app bundle in `dist/`
(SDL3 and sdl2-compat built from source for macOS 11, bundled; ad-hoc signed)
and checks that it loads only its own libraries.

**Player 1**

| key | action |
|---|---|
| arrows | move |
| A / S / D | high attack: quick, medium, fierce |
| Z / X / C | low attack: quick, medium, fierce |
| 1 | start |
| 5 | coin |

**Player 2**

| key | action |
|---|---|
| I / J / K / L | move |
| Q / W / E | high attacks |
| T / Y / U | low attacks |
| 2 | start |
| 6 | coin |

**Gamepads** (Xbox layout; first pad = player 1, second = player 2)

| button | action |
|---|---|
| d-pad / left stick | move |
| X / Y / RB | high attack: quick, medium, fierce |
| A / B / RT | low attack: quick, medium, fierce |
| Start | start |
| Back | coin |

**Other keys**

| key | action |
|---|---|
| F5 | save the state (taken at the next frame) |
| F9 | load the saved state |
| F2 | test switch |
| Tab | unthrottled (no sound while on) |
| Esc | quit |

### Debugging against MAME
- `build/ki_run --inputs FILE` replays a scripted session; generate one with
  `tools/make_input_script.py`.
- `--call-log-from N` logs function entries.
- `--dump-at ADDR[:N]` saves the state at the N-th entry of a function, in the
  snapshot format.
- `tools/mame_snapshot.py` takes the same kind of snapshot in MAME, mid-game.
  `ki_run --boot` starts from either kind of snapshot.
- With `-DKI_BUILD_TRACE=ON`, `ki_run_trace --trace-out` writes one PC per
  executed instruction, to diff against a MAME `trace` log.
- `tools/mame_dcs_capture.py --seconds 150 --out traces/dcs_play` records the
  sound commands of a MAME session and its audio at 31250 Hz;
  `build/dcs_replay traces/dcs_play data/roms/kinst` replays the commands on the
  DCS model and compares the output with MAME's, sample by sample.

## License and game data

The code in this repository is released under the [MIT License](LICENSE).

It covers only the code written for this project. Killer Instinct and every
file derived from it belong to their rights holders and are neither included
nor licensed here. That covers ROMs, the disk image, extracted images, MAME
captures, the generated C++ (`build/gen/`) and the full report (`poc/out/`).
You need your own copy of the game. Release builds contain the compiled
recompiled code, as other static recompilation projects do, but no game data.

Third-party projects:

- **MAME** (BSD-3-Clause for the files consulted) is used as documentation and
  as an external reference emulator. The ADSP-2105 core in
  [`third_party/adsp2100/`](third_party/adsp2100) is derived from MAME's ADSP-21xx
  core and keeps its BSD-3-Clause license and copyright notice (Aaron Giles);
  see the README there. The DCS board model (`runtime/src/dcs.cpp`) is written
  for this project, following MAME's `dcs.cpp` as its specification.
- **N64Recomp** (MIT) is used as documentation.
- **rabbitizer** (MIT) is installed with pip.
- **ki-rom** (AGPL-3.0-only): its kipack tool is built and run as a separate
  program by `tools/extract_game_code.sh` at build time. Release builds do not
  use it: the recompiled boot ROM unpacks the game.
- **libchdr** (BSD-3-Clause) and the **miniz** it bundles (MIT) are fetched by
  CMake at a fixed revision to read `kinst.chd` and `kinst.zip`.
- **SDL3** and **sdl2-compat** (zlib) are bundled with the macOS app.

Apart from `third_party/adsp2100/`, no source code from these projects is
included. Code reused from them must keep its original copyright notice. Code
from ki-rom cannot be included without changing this project's license.
