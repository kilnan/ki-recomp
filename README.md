# ki-recomp

Exploratory static recompilation of **Killer Instinct** (Rare/Midway arcade,
1994; MIPS R4600 little-endian) to C++, with macOS Apple Silicon as the first
host and a portable runtime (C++20, CMake, no SDL yet).

Current state: a feasibility proof. Two real game functions are translated
automatically from MIPS to C++ and match MAME bit for bit (registers and all
of RAM) on 24 captured calls. There is no runnable game, and none is planned
for this phase.

- Findings, sources, open questions and next step: [docs/TECHNICAL_NOTE.md](docs/TECHNICAL_NOTE.md)
- Proof results per case: [poc/SUMMARY.md](poc/SUMMARY.md). The full report with the original
  instructions and the generated code is produced locally in `poc/out/REPORT.md` (ignored by Git).

## Layout

| path | what |
|---|---|
| `tools/kirecomp/kirecomp.py` | MIPS III (LE) -> C++ translator, decoder: rabbitizer |
| `runtime/` | guest runtime: CPU context, memory map, arithmetic/unaligned helpers |
| `tools/mame_capture.py` | captures CPU state and RAM around one call of a function in MAME |
| `tests/compare_case.cpp` | replays a capture through the recompiled code and diffs everything |
| `tests/mutation_check.py` | negative control: injects translation bugs, checks they are caught |
| `tests/runtime_selftest.cpp` | helper semantics on synthetic values (no game data) |
| `poc/cases.txt`, `poc/run_captures.sh` | the capture cases of the proof |
| `poc/make_report.py` | regenerates `poc/SUMMARY.md` and the local `poc/out/REPORT.md` |
| `tools/check_data.py` | verifies your ROM files against MAME's hashes |
| `tools/extract_game_code.sh` | unpacks the game code from the U98 boot EPROM (kipack) |
| `tools/mame_verify_extraction.py` | checks the unpacked code against DRAM in MAME |
| `data/`, `traces/`, `poc/out/`, `build/` | **ignored by Git**: game data, captures, game-derived reports and generated code stay local |

## Requirements (macOS arm64)

```sh
xcode-select --install                 # Apple clang (C++20)
brew install cmake ninja python mame rom-tools   # rom-tools provides chdman
python3 -m venv .venv && .venv/bin/pip install rabbitizer==1.16.2
```

`mips-linux-gnu-binutils` (Homebrew) is optional, for `objdump` listings.
Tested with Apple clang 21, CMake 4.4, MAME 0.289, rabbitizer 1.16.2.

## Game data (not included, never committed)

Put the MAME `kinst` set (BIOS v1.5d) in `data/roms/kinst/`:

| file | size | CRC32 | SHA1 | needed now |
|---|---|---|---|---|
| `ki-l15d.u98` | 512 KiB | 7b65ca3d | 607394d4ba1713f38c2cb5159303cace9cde991e | yes |
| `u10-l1` ... `u36-l1` (8 files) | 512 KiB each | see `tools/check_data.py` | | for MAME to start |
| `kinst.chd` | 131 MB disk | content SHA1 81d833236e994528d1482979261401b198d1ca53 | | no (see below) |

Hashes are from MAME `src/mame/rare/kinst.cpp` (`ROM_START(kinst)`). Without
`kinst.chd`, `tools/make_blank_chd.sh` creates an empty disk with the right
geometry: MAME boots it (checksum warning) far enough for this proof.

```sh
python3 tools/check_data.py            # verify the files you supplied
tools/extract_game_code.sh             # -> data/kipack-out/rom-{0,1,2}.bin
tools/make_blank_chd.sh                # only if you do not have kinst.chd
python3 tools/mame_verify_extraction.py   # optional: rom-0 == DRAM in MAME
```

## Build and run the proof

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build                     # recompiles 0x8802d560 and 0x8802d774
poc/run_captures.sh                     # MAME captures -> traces/poc/ (see note on timing)
cmake -S . -B build && ctest --test-dir build   # one test per capture + self-test
python3 tests/mutation_check.py build traces/poc
python3 poc/make_report.py              # -> poc/SUMMARY.md, poc/out/REPORT.md
```

`poc/run_captures.sh` takes about 15 s for the DRC cases. Cases that inject
inputs (`syn-*`) and the `*-interp` cross-checks use MAME's MIPS interpreter,
whose boot is much slower: 1-2 minutes per case, run in parallel. `ONLY=<regex>`
selects cases, and `JOBS` sets parallelism. The function list is the CMake cache
variable `KI_FUNCTIONS`.

Survey of the whole code image (translation coverage, no validation):

```sh
.venv/bin/python tools/kirecomp/kirecomp.py --image data/kipack-out/rom-0.bin \
    --vram 88000000 --discover --out build/survey/all.cpp
```

## License and game data

The code in this repository is released under the [MIT License](LICENSE).

It covers only the code written for this project. Killer Instinct and every
file derived from it belong to their rights holders and are neither included
nor licensed here. That covers ROMs, the disk image, extracted images, MAME
captures, the generated C++ (`build/gen/`) and the full report (`poc/out/`).
You need your own copy of the game.

Third-party projects:

- **MAME** (BSD-3-Clause for the files consulted) is used as documentation and
  as an external reference emulator.
- **N64Recomp** (MIT) is used as documentation.
- **rabbitizer** (MIT) is installed with pip.
- **ki-rom** (AGPL-3.0-only): its kipack tool is built and run as a separate
  program by `tools/extract_game_code.sh`.

No source code from these projects is included. Any code reused from them in
the future must keep its original copyright notice. Code from ki-rom cannot be
included without changing this project's license.
