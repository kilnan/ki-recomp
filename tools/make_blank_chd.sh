#!/usr/bin/env bash
# Create an all-zero hard disk image with the KI 1 drive geometry (Seagate
# ST9150AG, C/H/S 419/13/47, 512-byte sectors; MAME kinst.cpp header notes).
# MAME warns about the wrong checksum but boots it, which is enough to run the
# boot ROM and the game code it decompresses from U98. The game itself then
# fails its disk checks; use the real kinst.chd for anything past that point.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/data/blankchd/kinst/kinst.chd"
mkdir -p "$(dirname "$OUT")"
[ -f "$OUT" ] || chdman createhd -o "$OUT" -chs 419,13,47 -ss 512 -c none
echo "$OUT"
