# Killer Instinct static recompilation: technical note (phase 1)

Status on 2026-10-18. Labels: **[S]** read in source code, **[O]** observed by
running the original code in MAME or by inspecting the game binary, **[H]**
hypothesis, **[?]** open.

## 1. Result in one paragraph

The main game program of Killer Instinct 1 (rev. L1.5D) is not on the hard
disk. It is stored compressed in the U98 boot EPROM, and the boot ROM unpacks it
into DRAM at 0x88000000 [O]. We extracted it with kipack and checked it word for
word against MAME's DRAM. With the real disk image, 15 minutes of scripted play
(character select, fights, game over, high scores, attract mode) execute no code
outside that image, apart from a 5-instruction exception stub in SRAM and the
boot ROM [O, §3]. A small dedicated translator (Python + rabbitizer) now turns
real game code into C++ automatically. The validated code is:
- six standalone functions: two single-precision FPU functions, a compiled
  `switch` with a jump table, and three integer functions;
- a whole call subtree of a drawing routine (eight entry points, including
  hand-written computed jumps and blend subroutines reached through `jalr`).

The subtree writes up to about 10 KB of framebuffer per call.

On 52 calls captured in MAME, 35 of them made by the game itself,
the recompiled code reproduces all 32 64-bit GPRs, HI, LO, the low halves of the
32 FPRs, FCSR, the return address, and every byte of SRAM (512 KiB) and DRAM
(8 MiB). A mutation test confirms that the comparison detects wrong translations,
and documents one coverage gap. The translator handles 562 of the 567 `jal`
targets of the image, plus 54 entry points found as code pointers in the data.
That includes jump tables and computed jumps, and the result compiles without
warnings.

## 2. Sources consulted

| source | revision | license | used for |
|---|---|---|---|
| MAME `src/mame/rare/kinst.cpp` | git ee0b4a91e670a47b55c0d17d2fd8c7522ab6f03c (2026-10-18) | BSD-3-Clause | memory map, devices, ROM hashes |
| MAME `src/devices/cpu/mips/mips3.cpp` | same | BSD-3-Clause | reference semantics (div, LWL/LWR), DRC/debugger behaviour |
| MAME `src/mame/shared/dcs.cpp` | same | BSD-3-Clause | DCS host interface, board behaviour (spec for `runtime/src/dcs.cpp`) |
| MAME `src/devices/cpu/adsp2100/` | same | BSD-3-Clause | ADSP-2105 core, **reused** in `third_party/adsp2100/` (§7d) |
| MAME `src/osd/modules/debugger/{none,debuggdbstub}.cpp`, `src/frontend/mame/luaengine_debug.cpp` | same | BSD-3-Clause | what can drive the debugger |
| MAME binary | 0.289 (Homebrew bottle) | | oracle; its `-listxml` hashes for `kinst` equal the source above |
| N64Recomp | ffb39cdad1da5de07eaaa48bd1db4a89a7986771 (2026-05-27) | MIT | reuse evaluation |
| nekuz0r/ki-rom | 8e4344cfd2b039e2ca97c9f115bb8f1acaff804b (2026-08-16) | AGPL-3.0-only | U98 compression format, kipack tool, disk notes |
| rabbitizer | 1.16.2 (PyPI) | MIT | instruction decoding |

Apart from the ADSP-2105 core in `third_party/adsp2100/` (derived from MAME,
BSD-3-Clause, see §7d), no code from these projects is copied into the
repository, which is MIT-licensed. The runtime follows
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
assets).

**No code is executed from the disk in the scenarios tested** [O,
`tools/mame_code_coverage.py`]. The tool runs MAME with the real `kinst.chd`
(content SHA1 verified) and `-drc_log_uml`, so every block the DRC compiles
is logged with its physical address. We ran two scripted sessions: coin, start,
character select, random inputs.
- **3 minutes:** fights, game over, name entry, high scores, back to attract mode
  (checked on snapshots).
- **15 minutes:** several games.

Code reached:
- rom-0: 35,892 instructions in 278 ranges, i.e. 433 of 568 `jal`-target functions;
- 5 instructions of the SRAM exception stub at 0x80000180;
- the boot EPROM.

No block was compiled twice, so no self-modifying or reloaded code was seen.

Limits:
- the DRC log is a superset of what actually executes;
- untested paths (test mode, endings, finishing moves, other characters) could
  in principle load code;
- a watchpoint-based check would be stricter.

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
- **Determinism:** identical capture runs are bit-identical. This was checked
  three times in parallel, and the 12 blank-disk DRC cases recaptured hours later
  match exactly [O].
- **Runs that stop the debugger diverge:** a `list` run stops at every call of the
  function and drifts away from a capture run, which stops only once. Hit #10 in a
  list was hit #16 in a capture, and some late calls seen in lists never happen in
  captures. Hypothesis [H]: each stop ends the current timeslice and changes the
  interleaving between the R4600 and the ADSP. Cases are therefore selected by call
  site (`--when ra==...`) or by a property of the inputs, plus occurrence.
- **Inputs:** scripted inputs (`tools/mame_inputs.py`) are timed on the screen's
  emulated frame number. Shifting them by one frame changes the whole game.
- **Parallel batch failure:** one batch of 8+ parallel MAME instances failed to
  reach calls that a lone run reached; the cause is unknown.
- **Resource hygiene:** the batch tools now cap parallelism at 3 and give every
  MAME process a wall-clock timeout, killing its process group (`tools/mame_run.py`).
  This was added after orphaned instances piled up.
- **Interpreter vs DRC differences in MAME (FPU):**
  - `CVT.W.S` truncates in the interpreter (`(int32_t)` cast), while the DRC uses
    FCSR's rounding mode (`UML_FSTOINT ... ROUND_DEFAULT`), as the hardware does
    [S mips3.cpp handle_cop1_fr1, mips3drc.cpp:3622].
  - 32-bit FPR writes zero the high half in the interpreter (`set_cop1_reg32`) and
    keep it in the DRC (`FPR32`).
  - The interpreter is therefore not a valid oracle for `CVT.W.S` on non-integers,
    and synthetic register injection needs the interpreter (see above).

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
- A branch may target the delay slot of another branch (loop rotation, e.g.
  0x8802d69c). That instruction is emitted twice: once as the delay slot, and once
  standalone at its label. Functions that run into the next function (shared
  tails, non-returning loops) end with an explicit tail call.
- `jr` to a register other than `$ra`:
  - A **jump table** is recognised from `lw rX, off(rA); jr rX`, where rA is a
    `lui`/`addiu` constant plus an index. The table is read from the code image or
    from the data images (rom-1, rom-2, passed with `--data`) while its entries point
    into the function, because table lengths are not encoded. Code emitted:
    `switch (target) { case …: goto label; default: call through the function table }`.
    During the boundary scan, case targets also extend the function, up to the next
    known entry point.
  - A **computed jump that cannot be resolved**: hand-written code jumping into
    unrolled loops (`subu at,s7,s2; jr at` in the 0x88010xxx drawing routines), or
    continuations held in registers. Every instruction of the function becomes a
    label and the jump dispatches over all of them. This is correct but heavy: it
    is used by 36 functions, and their switch-and-goto control flow makes the whole
    image take about 4.5 minutes to compile at `-O2`, against 6 s before. Splitting
    those functions into a separate `-O1` unit is the obvious mitigation.
- **Entry points** come from `jal` targets (`--discover`) and from words in the
  data images that point at code (`--discover-pointers`), excluding recognised
  jump-table cases. The latter finds functions reached only through `jalr`, such
  as the four BGR555 blend routines inside the drawing code at 0x880107a8, called
  through the table at 0x880356a4. Overlapping entry points are fine: each
  becomes its own C++ function, and the shared code is duplicated.
- **TLB:** only the single mapping the game creates is modelled (VA 0x0000-0x0fff
  → PA 0x08090000, for `$zero`-based globals). A MAME probe after boot confirmed
  that it is the only mapped page; any other mapped address raises `Fault::Tlb`.
- COP1 (Status.FR = 1, single precision only, as in the game):
  - FPRs are `uint64_t`; singles live in the low 32 bits, and writes keep the high
    half (DRC behaviour). The comparison only checks the low halves.
  - Arithmetic is host binary32: no `-ffast-math`; FLT_EVAL_METHOD is 0 on arm64
    and x86-64.
  - `CVT.W.S` honours FCSR.RM, with round-to-nearest ties to even. NaN and
    out-of-range inputs saturate; on the R4600 they would trap. Not validated.
  - `C.cond.S` sets FCSR bit 23, and `BC1F/BC1T` (and their "likely" forms) test it.
  - FCR0 reads 0x2020, MAME's PRId for the R4600.
  - FPU exceptions and FCSR.FS (flush to zero, set by the game) are not modelled;
    the captured cases contain no denormals.
- No C/C++ undefined behaviour: unsigned arithmetic, C++20 arithmetic `>>`, `memcpy`
  for memory, a portable 64×64→128 multiply (no `__int128`, for MSVC), and a
  `static_assert` on a little-endian host.

## 7. Proof of feasibility

Per-case results: [poc/SUMMARY.md](../poc/SUMMARY.md). The full report with the original instructions and the
generated code is created locally in `poc/out/REPORT.md` (ignored by Git, because it is derived from the game).

| function | role | instr. | cases | of which real game calls | result |
|---|---|---|---|---|---|
| 0x8802d560 | checksum: `lbu`, 64-bit shifts, `addi`, delay slot | 20 | 15 | 11 (4 call sites, 3 also on the interpreter; blank disk) | 15/15 exact |
| 0x8802d774 | compiled C: stack frame, table lookup, `sw` | 41 | 9 | 4 (2 call sites; blank disk) | 9/9 exact |
| 0x880176f0 | FPU: point-line distance, `c.eq/c.lt` + `bc1t/bc1f`, `sqrt.s`, `neg.s`, `cvt.w.s`, internal `j` | 50 | 7 | 3 (gameplay, real disk) | 7/7 exact |
| 0x8801a1e8 | FPU: distance and scaled unit vector, `cvt.s.w`, `c.olt.s`, `div.s`, `cvt.w.s` results in FPRs | 37 | 5 | 5 (gameplay, real disk; one at distance 0, one with negative coordinates) | 5/5 exact |
| 0x8802d694 | table fill loop; a branch targets a delay slot | 12 | 4 | 3 (after coin insert, real disk) | 4/4 exact |
| 0x8802dc94 | compiled `switch`: `sltiu` bound check, 6-entry jump table in rom-2, default case | 28 | 9 | 6 (cases 1-5 and the default; real disk) | 9/9 exact |
| 0x880274c8 + callees | drawing subtree: 0x880274c8, 0x8801905c, 0x880123d4 and 0x880107a8 (1599 instr.: jump table, 23 computed jumps, 10 `jalr`), plus the blend routines 0x88011dac, 0x88010d8c, 0x88010d78 and 0x88010da8; zero-page globals | 1,800+ | 3 | 3 (gameplay, real disk; 58 / 6,038 / 9,930 bytes of SRAM framebuffer written) | 3/3 exact |

- **Automatic:** translation (no hand edits), CMake regeneration from the image,
  capture, comparison and report.
  **Manual:** choosing the functions and the cases.
- **Reproducibility:** the 12 blank-disk DRC cases were recaptured hours later with
  `--blank-disk` and matched bit for bit.
- **Real cases:**
  - The first 24 cases come from the boot sequence with a **blank** disk. Several
    buffers are zero-filled disk reads, but the game-built settings sectors contain
    non-zero data.
  - The FPU, table-fill and `switch` cases come from scripted gameplay with the
    **real** disk.
  - Case 0, the bound (6) and a "negative" index of the `switch` are synthetic.
  - For 0x8802d774 the game only calls the `a0 == 2` path. The `a0 == 1` path,
    large sums, empty ranges and the special vectors of 0x880176f0 are covered by
    **synthetic** inputs only: the original code runs in MAME, but with inputs we chose.
  - Register injection uses MAME's interpreter. The 0x880176f0 synthetic cases only
    inject memory, so they run on the DRC.
- **Negative control** (`tests/mutation_check.py`):
  - **Caught (10 of 10 that must be caught):** 32-bit `dsrl32`, dropped delay slot,
    sign-extending `lbu`, 64-bit result instead of 32, 16-bit `sw`, `c.lt` evaluated
    as `c.le`, dropped `sqrt.s`, truncating `cvt.w.s` in 0x880176f0, a jump-table case
    sent to the wrong label, and a wrong colour mask in the blend routine reached
    through `jalr` (caught by all three subtree cases).
  - The truncating-`cvt.w.s` mutant is caught by `syn-fpu-76f0-random2` (−6.63
    rounds to −7, truncates to −6). That case injects memory only and runs on the
    DRC, which confirms that the oracle rounds by FCSR.
  - **Equivalent, correctly not caught:** `sra`→`srl` on an `lbu`-derived value, and a
    binary32 operation computed in binary64 then rounded once (for +, −, × and ÷ the
    result is identical).
  - **Known coverage gap:** truncating `cvt.w.s` in 0x8801a1e8. In every captured
    call both points share the same y, so the converted values are integers. A
    synthetic case would need register injection, i.e. the interpreter, which
    truncates.
- **Limits:**
  - No validated code touches I/O, COP0 or exceptions.
  - The subtree cases exercise calls, `jalr` through a pointer table, the zero page
    and the all-labels fallback of 0x880107a8. Which of its 23 computed jumps were
    actually taken has not been traced.
  - Timer interrupts are not modelled.
  - In our captures, no memory outside the function's own writes changed during a
    call, so no interrupt handler ran inside one.

### Whole-image survey (translation only, not validated)
`--discover` on rom-0 with the data images: 562 of the 567 `jal` targets are
translated, about 44,000 instructions. With `--discover-pointers`, 54 more entry
points come from code pointers in the data, all of which translate (616 functions,
about 47,600 instructions). The output compiles with `-Wall -Wextra`
and no warnings (about 4.5 minutes at `-O2`, see §6).

Of the 60 `jr` sites through a register other than `$ra`:
- **7 are recognised jump tables.** 5 of them are dispatched in translated
  functions; the table at 0x880062b0 is shared by two overlapping entry points.
  Of the other 2 tables, 0x8803416c sits in the large region at 0x88002038, which
  fails to translate (see below). The cases of 0x8800f6d0 fall outside the bounds
  the scan assigns to 0x8800ebac **[?]**, so its `jr` uses the all-labels fallback;
- **the rest are computed jumps** (handled by the all-labels fallback) or calls
  through trampolines.

Remaining failures:
- **3 functions with a branch inside a delay slot** (0x8800ba10, which never ran in
  our coverage; 0x880105b0; 0x8803228c). This is UNPREDICTABLE in the ISA and is
  hand-written spin or delay code, e.g. `bgez; bltz` at 0x880106b8. Reproducing it
  needs MAME's exact behaviour or a small interpreter fallback.
- **2 very large regions** starting at 0x88002038 and 0x88008cbc. The first looks
  like a bytecode interpreter: an opcode is read with `lbu` from `s8` and
  dispatched through the table at 0x8803416c. Both regions contain `jal` targets
  in their middle, and the linear scan runs into data (`INVALID`). With other
  bounds they also contain branches in delay slots (0x88002664, 0x8800a790).
  They need explicit bounds, or analysis of the bytecode dispatch.

## 7b. Whole program on a board model (`ki_run`)

All 731 entry points the translator finds are compiled into one executable,
together with a model of the board:
- **Entry points:** `jal` targets, code pointers in the data images, and
  tail-jump and fall-through targets found iteratively until no new ones appear.
- **Board** (`runtime/src/kinst_board.cpp`):
  - the kinst.cpp I/O map;
  - an ATA drive in PIO mode on a raw image extracted from the CHD (its SHA1
    matches the CHD's data SHA1);
  - VBLANK on Int0 and the IDE interrupt on Int1, both polled by the game;
  - the COP0 Count/Compare timer on IP7. When the timer is due, the runtime
    reproduces the SRAM vector stub and calls the recompiled handler at
    0x880266c8; `eret` returns from it.
- **Time:** one cycle per executed guest instruction, added per basic block.
  Safe points (function entry, backward jumps, dispatch jumps) deliver interrupts
  and board events.
- **Start state:** a MAME snapshot at 0x88000000, i.e. what the original boot
  ROM leaves behind.

**Result [O]:** the recompiled program boots and plays the attract mode.
- It shows the copyright screen with the Killer Instinct logo, the animated
  Rareware logo and the Midway screen.
- It reads about 5,600 sectors, and takes 0.33 s of host time for 600 frames.
- These are the same screens MAME shows. Their timing and pixels have not yet
  been compared frame by frame.

Problems found and fixed on the way (all general, none patched per address):
- Returns through registers other than `$ra` (`move a1,ra … jr a1`): each function
  remembers the `$ra` it was called with.
- A dispatcher that is called with `jal` and comes back with `j`: calls to an
  address inside the same function become `goto`, with `jr $ra` switching over
  the local return sites, so the host stack stays flat.
- Tail jumps to addresses no `jal` reaches.
- INITIALIZE DEVICE PARAMETERS (0x91). The game switches CHS addressing to
  14 heads × 40 sectors (as MAME's `set_geometry`). Without it, the graphics
  decompressor (0x88031170) found bad data and stopped on its assertion
  `syscall` at 0x880311a4. This is the same address MAME stopped at with the
  blank disk.

Not modelled yet:
- display, inputs and sound (DCS);
- the exact cycle timing of the R4600 (caches, multiply/divide latencies);
- disk writes are kept in memory only.

## 7c. From attract mode to full games (18/10 evening)

A 3-minute scripted session now plays without faults: coin, character select,
a fight, game over, high scores and back to attract mode. It issues 826 disk
commands. `ki_play` shows it in a window with keyboard input.

The bugs were found by lockstep comparison with MAME:
1. Snapshots of the full machine state are taken in MAME mid-game
   (`tools/mame_snapshot.py`, two in one run when they must be comparable).
2. The recompiled program is started from such a snapshot (`ki_run --boot`).
3. The states are compared at the same function entry (`--dump-at`).
4. The executed PCs are diffed against a MAME `trace` (`ki_run_trace`).

Fixes, all general:
- **VS-screen hang.** The object loop entry 0x880021c8 had control-flow bounds
  that include lower addresses, and the generated function started at its lowest
  address (0x88002060) instead of its entry. The game re-processed object 0
  forever. A one-line fix, found by the PC-level diff, which diverged at the
  first instruction of that function.
- **Disk.** INITIALIZE DEVICE PARAMETERS sets the logical CHS geometry to
  14 × 40. The drive check at 0x880105b0 compares IDENTIFY words 27-31 with
  "ST9150AG  "; on a mismatch the game deliberately corrupts memory. Command,
  seek and transfer timings follow MAME.
- **Control flow:**
  - tail calls run through a trampoline, so long jump chains in the bytecode
    interpreter keep the host stack bounded;
  - cases outside a function become entry points;
  - leading unused jump-table slots are skipped;
  - unresolved computed jumps (into unrolled code) do not end a function;
  - code pointers built with `lui`/`addiu` are entry points;
  - branch-and-link (`bgezal`/`bltzal`) is a conditional call. The background
    tile renderer (0x88030c78) calls subroutines of its own function that way;
    translated as a plain branch, their `jr $ra` left the whole renderer, so the
    fight stage was never drawn and fighters left trails. Found by comparing the
    set of PCs executed in one frame (MAME `trace` vs `ki_run_trace`) from the
    same snapshot: 180 instructions ran only in MAME. After the fix the frame's
    back buffer differs in 276 pixels instead of 46,648.

Pitfalls of MAME as an oracle (in addition to §4):
- **Memory snapshots are a little late.** With the DRC and `-debugger none`,
  memory saved by a breakpoint action reflects a slightly later point than the
  registers printed by the same action. A table written by code after the
  breakpoint was already present in the dump.
- **Breakpoints change the game.** Adding breakpoints at different times changes
  the emulated timeline (for example the CPU opponent picked). Comparisons must
  come from the same run.
- **Mid-game starts need board state.** Starting the recompiled program from a
  mid-game snapshot also needs the board state that the snapshot lacks; for now
  that is the logical disk geometry.

Still open:
- **Timing.** One cycle per instruction, which matches MAME's frame timing for
  20.8 s of attract mode (§7e); one frame slips after that.
- **Graphics fidelity.** Long sessions are not yet compared frame by frame with
  MAME. Single frames from mid-fight snapshots match except for a few hundred
  pixels (snapshot timing, see above).
- **Memory write taps** do not see DRC writes to RAM (fast RAM bypasses the
  handlers), so "who wrote this pixel" needs a trace instead.

## 7d. Sound: the DCS board (19/10 night)

Killer Instinct uses Midway's ROM-based DCS board: an ADSP-2105 DSP at 10 MHz,
2K words of RAM shared between program and data space, eight 512 KiB sample
ROMs and a 16-bit mono DAC at 31250 Hz [S dcs.cpp:12-24, kinst.cpp:721-774].

**Choice: emulate the DSP, at low level.** The alternative, replacing the board
with native code that interprets the game's sound commands, would need the DCS
command set and its compressed sample format reverse-engineered first, and it
could not be checked against MAME sample by sample. The DSP is a peripheral
with its own code in the sound ROMs (loaded at boot and rebooted from another
ROM bank by the game), so an interpreter is the simplest faithful option. It
costs about 0.5 s of host time per 20 s of game.

- **DSP core:** `third_party/adsp2100/`, MAME's ADSP-21xx core with the device
  framework removed (BSD-3-Clause, notice kept). Writing an equivalent core from
  the manual would have duplicated about 4,000 lines of instruction semantics
  with no gain in fidelity.
- **Board:** `runtime/src/dcs.cpp`, written for this project after MAME's
  `dcs.cpp` (REV_DCS1 paths):
  - memory map: program and data RAM, the 4K-word ROM bank window, bank select;
  - input latch: IRQ2 and the "input empty" flag, acknowledged when the DSP reads it;
  - internal timer, including MAME's "DRAM refresh only" shortcut;
  - SPORT1 autobuffer: every half buffer, the samples go to the DAC; on wrap, IRQ1.
- **Host side:** the CPU's writes to sound_reset, sound_control and sound_data
  drive the board as in `kinst.cpp`. The VOLUME read now returns the real
  input-empty flag. The DSP is advanced to the CPU's time at every sound access
  and at safe points.

**Scheduling details that mattered** (each found as a sample mismatch):
- Interrupts are sampled only when a DSP slice starts, as in MAME. IRQ1 is a
  pulse (`pulse_input_line` for the minimum quantum), so the slice after it is
  one instruction long. With a longer pulse the DSP took the interrupt late,
  after an RTI, and the boot-time beep came 1.6 s late.
- When the DSP schedules an event (DAC start, timer write), the current slice
  ends at once. MAME does the same by aborting the timeslice.
- While the DAC is off, silence is output, so the stream starts at power-on.

**Verification [O]:**
- **Board alone:** MAME records 150 s of a scripted session (attract mode,
  coin, fights) at 31250 Hz together with every sound register write
  (`tools/mame_dcs_capture.py`). `tests/dcs_replay.cpp` replays the 2,806
  writes on the model. All **4,599,167** non-silent samples are identical. The
  two streams are aligned in two segments (MAME's DAC device buffers 241, then
  351 samples after the game reboots the DSP); the content does not differ.
- **Whole program:** `ki_run` from the boot snapshot, with the recompiled CPU
  driving the board, produces 20 s of attract-mode audio. All **627,116**
  samples after the first non-silent one equal MAME's recording.
- **Not verified:** gameplay audio of the recompiled program against MAME. The
  two runs diverge, since the game's random numbers depend on timing, so their
  sound commands differ. The board model is verified for those commands by the
  replay above.

`ki_play` queues the samples to SDL at 31250 Hz and slows its frame pacing
slightly when more than about 60 ms of audio are queued.

## 7e. Timing against MAME, and portability (19/10)

**Method [O].** In a normal MAME run (no `-debug`), Lua write taps on I/O
registers log emulated times. The attract mode is compared with `ki_run` started
from the boot snapshot:
- framebuffer flips (writes to 0x10000080, `KI_TRACE_VRAM=1`);
- disk commands (`KI_TRACE_IDE=1`);
- screen pixels every frame;
- audio.

Fixes, both general:
- **VBLANK phase.** The boot snapshot had no beam position, so our frames were
  about 9 ms out of phase with MAME's. `mame_capture.py --entry-only` and
  `mame_snapshot.py` now compute it from the debugger's `totalcycles` at the
  breakpoint (the screen starts at time 0; one frame is 406 x 261 pixels of 16
  CPU cycles), and write `screen.json`.
- **Disk head position.** MAME moves the head with every sector of a
  multi-sector transfer (`next_sector()` updates `m_cur_lba`), so a read that
  continues where the last one stopped costs 400 ns. We kept the head at the
  first sector and charged a 1.3 ms seek. The disk model now tracks the head
  per sector.

**Results [O]:**
- **Frame timing.** 512 framebuffer flips are within 1.2 µs of MAME's for the
  first 20.8 s.
- **Disk.** All disk commands within 30 µs up to the 52nd command.
- **Screen.** The first 1,230 frames are identical pixel for pixel.
- **Audio.** The first 21.77 s are identical.

**One cycle per instruction.** MAME's DRC front end lists extra cycles for
`mult` (3), `div` (35), `dmult` (7), `ddiv` (67) and for reading Count or Cause
(`mips3fe.cpp`).
- Adding the multiply/divide costs to the translator made the flips diverge
  from MAME at 0.24 s instead of 20.8 s.
- The Count/Cause costs changed nothing.
- So the translator keeps one cycle per instruction, which is what matches.

Why MAME's run does not show these costs is **[?]**.

**Still open.** One frame slips at 20.84 s, in a stretch with no disk or sound
access: our work between two flips takes at least 3.3 ms more than MAME's.
- MAME traces cannot be used to find out why: with `-debug` MAME's own
  timeline changes, so the same moment shows different frame numbers and flip
  times.
- The game keeps running correctly after the slip; the two runs are just one
  frame apart.

**Portability [O]:**
- **Linux:** Debian 12 arm64, GCC 12, in Docker (`tools/docker/Dockerfile.linux`).
  All 53 tests pass, and the `dcs_replay` comparison is 100% equal. 1200 frames
  of `ki_run` give the same WAV and the same framebuffer as macOS, byte for byte.
  - Needed: the generated-code libraries and the runtime depend on each other.
    GNU ld resolves static libraries in one pass, so each code library is listed
    again after the runtime (`ki_code_library()` in CMake).
  - The 785 `-Wmisleading-indentation` warnings from GCC in generated code are fixed.
- **Windows:** MinGW-w64 x86-64 cross-compiles everything except `ki_play`
  (no SDL in that environment). The executables were not run.
- **Negative control:** rerun after these changes, same result as §7 (10/10
  caught, one known gap, two equivalent mutants).

## 8. Open questions

1. Code from the disk: none in 15 minutes of scripted play [O]. Still untested:
   test mode, endings, every character, and a stricter watchpoint check.
2. The main loop and frame pacing: polled VBLANK plus the Count/Compare timer
   interrupt. What does the timer handler at 0x880266c8 do?
3. How long the game waits on the DCS ready flag now that it is real (the DSP
   acknowledges within microseconds).
4. Which exceptions the game takes in normal play. The handler treats everything
   except interrupts as fatal [O]; expected: only the timer.
5. What the 2 `cache` instructions in rom-0 do (no recompiled blocks were seen).
6. The exact semantics of the 3 branch-in-delay-slot sequences, in MAME and on hardware.
7. Why a list run (many debugger stops) diverges from a capture run (hypothesis
   in §4), and why one batch of 8+ parallel instances failed.

## 9. Next step (chosen from the evidence above)

The evidence now points to whole-program recompilation:
- all game code is in rom-0;
- 562 of 567 functions translate;
- the FPU is validated.

**Update:** the recompiled program now boots to the attract mode (§7b). The
list below is kept for reference. The next steps are a frame-by-frame
comparison with MAME (framebuffer and call sequence), then SDL for display and
inputs, then sound. Remaining items:
1. Bounds for the two large regions and their bytecode dispatch (0x88002038,
   0x88008cbc), and a fallback for the 3 branch-in-delay-slot sequences.
2. A COP0 model: Status, Cause (with VBLANK/IDE pending bits supplied by the I/O
   model) and Count/Compare. Deliver the timer interrupt at safe points (function
   entry and loop back-edges) by calling the recompiled handler.
3. A lockstep check against MAME at function granularity. Run the recompiled boot
   path from 0x880001b8 with an I/O model that **replays the I/O reads recorded
   in MAME**, and compare the sequence of calls and the framebuffer up to the
   first frame of attract mode. The coverage tool's call data says which
   functions to expect.

SDL is still unnecessary until step 4 produces a framebuffer worth displaying.
