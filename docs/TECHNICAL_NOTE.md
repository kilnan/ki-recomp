# Killer Instinct static recompilation: technical note (phase 1)

Status on 2026-10-18. Labels: **[S]** read in source code, **[O]** observed by
running the original code in MAME or by inspecting the game binary, **[H]**
hypothesis, **[?]** open.

## 1. Result in one paragraph

The main game program of Killer Instinct 1 (rev. L1.5D) is not on the hard
disk. It is stored compressed in the U98 boot EPROM, and the boot ROM unpacks it
into DRAM at 0x88000000 [O]. We extracted it with kipack and checked it word for
word against MAME's DRAM. A small dedicated translator (Python + rabbitizer)
turned two real game functions into C++ automatically: 0x8802d560 (a 64-bit
shift-register checksum, 20 instructions) and 0x8802d774 (compiled C with a
stack frame, 41 instructions). On 24 calls captured in MAME, 15 of them made by
the game itself, the recompiled code reproduces all 32 64-bit GPRs, HI, LO, the
return address and every byte of SRAM (512 KiB) and DRAM (8 MiB). A mutation
test confirms that the comparison detects wrong translations. The same
translator handles 519 of the 567 `jal` targets of the image, and the result
compiles. The 48 failures are mostly FPU instructions, which are not yet
supported.

## 2. Sources consulted

| source | revision | license | used for |
|---|---|---|---|
| MAME `src/mame/rare/kinst.cpp` | git ee0b4a91e670a47b55c0d17d2fd8c7522ab6f03c (2026-10-18) | BSD-3-Clause | memory map, devices, ROM hashes |
| MAME `src/devices/cpu/mips/mips3.cpp` | same | BSD-3-Clause | reference semantics (div, LWL/LWR), DRC/debugger behaviour |
| MAME `src/mame/shared/dcs.cpp` | same | BSD-3-Clause | DCS host interface |
| MAME `src/osd/modules/debugger/{none,debuggdbstub}.cpp`, `src/frontend/mame/luaengine_debug.cpp` | same | BSD-3-Clause | what can drive the debugger |
| MAME binary | 0.289 (Homebrew bottle) | | oracle; its `-listxml` hashes for `kinst` equal the source above |
| N64Recomp | ffb39cdad1da5de07eaaa48bd1db4a89a7986771 (2026-05-27) | MIT | reuse evaluation |
| nekuz0r/ki-rom | 8e4344cfd2b039e2ca97c9f115bb8f1acaff804b (2026-08-16) | AGPL-3.0-only | U98 compression format, kipack tool, disk notes |
| rabbitizer | 1.16.2 (PyPI) | MIT | instruction decoding |

No code from these projects is copied into the repository, which is MIT-licensed. The runtime follows
MAME's *behaviour* in the cases that are architecturally undefined, because
MAME is the oracle (see §6).

## 3. Hardware and software facts

### CPU and memory
- R4600, **little-endian** core (`R4600LE`), 100 MHz, 16 KiB I- and D-cache [S kinst.cpp:702-705]. ISA: MIPS III.
- Physical map (`kinst_map`) [S kinst.cpp:523-539]. Unmapped reads return all ones (`unmap_value_high`).

  | physical | size | device |
  |---|---|---|
  | 0x00000000 | 512 KiB | SRAM; it also holds both framebuffers |
  | 0x08000000 | 8 MiB | DRAM: game code, data and stack |
  | 0x10000080-0x100000b3 | | P1, P2, VOLUME/sound status, UNUSED, DSW (reads); vram control, sound reset/control/data, coin control (writes) |
  | 0x10000100-0x1000013f | | IDE CS0, one register per 8 bytes (`cs0_r(offset/2)` on a 32-bit handler) |
  | 0x10000170 | | IDE CS1 register 6 (alt status / device control) |
  | 0x1fc00000 | 512 KiB | U98 EPROM; each access costs 128 extra CPU cycles (`rom_r`) [S :505-513] |

  KI2 shuffles the I/O registers (`kinst2_map`) [S :542-554]. Only KI1 is considered here.
- The game runs in 32-bit kernel mode with 64-bit registers. Compiled code uses
  `dsll32/dsra32` for sign extension, and the checksum uses 64-bit shifts [O].
- TLB: at boot the game invalidates the TLB and creates a single mapping:
  VA 0x00000000-0x00000fff → PA 0x08090000, global, valid, dirty; the odd page
  is invalid. It is used for `$zero`-based addressing, for example
  `sd sp,0x1f0($zero)` in the exception stub [O code 0x880001ec-0x88000248, helper 0x8800b2e0].
- FPU: `FR=1` is set at boot (Status |= 0x24000000 at 0x88000258) and rom-0 contains
  733 COP1 instructions [O].

### Boot path and code provenance
1. Reset at 0xbfc00000, i.e. the U98 EPROM. The boot ROM tests the hardware and then
   decompresses a "br" stream. This custom Huffman coding works on the fields of MIPS
   instructions and is documented by ki-rom `tools/kipack/README.md`. The stream sits
   at U98 offset 0x0fd0, with tables at 0x0e48-0x0fcf [S ki-rom].
2. Unpacked images [O, kipack output]:

   | image | load address | size | content |
   |---|---|---|---|
   | rom-0 | 0x88000000 | 0x32cb0 | code (SHA1 ed875b9f2a0d1bccf79ff63189c37488ee718659) |
   | rom-2 | 0x88033880 | 0x80 | pointer table |
   | rom-1 | 0x88033900 | 0x51100 | data: jump tables, constants |

3. Cross-check [O, `tools/mame_verify_extraction.py`]: about 15 s into a MAME run with
   the original boot ROM, rom-0 and rom-2 equal DRAM exactly. rom-1 differs in 22
   words at 0x8803a154+, which the game had overwritten by then [H].
   Independent corroboration: ki-rom's AnyIDE patch for L1.5D writes at
   0x880105c0, where our image has the expected `mfc0 v0,$12` sequence.
4. Entry: rom-0 starts with `j 0x880001b8`. A MAME trace reaches 0x880001b8
   about 6 s into emulation (DRC core) [O]. ki-rom's replacement boot ROM jumps to
   0x88000000 [S ki-rom roms.S `rom_start`].

**Consequence:** the code to recompile is available from the 512 KiB U98 alone.
The 131 MB disk holds graphics and other data (ki-rom NOTES.md lists image
assets). Whether the disk also contains code loaded into RAM later is **[?]**:
it needs the real CHD and a watchpoint on executed DRAM outside rom-0.

### Disk
- ATA drive on CS0/CS1; its IRQ goes to CPU input 1 = Cause.IP3 [S :708]. The game
  polls Cause bit 0x800 in its IDE wait loops and does not take it as an interrupt [O 0x8802dbc8, 0x88010608].
- The driver patches the IDENTIFY model string to "ST9150AG" so that the game's
  drive check passes [S :332-350]. Geometry 419/13/47, 512-byte sectors.
- Function 0x8802d560 (our PoC) checksums 504/512-byte settings and audit
  sectors, and blocks of 1982 sectors (0xf7c00 bytes at 0x88300000) [O callers at
  0x8801e228, 0x8802ec3c, 0x8802ece4, 0x8802ed18, 0x8802f0fc, 0x8802f130]. This matches
  ki-rom's "sectors per block 0x7BE" disk notes.

### Video
- No video chip: the CPU writes a 320×240 frame of 16-bit BGR555 pixels (bit 15
  ignored, stride 640 bytes) into SRAM at 0x30000, or at 0x58000 when bit 2 of
  the vram control register (0x10000080) is set. This gives double buffering [S :360-382, :465-472].
- Pixel clock 6.25 MHz, 406×261 total, so 58.98 Hz. VBLANK drives CPU input 0 =
  Cause.IP2 [S :711-714], which the game polls as Cause bit 0x400 [O 0x88001418, 0x88012624].

### Interrupts and timers
- The game installs vectors in SRAM. The general exception vector at 0x80000180
  saves sp in the zero page and jumps to 0x880266c8. The TLB refill and other
  vectors at 0x80000000/080/100 go back to the boot ROM BEV vectors [O SRAM dump].
- Status is set to IM7|IE with BEV cleared (0x88026488). The only asynchronous
  interrupt is the COP0 Count/Compare timer (`mtc0 $11` at 3 sites). VBLANK and IDE are polled [O].
  At the 0x880266c8 handler, any real exception (Cause.ExcCode ≠ 0) takes a fatal path into the boot ROM.
- The game opens critical sections by toggling Status.IE (0x88003e44, 0x880105c0, ...).

### Inputs
- P1/P2/VOLUME/DSW are 32-bit reads, active low [S :572-678]. VOLUME bit 1 is the sound status bit.

### Sound (DCS)
- `DCS_AUDIO_2K` board: ADSP-2105 plus eight 512 KiB sample ROMs [S :721-723, :766-774].
- The interface is **one way** [S kinst.cpp:475-495, dcs.cpp:174-175, :1442-1453]:
  - write a 16-bit value to `sound_data` (0x10000098);
  - a rising edge of bit 1 in `sound_control` (0x10000090) calls `dcs->data_w`;
  - bit 0 of `sound_reset` (0x10000088) holds or releases the ADSP;
  - the only thing read back is the "input latch empty" flag (`control_r()` bit 11), seen as bit 1 of VOLUME.
- Consequence for the CPU [H]: audio affects execution only through the
  CPU waiting for that flag, so a hybrid needs a 16-bit latch and the flag.
  Behind them can sit either an emulated ADSP (as in MAME) or, at first, an
  acknowledge-immediately stub. How often and how long the game waits on the
  flag is **[?]**.

## 4. MAME as the oracle: what works and what does not

- Headless control: `-debug -debugger none` with commands sent from a Lua
  `-autoboot_script` (`manager.machine.debugger:command`). `-debugscript` is **not**
  executed with the `none` frontend [O, MAME 0.289]. `debug_none::wait_for_debugger`
  resumes immediately, so Lua cannot run while the emulation is stopped [S none.cpp].
- Captures use breakpoint actions only: `printf` of registers, read back by Lua
  from the console log, and `save` of SRAM/DRAM. The exit breakpoint sits on
  `$ra`, conditional on `$sp` matching its entry value.
- The gdb stub cannot be used for KI: it maps only the `r4600` (big-endian)
  short name, with 32-bit registers [S debuggdbstub.cpp:267, :719].
- **The DRC ignores register writes from the debugger** when the register is
  cached in a UML register; `state_import` only logs "must update UML"
  [S mips3.cpp:922-930]. Our first synthetic cases were silently wrong because
  of this, and the comparison caught it (a0 kept its old value). Cases that inject
  inputs therefore run with `-nodrc` (interpreter).
- With the interpreter, boot takes much longer in emulated time: 1-2 minutes of real time
  to the first call, against about 3 s with the DRC. Each instruction fetch from the EPROM is charged the 128-cycle
  wait state, which the DRC does not charge [S kinst.cpp:505-513; O].
- The order of calls is not reproducible between MAME runs: hit #10 in one run
  was hit #16 in another, apparently an extra write/verify round on the blank disk [O; cause ?].
  Cases are therefore selected by call site (`--when ra==...`) plus occurrence.

## 5. N64Recomp versus a dedicated translator

N64Recomp is strong where KI does not need it: ELF input with relocations,
symbol files, overlays and libultra hooks. Its central assumptions conflict
with KI:
- It assumes a big-endian guest: words are byteswapped on decode
  (src/recompilation.cpp:790), memory macros XOR byte/halfword addresses (`^3`, `^2`)
  and doublewords are split hi/lo (include/recomp.h:95-123).
- It expects an ELF plus TOML; KI only has a flat image with no symbols.
- Its COP0 support targets the N64. KI programs the TLB, enables the timer interrupt
  and takes exceptions through SRAM vectors.

**Decision:** a small dedicated translator (`tools/kirecomp/kirecomp.py`, about 500
lines). It uses rabbitizer, the same MIT decoder as N64Recomp, only to identify
instructions; the fields are decoded from the instruction word. The emitted C++ calls
`runtime/include/ki/runtime.h`. This keeps the guest model (LE, memory map, TLB,
faults) explicit. N64Recomp remains a reference for later problems: jump tables,
function-boundary analysis and mod hooks.

## 6. Translation semantics (summary)

- GPRs, HI and LO are `uint64_t`. 32-bit operations take the low 32 bits and sign-extend
  the result. Effective address = low 32 bits of `base + sext(imm)`.
- Delay slots: the branch condition is evaluated first, then the delay slot runs, then
  the branch is taken. Branch-likely forms execute the delay slot only when taken.
  `jal` sets `$ra`, runs the delay slot, then makes a direct C++ call. `jr $ra` returns,
  which assumes `$ra` is intact, as N64Recomp does. Other `jr`/`jalr` targets go
  through `ki::call_indirect`.
- `add/addi/sub/dadd/daddi/dsub` check for overflow and raise `ki::Fault::Overflow`.
  Loads into `$zero` are still performed, because I/O reads can have side effects.
  Misaligned accesses raise address errors. KUSEG/KSEG2 accesses raise `Fault::Tlb`
  (not yet modelled).
- Behaviour where the architecture leaves the result undefined, matched to MAME:
  division by zero leaves HI/LO unchanged, INT_MIN/−1 wraps, and LWR always sign-extends.
- No C/C++ undefined behaviour: unsigned arithmetic, C++20 arithmetic `>>`, `memcpy`
  for memory, a portable 64×64→128 multiply (no `__int128`, for MSVC), and a
  `static_assert` on a little-endian host.

## 7. Proof of feasibility

Per-case results: [poc/SUMMARY.md](../poc/SUMMARY.md). The full report with the original instructions and the
generated code is created locally in `poc/out/REPORT.md` (ignored by Git, because it is derived from the game).

| function | role | instr. | cases | of which real game calls | result |
|---|---|---|---|---|---|
| 0x8802d560 | checksum: `lbu`, 64-bit shifts, `addi`, delay slot | 20 | 15 | 11 (4 call sites, 3 also on the interpreter) | 15/15 exact |
| 0x8802d774 | compiled C: stack frame, table lookup, `sw` | 41 | 9 | 4 (2 call sites) | 9/9 exact |

- **Automatic:** translation (no hand edits), CMake regeneration from the image,
  capture, comparison and report.
  **Manual:** choosing the functions and the cases.
- Real cases come from the boot sequence with a **blank** disk. Several buffers
  are zero-filled disk reads, but the game-built settings sectors (`real-ecec-*`,
  `real-ed20-*`, `real-f104-1`) contain non-zero data. For 0x8802d774 the game only
  calls the `a0 == 2` path, and only `d774-real-f03c-7` reads a non-zero table entry.
  The `a0 == 1` path and large sums are covered by **synthetic** inputs only. These
  run the original code in MAME, but with inputs we chose.
- Negative control (`tests/mutation_check.py`): 5 of 5 non-equivalent mutants are caught.
  They are a 32-bit `dsrl32`, a dropped delay slot, a sign-extending `lbu`, a 64-bit
  result instead of 32-bit, and a 16-bit `sw`. The `sra`→`srl` mutant is equivalent for
  this function (its input comes from `lbu`) and correctly goes undetected.
- Limits: 0x8802d560 performs no stores, so for that function the memory check only
  proves that no stray writes happen. Neither function touches I/O, the TLB, COP0, the
  FPU, jump tables, calls or exceptions. Interrupts cannot occur inside these short
  calls in our runs (no memory changed outside the stack slot), but asynchronous
  timer interrupts are not modelled at all.

### Whole-image survey (translation only, not validated)
`--discover` on rom-0: 519 of the 567 `jal` targets are translated (37,829 instructions).
The output compiles cleanly (`-Wall -Wextra`) in about 5 s. Failures:
- 36 FPU (`lwc1`, `mtc1`, `mfc1`, `swc1`);
- 10 with unusual delay slots (a branch target or a jump inside a delay slot):
  hand-written assembly, or wrong boundaries [H];
- 2 boundaries not found.

Beyond the `jal` targets, the image contains 53 `jr` through registers other than
`$ra` (jump tables) and 42 `jalr` (function pointers).

## 8. Open questions

1. Does the disk contain code that is copied to RAM and executed? This needs `kinst.chd`.
2. Behaviour after the disk check: the main loop, frame pacing (polled VBLANK
   plus the Count/Compare timer interrupt) and what the timer handler does.
3. How the game uses the DCS ready flag: polling frequency and whether it blocks.
4. Which exceptions the game takes in normal play. The handler treats everything
   except interrupts as fatal [O]; expected: only the timer.
5. Self-modifying code or code copied at run time inside DRAM: none seen yet;
   the 2 `cache` instructions in rom-0 deserve a check.
6. The 10 functions with unusual delay slots: real hand-written assembly or
   boundary errors?
7. Why MAME call sequences differ between runs.

## 9. Next step (chosen from the evidence above)

Get the real `kinst.chd`. Then capture, in MAME, a frame-level reference past the
disk check: the set of executed DRAM addresses, a watchpoint for code executed
outside rom-0 (open question 1), and the Count/Compare/Cause timeline.
In parallel, without the disk: add the FPU (COP1, FR=1) and the fixed zero-page
TLB mapping to the translator and runtime, then validate one FPU-using function
with the same capture and compare pipeline. Together these remove the two largest
blockers measured by the survey (FPU failures and `Fault::Tlb` on `$zero`-based
globals). Only after that does it make sense to recompile the boot-time call
graph from 0x880001b8 and run it against a minimal I/O model.
