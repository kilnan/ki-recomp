#!/usr/bin/env python3
"""Write an input script for ki_run from the same scenario MAME runs use
(tools/mame_inputs.play_script): coin, start, character select, random play.

Frames are counted from game entry (ki_run) rather than from power-on (MAME),
so the scenario is shifted by --offset frames (MAME enters the game around
frame 360-400).

usage: make_input_script.py --frames N [--seed S] [--offset F] > script.txt
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mame_inputs import play_script  # noqa: E402

BITS = {"High Attack - Quick": 0x001, "High Attack - Medium": 0x002, "High Attack - Fierce": 0x004,
        "Low Attack - Quick": 0x008, "Low Attack - Medium": 0x010, "Low Attack - Fierce": 0x020,
        "Up": 0x040, "Down": 0x080, "Left": 0x100, "Right": 0x200}


def bit_of(field: str) -> int:
    if field == "1 Player Start":
        return 0x400
    if field == "Coin 1":
        return 0x800
    return BITS[field.split(" ", 1)[1]]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--offset", type=int, default=380)
    args = ap.parse_args()
    events, _ = play_script(args.frames + args.offset, 10**9, args.seed)
    for frame, port, field, value in events:
        f = frame - args.offset
        if f > 0:
            print(f, 1 if port == ":P1" else 2, f"{bit_of(field):x}", value)
    return 0


if __name__ == "__main__":
    sys.exit(main())
