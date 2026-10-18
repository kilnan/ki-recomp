#!/usr/bin/env python3
"""Snapshot the full machine state in MAME at a function entry, mid-game.

Runs kinst with the scripted play inputs (tools/mame_inputs.py), arms a
breakpoint at --addr once the screen reaches --frame, and at the next hit
saves what tools/mame_capture.py saves for --entry-only (registers, COP0,
FPU, SRAM, DRAM) plus the screen position, then quits. ki_run can start from
such a snapshot (it enters at the saved pc), which allows bisecting a
divergence between the recompiled program and MAME.

usage: mame_snapshot.py --addr 88002038 --frame 1935 --out traces/snap1 [--play-seed 1]
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mame_run  # noqa: E402
from mame_capture import REGS, SRAM, DRAM, parse_regs, regs_printf  # noqa: E402
from mame_inputs import input_lua, play_script  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", required=True, type=lambda x: int(x, 16))
    ap.add_argument("--frame", required=True, type=int)
    ap.add_argument("--out", required=True)
    ap.add_argument("--play-seed", type=int, default=1)
    ap.add_argument("--cond", default="(sr & 2) == 0", help="breakpoint condition (default: not in an exception)")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--then", type=lambda x: int(x, 16), help="take a second snapshot at the next entry of this address")
    ap.add_argument("--then-out", help="directory of the second snapshot")
    args = ap.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    wd = out / ".work"
    shutil.rmtree(wd, ignore_errors=True)
    wd.mkdir()
    saves = [f"save {out}/entry_sram.bin,{SRAM[0]:x},{SRAM[1]:x}",
             f"save {out}/entry_dram.bin,{DRAM[0]:x},{DRAM[1]:x}"]
    # The screen position is printed from Lua right after the hit (see below).
    action = ";".join([regs_printf("KI_ENTRY"), *saves, 'printf "KI_SNAPSHOT_DONE"', "g"])
    then_out = Path(args.then_out).resolve() if args.then else None
    if then_out:
        then_out.mkdir(parents=True, exist_ok=True)
        then_saves = [f"save {then_out}/entry_sram.bin,{SRAM[0]:x},{SRAM[1]:x}",
                      f"save {then_out}/entry_dram.bin,{DRAM[0]:x},{DRAM[1]:x}"]
        then_action = ";".join([regs_printf("KI_THEN"), *then_saves, 'printf "KI_THEN_DONE"', "g"])
    events, _ = play_script(args.frame + 600, 10**9, args.play_seed)
    lua = f"""
local dbg = manager.machine.debugger
local cpu = manager.machine.devices[":maincpu"]
local scr = manager.machine.screens[":screen"]
local armed, seen, bp = false, 0, nil
local out = io.open({json.dumps(str(wd / 'console.log'))}, "w")
local function step()
  if not armed and scr:frame_number() >= {args.frame} then
    armed = true
    bp = cpu.debug:bpset({args.addr}, {json.dumps(args.cond)}, {json.dumps(action)})  -- not inside an exception handler
  end
  local log = dbg.consolelog
  for i = seen + 1, #log do
    out:write(log[i], "\\n")
    if string.find(log[i], "KI_SNAPSHOT_DONE", 1, true) then
      cpu.debug:bpclear(bp)
      out:write(string.format("KI_SCREEN frame=%d to_vblank_ns=%d\\n", scr:frame_number(),
        math.floor(scr:time_until_vblank_start():as_double() * 1e9)))
      {"bp = cpu.debug:bpset(" + str(args.then) + ", " + json.dumps(args.cond) + ", " + json.dumps(then_action) + ")" if args.then else "out:close(); manager.machine:exit(); return"}
    end
    if string.find(log[i], "KI_THEN_DONE", 1, true) then
      cpu.debug:bpclear(bp)
      out:write(string.format("KI_THEN_SCREEN frame=%d to_vblank_ns=%d\\n", scr:frame_number(),
        math.floor(scr:time_until_vblank_start():as_double() * 1e9)))
      out:close()
      manager.machine:exit()
      return
    end
  end
  seen = #log
end
local reported = false
emu.register_frame_done(function()
  local ok, err = pcall(step)
  if not ok and not reported then
    reported = true
    print("KI_LUA_ERROR " .. tostring(err))
  end
end)
{input_lua(events)}
cpu.debug:go()
"""
    (wd / "snap.lua").write_text(lua)
    rompath = str(ROOT / "data/roms")
    rc, output = mame_run.run([shutil.which("mame") or "mame", "kinst", "-rompath", rompath, "-video", "none",
                               "-sound", "none", "-nothrottle", "-skip_gameinfo", "-debug", "-debugger", "none",
                               "-nvram_directory", str(wd / "nv"), "-diff_directory", str(wd / "df"),
                               "-cfg_directory", str(wd / "cf"), "-autoboot_script", str(wd / "snap.lua")],
                              wd, args.timeout)
    lines = (wd / "console.log").read_text().splitlines() if (wd / "console.log").exists() else []
    entry = " ".join(l for l in lines if l.startswith("KI_ENTRY"))
    screen = next((l for l in lines if l.startswith("KI_SCREEN")), "")
    if not entry or not screen:
        print("snapshot failed", rc, [l for l in output.splitlines() if "KI_LUA_ERROR" in l or "LUA" in l][:3],
              output[-300:], file=sys.stderr)
        return 1
    regs = parse_regs(entry)
    (out / "entry_regs.txt").write_text("".join(f"{r} {regs[r]:016x}\n" for r in REGS))
    scr = parse_regs(screen.replace("KI_SCREEN ", ""))
    # parse_regs reads hex; frame/vpos/hpos were printed in decimal
    scr = {k: int(v) for k, v in (kv.split("=") for kv in screen.split()[1:])}
    (out / "screen.json").write_text(json.dumps(scr) + "\n")
    (out / "meta.json").write_text(json.dumps({"addr": f"{args.addr:08x}", "armed_at_frame": args.frame,
                                               "play_seed": args.play_seed, **scr}, indent=2) + "\n")
    if then_out:
        then_line = " ".join(l for l in lines if l.startswith("KI_THEN "))
        then_screen = next((l for l in lines if l.startswith("KI_THEN_SCREEN")), "")
        if not then_line or not then_screen:
            print("second snapshot missing", file=sys.stderr)
            return 1
        tregs = parse_regs(then_line)
        (then_out / "entry_regs.txt").write_text("".join(f"{r} {tregs[r]:016x}\n" for r in REGS))
        tscr = {k: int(v) for k, v in (kv.split("=") for kv in then_screen.split()[1:])}
        (then_out / "screen.json").write_text(json.dumps(tscr) + "\n")
        print(f"second snapshot at {args.then:08x}, frame {tscr['frame']} -> {then_out}")
    shutil.rmtree(wd, ignore_errors=True)
    print(f"snapshot at {args.addr:08x}, screen frame {scr['frame']}, {scr['to_vblank_ns']} ns to VBLANK -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
