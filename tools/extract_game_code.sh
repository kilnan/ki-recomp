#!/usr/bin/env bash
# Unpack the game code that the KI 1 boot ROM (U98 ki-l15d.u98) decompresses
# into DRAM, using kipack from nekuz0r/ki-rom (AGPL-3.0, built and run as an
# external tool; none of its code is copied into this repository).
#
# Output (ignored by Git): data/kipack-out/rom-{0,1,2}.{bin,addr}
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KI_ROM_COMMIT=8e4344cfd2b039e2ca97c9f115bb8f1acaff804b
SRC="$ROOT/third_party_src/ki-rom"
ROM="${1:-$ROOT/data/roms/kinst/ki-l15d.u98}"
OUT="$ROOT/data/kipack-out"

if [ ! -d "$SRC/.git" ]; then
  git clone -q https://github.com/nekuz0r/ki-rom.git "$SRC"
fi
git -C "$SRC" fetch -q --depth 1 origin "$KI_ROM_COMMIT" 2>/dev/null || true
git -C "$SRC" checkout -q "$KI_ROM_COMMIT"
make -s -C "$SRC/tools/kipack" GCC="${CC:-cc}"

mkdir -p "$OUT"
"$SRC/tools/kipack/kipack" unpack "$ROM" "$OUT"
python3 "$ROOT/tools/check_data.py"
