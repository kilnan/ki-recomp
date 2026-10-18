#!/usr/bin/env python3
"""Record which guest code MAME executes, optionally while driving inputs.

Runs kinst with the MIPS3 DRC and -drc_log_uml. Every block the DRC compiles
is logged with the virtual and physical address of each instruction, which
gives a cheap map of the code reached. It errs on the side of inclusion: a
block is compiled when entered, and its descriptors can include instructions
after a branch that was never taken. A block compiled again (for example after
self-modifying code) shows up as a repeated descriptor list.

The "play" scenario inserts a coin, presses start and then mashes random
(seeded) directions and buttons, to reach character select and fights.
Snapshots are saved every --snap-every frames.

Outputs in --out:
  coverage.txt        executed physical addresses (hex, one per line)
  regions.txt         executed ranges grouped by memory region
  functions.txt       rom-0 functions (jal targets) reached / not reached
  recompiled.txt      descriptor lists compiled more than once
  snap/               PNG snapshots
"""
import argparse
import re
import shutil
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools/kirecomp"))
sys.path.insert(0, str(ROOT / "tools"))
from mame_inputs import input_lua, play_script  # noqa: E402
import mame_run  # noqa: E402

DESC = re.compile(r"^([0-9A-F]{8}) \[([0-9A-F]{8})\] ")
LIST = re.compile(r"^Descriptor list @ ([0-9A-F]{8})")

REGIONS = [  # physical
    ("sram", 0x00000000, 0x00080000),
    ("dram:rom-0 code", 0x08000000, 0x08032CB0),
    ("dram:rom-2 data", 0x08033880, 0x08033900),
    ("dram:rom-1 data", 0x08033900, 0x08084A00),
    ("dram:other", 0x08000000, 0x08800000),
    ("boot eprom", 0x1FC00000, 0x1FC80000),
]


def region_of(phys):
    for name, lo, hi in REGIONS:
        if lo <= phys < hi:
            return name
    return "other"


def ranges(addrs):
    out, start, prev = [], None, None
    for a in sorted(addrs):
        if start is None:
            start = prev = a
        elif a == prev + 4:
            prev = a
        else:
            out.append((start, prev + 4))
            start = prev = a
    if start is not None:
        out.append((start, prev + 4))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=120, help="emulated seconds (60 frames each)")
    ap.add_argument("--scenario", choices=["attract", "play"], default="play")
    ap.add_argument("--snap-every", type=int, default=600)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=1800, help="wall-clock limit for MAME (seconds)")
    args = ap.parse_args()

    out = Path(args.out).resolve()
    shutil.rmtree(out, ignore_errors=True)
    wd = out / "work"
    wd.mkdir(parents=True)
    frames = args.seconds * 60
    events, snaps = play_script(frames, args.snap_every, args.seed) if args.scenario == "play" else \
        ([], list(range(args.snap_every, frames, args.snap_every)))
    (wd / "script.lua").write_text(input_lua(events, snaps, frames))
    rompath = str(ROOT / "data/roms")
    if not (ROOT / "data/roms/kinst/kinst.chd").exists():
        sys.exit("needs the real kinst.chd in data/roms/kinst/")
    rc, output = mame_run.run([shutil.which("mame") or "mame", "kinst", "-rompath", rompath, "-video", "none",
                    "-sound", "none", "-nothrottle", "-skip_gameinfo", "-drc_log_uml",
                    "-nvram_directory", str(wd / "nvram"), "-diff_directory", str(wd / "diff"),
                    "-cfg_directory", str(wd / "cfg"), "-snapshot_directory", str(out / "snap"),
                    "-autoboot_script", str(wd / "script.lua")], wd, args.timeout)
    (out / "mame.log").write_text(output)
    if rc != 0:
        print(f"MAME exited with {rc}; see {out}/mame.log", file=sys.stderr)

    log = wd / "drcuml_r4600le_maincpu.asm"
    executed, lists = set(), Counter()
    with open(log, errors="replace") as f:
        for line in f:
            m = DESC.match(line)
            if m:
                executed.add(int(m.group(2), 16))
                continue
            m = LIST.match(line)
            if m:
                lists[int(m.group(1), 16)] += 1
    (out / "coverage.txt").write_text("".join(f"{a:08x}\n" for a in sorted(executed)))

    by_region = {}
    for a in executed:
        by_region.setdefault(region_of(a), set()).add(a)
    lines = []
    for name, _, _ in REGIONS + [("other", 0, 0)]:
        s = by_region.get(name)
        if not s:
            continue
        rs = ranges(s)
        lines.append(f"{name}: {len(s)} instructions in {len(rs)} ranges")
        if name not in ("dram:rom-0 code", "boot eprom"):
            lines += [f"  {lo:08x}-{hi:08x}" for lo, hi in rs[:200]]
    (out / "regions.txt").write_text("\n".join(lines) + "\n")

    # rom-0 functions reached
    import kirecomp
    img = kirecomp.Image((ROOT / "data/kipack-out/rom-0.bin").read_bytes(), 0x88000000)
    funcs = sorted(kirecomp.discover_jal_targets(img) | {0x88000000})
    reached = [f for f in funcs if (f & 0x1FFFFFFF) in executed]
    (out / "functions.txt").write_text(
        f"{len(reached)}/{len(funcs)} jal-target functions reached\n" +
        "".join(f"{'R' if (f & 0x1FFFFFFF) in executed else '-'} {f:08x}\n" for f in funcs))
    again = {a: n for a, n in lists.items() if n > 1}
    (out / "recompiled.txt").write_text("".join(f"{a:08x} x{n}\n" for a, n in sorted(again.items())))
    log.unlink()
    print("\n".join(lines[:1] + [l for l in lines if not l.startswith("  ")]))
    print(f"{len(reached)}/{len(funcs)} jal-target functions reached; "
          f"{len(again)} descriptor lists compiled more than once; snapshots: {len(list((out / 'snap').rglob('*.png')))}")


if __name__ == "__main__":
    main()
