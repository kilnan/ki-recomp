#!/usr/bin/env python3
"""Capture reference CPU state around one call of a guest function in MAME.

MAME (with -debug -debugger none) runs the original boot ROM and game code.
Debugger commands are issued from a Lua autoboot script because MAME does not
execute -debugscript when the "none" debugger frontend is selected (observed
with MAME 0.289). For the N-th execution of the function entry:

  entry: optionally inject inputs (load a file into memory, set registers),
         print all GPRs + HI/LO, save SRAM and DRAM;
  then a breakpoint on the return address, conditional on $sp being back to
  its entry value (robust against recursion), prints/saves the same state.

Modes:
  list    -- print a0..a3/ra/sp for the first hits of the entry breakpoint
  capture -- write one case directory (regs as text, memory as raw dumps)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
FPR = [f"fpr{i}" for i in range(32)]
# ccr31 = FCSR. FPRs are printed as 64-bit raw values; the comparison only
# uses their low 32 bits (see tests/compare_case.cpp).
REGS = GPR + ["hi", "lo", "pc"] + FPR + ["ccr31"]
SRAM = (0x80000000, 0x80000)   # logical KSEG0 address, length
DRAM = (0x88000000, 0x800000)

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mame_run  # noqa: E402


def regs_printf(tag: str) -> str:
    # The debugger console wraps long lines, so print 9 registers per line.
    out = []
    for n in range(0, len(REGS), 9):
        group = REGS[n:n + 9]
        fmt = " ".join(f"{r}=%x" for r in group)
        out.append(f'printf "{tag} {fmt}",{",".join(group)}')
    return ";".join(out)


def lua_script(commands: list[str], console_out: Path, extra: str = "") -> str:
    cmds = ",\n".join("  " + json.dumps(c) for c in commands)
    return f"""
local dbg = manager.machine.debugger
local cpu = manager.machine.devices[":maincpu"]
local cmds = {{
{cmds}
}}
for _, c in ipairs(cmds) do dbg:command(c) end
local seen = 0
local out = io.open({json.dumps(str(console_out))}, "w")
local function flush()
  local log = dbg.consolelog
  for i = seen + 1, #log do out:write(log[i], "\\n") end
  seen = #log
  out:flush()
end
emu.register_frame_done(flush)
emu.add_machine_stop_notifier(function() flush(); out:close() end)
{extra}
cpu.debug:go()
"""


def run_mame(args, commands: list[str], workdir: Path, seconds: int) -> list[str]:
    console = workdir / "console.log"
    script = workdir / "capture.lua"
    extra = ""
    if args.play is not None:
        sys.path.insert(0, str(ROOT / "tools"))
        from mame_inputs import input_lua, play_script
        events, _ = play_script(seconds * 60, 10**9, args.play, args.play_start)
        extra = input_lua(events)
    script.write_text(lua_script(commands, console, extra))
    # Fresh NVRAM/diff each run so every capture starts from the same state.
    for d in ("nvram", "diff", "cfg"):
        shutil.rmtree(workdir / d, ignore_errors=True)
    cmd = [args.mame, "kinst", "-rompath", ";".join(args.rompath),
           "-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
           "-nvram_directory", str(workdir / "nvram"), "-diff_directory", str(workdir / "diff"),
           "-cfg_directory", str(workdir / "cfg"), "-snapshot_directory", str(workdir / "snap"),
           "-debug", "-debugger", "none", "-autoboot_script", str(script),
           "-seconds_to_run", str(seconds)]
    if args.bios:
        cmd += ["-bios", args.bios]
    if args.interp:
        # Use MAME's MIPS3 interpreter. Required when injecting registers: with
        # the DRC, registers cached in UML host registers ignore debugger writes
        # (mips3_device::state_import only logs "must update UML").
        cmd += ["-nodrc"]
    rc, output = mame_run.run(cmd, workdir, args.timeout)
    lines = console.read_text().splitlines() if console.exists() else []
    if rc is None:
        print(f"MAME killed after {args.timeout}s (wall clock)", file=sys.stderr)
    if args.verbose:
        print(output, *lines, sep="\n", file=sys.stderr)
    return lines


def parse_regs(line: str) -> dict[str, int]:
    return {k: int(v, 16) for k, v in re.findall(r"(\w+)=([0-9a-fA-F]+)", line)}


def cmd_list(args) -> int:
    with tempfile.TemporaryDirectory() as td:
        wd = Path(td)
        commands = ["temp0=0",
                    f'bpset {args.func:x},1,{{temp0=temp0+1;printf "KI_HIT n=%d v0=%x v1=%x a0=%x a1=%x a2=%x a3=%x ra=%x sp=%x",'
                    f'temp0,v0,v1,a0,a1,a2,a3,ra,sp;g}}']
        lines = run_mame(args, commands, wd, args.seconds)
    hits = [l for l in lines if l.startswith("KI_HIT")]
    for l in hits[: args.max]:
        print(l)
    print(f"{len(hits)} hits in {args.seconds} emulated seconds", file=sys.stderr)
    return 0


def cmd_capture(args) -> int:
    if (args.set) and not args.interp:
        print("--set requires --interp (DRC ignores debugger register writes)", file=sys.stderr)
        return 2
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    wd = out / ".work"
    wd.mkdir(exist_ok=True)
    inject = []
    for spec in args.load or []:
        f, addr = spec.rsplit("@", 1)
        inject.append(f"load {Path(f).resolve()},{int(addr, 16):x}")
    for spec in args.set or []:
        reg, val = spec.split("=", 1)
        inject.append(f"{reg}={int(val, 16):x}")

    def saves(prefix: str) -> list[str]:
        return [f"save {out}/{prefix}_sram.bin,{SRAM[0]:x},{SRAM[1]:x}",
                f"save {out}/{prefix}_dram.bin,{DRAM[0]:x},{DRAM[1]:x}"]

    exit_action = ";".join([regs_printf("KI_EXIT"), *saves("exit"), "quit"])
    entry_action = ";".join([*inject, regs_printf("KI_ENTRY"), *saves("entry"),
                             "temp1=sp", f"bpset ra,sp==temp1,{{{exit_action}}}", "g"])
    # Optional filter: only calls satisfying --when are counted by --hit.
    when = f"({args.when})" if args.when else "1"
    cond = f"((temp0=temp0+({when}!=0))=={args.hit})&&{when}"
    commands = ["temp0=0", f"bpset {args.func:x},{cond},{{{entry_action}}}"]
    lines = run_mame(args, commands, wd, args.seconds)
    (out / "console.log").write_text("\n".join(lines) + "\n")
    entry = " ".join(l for l in lines if l.startswith("KI_ENTRY")) or None
    exit_ = " ".join(l for l in lines if l.startswith("KI_EXIT")) or None
    if not entry or not exit_:
        print(f"capture failed (entry={bool(entry)} exit={bool(exit_)}); see {out}/console.log", file=sys.stderr)
        return 1
    for name, line in (("entry", entry), ("exit", exit_)):
        regs = parse_regs(line)
        missing = [r for r in REGS if r not in regs]
        if missing:
            print(f"missing registers {missing} in {name} line", file=sys.stderr)
            return 1
        (out / f"{name}_regs.txt").write_text("".join(f"{r} {regs[r]:016x}\n" for r in REGS))
    meta = {
        "function": f"{args.func:08x}", "hit": args.hit, "when": args.when, "inject": inject,
        "mame": subprocess.run([args.mame, "-version"], capture_output=True, text=True).stdout.strip(),
        "bios": args.bios or "default",
        "mips3_core": "interpreter" if args.interp else "drc",
        "play_seed": args.play,
        "play_start": args.play_start if args.play is not None else None,
        "disk": next(("blank" if "blankchd" in p else "kinst.chd") for p in args.rompath
                     if (Path(p) / "kinst/kinst.chd").exists()),
        "boot_rom_sha1": sha1_of_rom(args.rompath),
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    shutil.rmtree(wd, ignore_errors=True)
    print(f"captured hit {args.hit} of {args.func:08x} into {out}")
    return 0


def sha1_of_rom(rompath: list[str]) -> str | None:
    for p in rompath:
        f = Path(p) / "kinst" / "ki-l15d.u98"
        if f.exists():
            return hashlib.sha1(f.read_bytes()).hexdigest()
    return None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mame", default=shutil.which("mame") or "mame")
    ap.add_argument("--rompath", action="append",
                    help="MAME rompath entries (default: data/roms, then data/blankchd)")
    ap.add_argument("--bios", help="MAME -bios (default v1.5d)")
    ap.add_argument("--func", required=True, type=lambda x: int(x, 16))
    ap.add_argument("--interp", action="store_true", help="run MAME's MIPS3 interpreter (-nodrc)")
    ap.add_argument("--play", type=int, metavar="SEED",
                    help="drive inputs (coin, start, random buttons) to reach gameplay")
    ap.add_argument("--play-start", type=int, default=1500, metavar="FRAME",
                    help="frame of the first coin for --play (use ~8000 with --interp)")
    ap.add_argument("--seconds", type=int, default=30, help="emulated-time limit")
    ap.add_argument("--timeout", type=int, default=900, help="wall-clock limit for MAME (seconds)")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="mode", required=True)
    pl = sub.add_parser("list")
    pl.add_argument("--max", type=int, default=100)
    pc = sub.add_parser("capture")
    pc.add_argument("--hit", type=int, required=True, help="1-based index of the entry hit")
    pc.add_argument("--when", help="debugger expression; only matching calls are counted")
    pc.add_argument("--blank-disk", action="store_true",
                    help="boot with the all-zero disk (data/blankchd) even if kinst.chd is present")
    pc.add_argument("--play", dest="play_case", type=int, metavar="SEED", help="same as the global --play")
    pc.add_argument("--play-start", dest="play_start_case", type=int, metavar="FRAME",
                    help="same as the global --play-start")
    pc.add_argument("--seconds", dest="seconds_case", type=int, help="same as the global --seconds")
    pc.add_argument("--out", required=True)
    pc.add_argument("--load", action="append", metavar="FILE@ADDR", help="inject file into memory at entry")
    pc.add_argument("--set", action="append", metavar="REG=HEX", help="set register at entry")
    args = ap.parse_args(argv)
    if getattr(args, "play_case", None) is not None:
        args.play = args.play_case
    if getattr(args, "play_start_case", None) is not None:
        args.play_start = args.play_start_case
    if getattr(args, "seconds_case", None) is not None:
        args.seconds = args.seconds_case
    if not args.rompath:
        args.rompath = [str(ROOT / "data/roms")]
        if getattr(args, "blank_disk", False):
            # MAME takes the CHD from the first rompath entry that has one.
            args.rompath.insert(0, str(ROOT / "data/blankchd"))
        elif not (ROOT / "data/roms/kinst/kinst.chd").exists():
            args.rompath.append(str(ROOT / "data/blankchd"))
    args.rompath = [str(Path(p).resolve()) for p in args.rompath]
    return cmd_list(args) if args.mode == "list" else cmd_capture(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
