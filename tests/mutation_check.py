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

# (name, exact text in build/gen/recompiled.cpp, replacement[, expected_caught])
# expected_caught=False marks an equivalent mutant: no input of the function
# can distinguish it, so a "miss" is the correct outcome.
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
    # t3 comes from lbu (0..255), so it is never negative: equivalent mutant.
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
            if gen.count(old) != 1:
                print(f"SKIP  {name}: pattern not found once in generated code")
                all_caught = False
                continue
            src = Path(td) / "mutant.cpp"
            src.write_text(gen.replace(old, new))
            exe = Path(td) / "compare_mutant"
            subprocess.run([cxx, "-std=c++20", "-O1", "-w", f"-I{ROOT}/runtime/include", str(src),
                            str(ROOT / "runtime/src/runtime.cpp"), str(ROOT / "tests/compare_case.cpp"),
                            "-o", str(exe)], check=True)
            failed = [c.name for c in case_dirs
                      if subprocess.run([str(exe), str(c)], capture_output=True).returncode != 0]
            caught = bool(failed)
            all_caught &= caught == expected
            label = ("CAUGHT" if caught else "MISSED") + ("" if expected else " (equivalent mutant, expected)")
            print(f"{label}  {name}: {len(failed)}/{len(case_dirs)} cases fail")
    return 0 if all_caught else 1


if __name__ == "__main__":
    sys.exit(main())
