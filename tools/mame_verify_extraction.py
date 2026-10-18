#!/usr/bin/env python3
"""Cross-check kipack's unpacked images against DRAM in MAME.

Boots kinst in MAME (original boot ROM, DRC core), dumps DRAM after the boot
ROM has jumped to the game, and compares each data/kipack-out/rom-N.bin with
the bytes at its load address. Words that differ are listed: the game may
legitimately have overwritten data by then (code should match exactly).
"""
import shutil
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mame_run  # noqa: E402
FRAMES = 900  # ~15 s emulated; the game is entered after ~6 s

LUA = """
local sp = manager.machine.devices[":maincpu"].spaces["program"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == {frames} then
    local f = io.open("{out}", "wb")
    for a = 0x08000000, 0x087ffffc, 4 do
      local v = sp:read_u32(a)
      f:write(string.char(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff))
    end
    f:close()
    manager.machine:exit()
  end
end)
"""


def main() -> int:
    rompath = [str(ROOT / "data/roms")]
    if not (ROOT / "data/roms/kinst/kinst.chd").exists():
        rompath.append(str(ROOT / "data/blankchd"))
    with tempfile.TemporaryDirectory() as td:
        wd = Path(td)
        dump = wd / "dram.bin"
        (wd / "dump.lua").write_text(LUA.format(frames=FRAMES, out=dump))
        mame_run.run([shutil.which("mame") or "mame", "kinst", "-rompath", ";".join(rompath),
                        "-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
                        "-nvram_directory", str(wd / "nvram"), "-diff_directory", str(wd / "diff"),
                        "-cfg_directory", str(wd / "cfg"), "-autoboot_script", str(wd / "dump.lua")],
                     wd, 300)
        dram = dump.read_bytes()
    ok = True
    for addr_file in sorted((ROOT / "data/kipack-out").glob("rom-*.addr")):
        (phys,) = struct.unpack("<I", addr_file.read_bytes())
        img = addr_file.with_suffix(".bin").read_bytes()
        off = phys - 0x08000000
        diffs = [i for i in range(0, len(img), 4) if img[i:i + 4] != dram[off + i:off + i + 4]]
        print(f"{addr_file.stem}: {len(img):#x} bytes at {phys | 0x80000000:#010x}: "
              f"{len(diffs)} differing words" + (f" (first {[hex(0x88000000 + off + d) for d in diffs[:4]]})" if diffs else ""))
        ok &= addr_file.stem != "rom-0" or not diffs
    print("code image rom-0 matches MAME DRAM" if ok else "MISMATCH in code image rom-0")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
