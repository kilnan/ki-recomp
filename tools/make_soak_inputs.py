#!/usr/bin/env python3
"""Write a long randomized input script for ki_run (robustness runs).

Every three minutes both players insert two coins, player 1 starts and, half
the time, player 2 joins; in between both press random directions, attacks and
start. Frames count from power-on (`ki_run --boot rom`).

usage: make_soak_inputs.py SEED FRAMES > script.txt
then:  build/ki_run --boot rom --disk data/kinst.img --rom data/roms/kinst/ki-l15d.u98 \\
           --sound-roms data/roms/kinst --frames FRAMES --inputs script.txt --out DIR
"""
import random
import sys

COIN, START = 0x800, 0x400
MOVES = [0x001, 0x002, 0x004, 0x008, 0x010, 0x020, 0x040, 0x080, 0x100, 0x200]


def main() -> int:
    seed, frames = int(sys.argv[1]), int(sys.argv[2])
    rng = random.Random(seed)
    ev = []

    def press(f, port, bit, length=6):
        ev.append((f, port, bit, 1))
        ev.append((f + length, port, bit, 0))

    f = 900
    while f < frames - 600:
        press(f, 1, COIN)
        press(f + 30, 1, COIN)
        press(f + 60, 2, COIN)
        press(f + 90, 2, COIN)
        press(f + 150, 1, START)
        if rng.random() < 0.5:
            press(f + 200, 2, START)
        end = f + 10800
        g = f + 300
        while g < end and g < frames - 60:
            press(g, rng.choice((1, 2)), rng.choice(MOVES + [START]), rng.randint(3, 20))
            g += rng.randint(2, 12)
        f = end
    for frame, port, bit, value in sorted(ev):
        print(frame, port, format(bit, "x"), value)
    return 0


if __name__ == "__main__":
    sys.exit(main())
