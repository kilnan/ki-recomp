#!/usr/bin/env bash
# Capture the PoC cases listed in poc/cases.txt from MAME into
# traces/poc/<name>/ (parallel). ONLY=<regex> restricts the case names.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PY:-$ROOT/.venv/bin/python}"
JOBS="${JOBS:-6}"
export INPUTS="$ROOT/traces/inputs"
mkdir -p "$INPUTS" "$ROOT/traces/poc"
"$PY" -I - "$INPUTS" <<'PYEOF'
import random, sys, pathlib
d = pathlib.Path(sys.argv[1])
rng = random.Random(0x4B49)  # fixed seed: reproducible synthetic inputs
(d / "random4093.bin").write_bytes(bytes(rng.randrange(256) for _ in range(4093)))
(d / "ones777.bin").write_bytes(b"\xff" * 777)
PYEOF
status=0
while read -r name func hit rest; do
  case "$name" in ''|\#*) continue ;; esac
  if [ -n "${ONLY:-}" ] && ! echo "$name" | grep -qE "$ONLY"; then continue; fi
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 1; done
  # shellcheck disable=SC2086  # options are intentionally word-split
  ( eval "set -- $rest"
    mode="--seconds 60"
    # The interpreter boots much more slowly in emulated time (EPROM wait
    # states apply to every instruction fetch), hence the larger limit.
    case "$name" in syn-*|*-interp) mode="--interp --seconds ${INTERP_SECONDS:-900}" ;; esac
    # shellcheck disable=SC2086
    "$PY" "$ROOT/tools/mame_capture.py" --func "$func" $mode capture \
        --hit "$hit" --out "$ROOT/traces/poc/$name" "$@" ) &
done < "$ROOT/poc/cases.txt"
for p in $(jobs -p); do wait "$p" || status=1; done
exit $status
