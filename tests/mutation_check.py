#!/usr/bin/env python3
"""Negative control for the PoC comparison: inject plausible translation bugs
into the generated C++ and check that the MAME captures reject every mutant.

usage: mutation_check.py <build_dir> <cases_dir>
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (name, exact text in build/gen/recompiled.cpp, replacement[, expectation[, scope]])
# scope: guest address of the function whose body the replacement is limited
# to (needed when the same code is emitted in several functions).
# expectation: True (default) = must be caught; False = equivalent mutant, no
# input can distinguish it; "gap" = distinguishable, but no current case does
# (documented coverage gap, reported but not failing the check).
MUTANTS = [
    ("dsrl32 done as 32-bit shift (0x8802d574)",
     "c.r[3] = c.r[3] >> 32;  // 8802d574", "c.r[3] = ki::sx32(ki::lo32(c.r[3] >> 0));  // 8802d574"),
    ("delay slot executed after the taken branch (0x8802d5a0)",
     "        c.r[8] = c.r[8] ^ c.r[7];  // 8802d5a0", "        ;  // 8802d5a0"),
    ("lbu sign-extends (0x8802d58c)",
     "c.r[7] = uint64_t(ki::load<uint8_t>(c, ki::ea(c.r[4], 0), 0x8802d58cu));",
     "c.r[7] = ki::sx8(ki::load<uint8_t>(c, ki::ea(c.r[4], 0), 0x8802d58cu));"),
    ("final dsrl32 keeps 64 bits (0x8802d5ac)",
     "c.r[2] = c.r[8] >> 32;  // 8802d5ac", "c.r[2] = c.r[8];  // 8802d5ac"),
    ("sw stores only 16 bits (0x8802d808)",
     "ki::store<uint32_t>(c, ki::ea(c.r[29], 4), static_cast<uint32_t>(c.r[13]), 0x8802d808u);",
     "ki::store<uint16_t>(c, ki::ea(c.r[29], 4), static_cast<uint16_t>(c.r[13]), 0x8802d808u);"),
    # FPU (0x880176f0, 0x8801a1e8)
    # In every captured real call of 0x8801a1e8 both points share y, so the
    # converted values are exact integers; synthetic register inputs would need
    # MAME's interpreter, whose CVT.W.S truncates (unusable as oracle here).
    ("cvt.w.s truncates (0x8801a264)",
     "ki::set_fbits(c, 1, static_cast<uint32_t>(ki::cvt_w_s(c, ki::fs(c, 1))));",
     "ki::set_fbits(c, 1, static_cast<uint32_t>(static_cast<int32_t>(ki::fs(c, 1))));", "gap"),
    ("c.lt.s evaluated as c.le.s (0x88017778)",
     "ki::set_fcc(c, ki::fcompare(12, ki::fs(c, 12), ki::fs(c, 8)));",
     "ki::set_fcc(c, ki::fcompare(14, ki::fs(c, 12), ki::fs(c, 8)));"),
    ("sqrt.s dropped (0x8801a22c)",
     "ki::set_fs(c, 0, std::sqrt(ki::fs(c, 8)));", "ki::set_fs(c, 0, ki::fs(c, 8));"),
    ("cvt.w.s truncates (0x880177a0)",
     "ki::set_fbits(c, 10, static_cast<uint32_t>(ki::cvt_w_s(c, ki::fs(c, 9))));",
     "ki::set_fbits(c, 10, static_cast<uint32_t>(static_cast<int32_t>(ki::fs(c, 9))));"),
    # binary32 +,-,*,/ computed in binary64 then rounded once to binary32 give
    # the correctly rounded result (53 >= 2*24+2): equivalent mutant.
    ("single op evaluated in double precision (0x88017758)",
     "ki::set_fs(c, 5, ki::fs(c, 5) / ki::fs(c, 16));",
     "ki::set_fs(c, 5, static_cast<float>(double(ki::fs(c, 5)) / double(ki::fs(c, 16))));", False),
    # jump table (0x8802dc94)
    ("jump table: case 1 dispatched to case 2's code",
     "case 0x8802dcc0u: goto L_8802dcc0;", "case 0x8802dcc0u: goto L_8802dcc8;"),
    # call subtree of 0x880274c8: blend routine reached through jalr (0x88010da8)
    ("blend mask 0x739c -> 0x7bde in the jalr target 0x88010da8",
     "c.r[22] = c.r[25] & 0x739cull;", "c.r[22] = c.r[25] & 0x7bdeull;", True, 0x88010da8),
    # t3 comes from lbu (0x0..0xff), so it is never negative: equivalent mutant.
    ("sra done as logical shift (0x8802d7f8)",
     "c.r[12] = ki::sra32(c.r[11], 1);", "c.r[12] = ki::sx32(ki::lo32(c.r[11]) >> 1);", False),
]


def main() -> int:
    build, cases = Path(sys.argv[1]), Path(sys.argv[2])
    gen = (build / "gen/recompiled.cpp").read_text()
    case_dirs = sorted(p for p in cases.iterdir() if (p / "exit_regs.txt").exists())
    cxx = os.environ.get("CXX", "c++")
    all_caught = True
    with tempfile.TemporaryDirectory() as td:
        for name, old, new, *rest in MUTANTS:
            expected = rest[0] if rest else True
            scope = rest[1] if len(rest) > 1 else None
            lo, hi = 0, len(gen)
            if scope is not None:
                lo = gen.index(f"void func_{scope:08x}(ki::Context& c) {{")
                hi = gen.index("\n}\n", lo)
            body = gen[lo:hi]
            if body.count(old) != 1:
                print(f"SKIP  {name}: pattern not found once in generated code")
                all_caught = False
                continue
            src = Path(td) / "mutant.cpp"
            src.write_text(gen[:lo] + body.replace(old, new) + gen[hi:])
            exe = Path(td) / "compare_mutant"
            subprocess.run([cxx, "-std=c++20", "-O1", "-w", f"-I{ROOT}/runtime/include", str(src),
                            str(ROOT / "runtime/src/runtime.cpp"), str(ROOT / "tests/compare_case.cpp"),
                            "-o", str(exe)], check=True)
            failed = [c.name for c in case_dirs
                      if subprocess.run([str(exe), str(c)], capture_output=True).returncode != 0]
            caught = bool(failed)
            if expected != "gap":
                all_caught &= caught == expected
            note = {True: "", False: " (equivalent mutant, expected)", "gap": " (known coverage gap)"}[expected]
            label = ("CAUGHT" if caught else "MISSED") + note
            print(f"{label}  {name}: {len(failed)}/{len(case_dirs)} cases fail")
    return 0 if all_caught else 1


if __name__ == "__main__":
    sys.exit(main())
