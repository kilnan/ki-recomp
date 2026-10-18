#!/usr/bin/env bash
# Capture the PoC cases listed in poc/cases.txt from MAME into
# traces/poc/<name>/ (parallel). ONLY=<regex> restricts the case names,
# OUT_DIR=<dir> writes elsewhere (e.g. to check that captures reproduce).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PY:-$ROOT/.venv/bin/python}"
# Each MAME instance takes a full core and ~250 MB; keep the default modest.
JOBS="${JOBS:-3}"
export INPUTS="$ROOT/traces/inputs"
OUT_DIR="${OUT_DIR:-$ROOT/traces/poc}"
mkdir -p "$INPUTS" "$OUT_DIR"
"$PY" -I - "$INPUTS" <<'PYEOF'
import random, sys, pathlib
d = pathlib.Path(sys.argv[1])
rng = random.Random(0x4B49)  # fixed seed: reproducible synthetic inputs
(d / "random4093.bin").write_bytes(bytes(rng.randrange(256) for _ in range(4093)))
(d / "ones777.bin").write_bytes(b"\xff" * 777)
import struct
# 0x880176f0 reads floats at +0 (x1), +8 (y1), +0xc (y2), +0x10 (x2) from 0x88035e30.
def vec(x1, y1, y2, x2): return struct.pack("<5f", x1, 0.0, y1, y2, x2)
(d / "fpu_zero.bin").write_bytes(vec(3.0, 4.0, 0.0, 0.0))          # |b| = 0: c.eq.s path
(d / "fpu_negcross.bin").write_bytes(vec(1.0, 0.0, -2.0, 0.5))     # negative cross product: neg.s path
(d / "fpu_random1.bin").write_bytes(vec(*(rng.uniform(-300, 300) for _ in range(4))))
(d / "fpu_random2.bin").write_bytes(vec(*(rng.uniform(-1e-3, 1e-3) for _ in range(4))))
PYEOF
status=0
# Kill the whole batch (and, through mame_run.py, every MAME child) on Ctrl-C/TERM.
trap 'kill $(jobs -p) 2>/dev/null; wait; exit 130' INT TERM
while read -r name func hit rest; do
  case "$name" in ''|\#*) continue ;; esac
  if [ -n "${ONLY:-}" ] && ! echo "$name" | grep -qE "$ONLY"; then continue; fi
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 1; done
  # shellcheck disable=SC2086  # options are intentionally word-split
  ( eval "set -- $rest"
    mode="--seconds 60"
    # The interpreter boots much more slowly in emulated time (EPROM wait
    # states apply to every instruction fetch), hence the larger limit.
    case "$name" in syn-fpu-*) ;; syn-*|*-interp) mode="--interp --seconds ${INTERP_SECONDS:-900}" ;; esac
    # shellcheck disable=SC2086
    "$PY" "$ROOT/tools/mame_capture.py" --func "$func" $mode capture \
        --hit "$hit" --out "$OUT_DIR/$name" "$@" ) &
done < "$ROOT/poc/cases.txt"
for p in $(jobs -p); do wait "$p" || status=1; done
exit $status
