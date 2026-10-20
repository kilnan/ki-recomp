#!/bin/bash
# Runs on the GPU instance: upscales atlases with SeedVR2 7B sharp, one
# folder after another as they arrive.
#   QUEUE/queue.txt          names in order, e.g. "c0 c5 misc"
#   QUEUE/<name>/a160/*.png  640x640 atlases (16 canvases of 160), READY marks a complete upload
#   QUEUE/<name>/a320/*.png  640x640 atlases (4 canvases of 320)
# Output: QUEUE/out/<name>/{a160,a320}/*.png at 2560x2560, DONE when finished.
# touch QUEUE/STOP to end after the current folder.
# Start it with `nohup bash vast_queue.sh QUEUE > QUEUE/queue.log 2>&1 < /dev/null &`
# (on Vast, after a `;`, not `&&`).
Q=${1:-/workspace/ki-sprites/atl}
source /venv/main/bin/activate
cd /workspace/seedvr2
while [ ! -f "$Q/STOP" ]; do
  next=""
  for n in $(cat "$Q/queue.txt" 2>/dev/null); do
    if [ -f "$Q/$n/READY" ] && [ ! -f "$Q/out/$n/DONE" ]; then next=$n; break; fi
  done
  if [ -z "$next" ]; then sleep 30; continue; fi
  echo "$(date +%T) start $next"
  for d in a160 a320; do
    [ -n "$(ls "$Q/$next/$d"/*.png 2>/dev/null)" ] || continue
    python inference_cli.py "$Q/$next/$d" --dit_model seedvr2_ema_7b_sharp_fp16.safetensors --model_dir /workspace/models \
      --resolution 2560 --batch_size 1 --seed 42 --output_format png --output "$Q/out/$next/$d" \
      --color_correction lab --cache_dit --cache_vae > "$Q/log_${next}_${d}.log" 2>&1
  done
  mkdir -p "$Q/out/$next"; touch "$Q/out/$next/DONE"
  echo "$(date +%T) done $next"
done
