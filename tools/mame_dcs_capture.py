#!/usr/bin/env python3
"""Record the DCS sound command stream and the audio output of a MAME kinst run.

Runs kinst with the scripted play inputs (tools/mame_inputs.py) for --seconds of
emulated time, logs every CPU write to the sound registers (sound_reset 0x10000088,
sound_control 0x10000090, sound_data 0x10000098) with its emulated time, and
records the audio with -wavwrite at the DAC rate (31250 Hz), so no resampling.
tests/dcs_replay feeds the same writes to the DCS model and compares the output.

usage: mame_dcs_capture.py --seconds 120 --out traces/dcs_play
writes: OUT/writes.txt ("time_ns address value" per line), OUT/mame.wav
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mame_run  # noqa: E402
from mame_inputs import input_lua, play_script  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--out", required=True)
    ap.add_argument("--play-seed", type=int, default=1)
    ap.add_argument("--no-play", action="store_true", help="attract mode only, no inputs")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    wd = out / ".work"
    shutil.rmtree(wd, ignore_errors=True)
    wd.mkdir()
    frames = args.seconds * 59 + 60
    events = [] if args.no_play else play_script(frames, 10**9, args.play_seed)[0]
    lua = f"""
local cpu = manager.machine.devices[":maincpu"]
local space = cpu.spaces["program"]
ki_dcs_out = io.open({json.dumps(str(out / 'writes.txt'))}, "w")
ki_dcs_tap = space:install_write_tap(0x10000088, 0x1000009b, "kidcs", function(offset, data, mask)
  local t = manager.machine.time
  ki_dcs_out:write(string.format("%d %08x %08x\\n", t.seconds * 1000000000 + math.floor(t.attoseconds / 1000000000), offset, data))
  ki_dcs_out:flush()
  return data
end)
{input_lua(events)}
"""
    (wd / "dcs.lua").write_text(lua)
    rompath = str(ROOT / "data/roms")
    rc, output = mame_run.run([shutil.which("mame") or "mame", "kinst", "-rompath", rompath, "-video", "none",
                               "-sound", "none", "-nothrottle", "-skip_gameinfo", "-seconds_to_run", str(args.seconds),
                               "-samplerate", "31250", "-wavwrite", str(out / "mame.wav"),
                               "-nvram_directory", str(wd / "nv"), "-diff_directory", str(wd / "df"),
                               "-cfg_directory", str(wd / "cf"), "-autoboot_script", str(wd / "dcs.lua")],
                              wd, args.timeout)
    writes = (out / "writes.txt").read_text().splitlines() if (out / "writes.txt").exists() else []
    if rc is None or not writes:
        print("capture failed", rc, output[-400:], file=sys.stderr)
        return 1
    shutil.rmtree(wd, ignore_errors=True)
    print(f"{len(writes)} sound register writes in {args.seconds} s -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
