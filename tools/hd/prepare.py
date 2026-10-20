#!/usr/bin/env python3
"""Prepare extracted sprites (ki_run --extract-sprites) for a 4x upscaler.

Each sprite's colours are spread into its transparent area (so the upscaler
sees no dark fringe), and it is placed on a square canvas of a fixed size per
bucket, so a whole bucket goes through the upscaler in one run at exactly 4x
(SeedVR2's --resolution is the short side).

usage: prepare.py EXTRACT_DIR OUT_DIR
writes OUT_DIR/c160/*.png, OUT_DIR/c320/*.png and OUT_DIR/layout.txt
("image bucket x y w h" per line)
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

BUCKETS = (160, 320)
MARGIN = 8


def spread(rgb: np.ndarray, opaque: np.ndarray, passes: int = 12) -> np.ndarray:
    """Fill transparent pixels with the average of opaque neighbours, growing outwards."""
    rgb = rgb.astype(np.float32).copy()
    known = opaque.copy()
    for _ in range(passes):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                k = np.roll(np.roll(known, dy, 0), dx, 1)
                v = np.roll(np.roll(rgb, dy, 0), dx, 1)
                acc += v * k[..., None]
                cnt += k
        grow = (~known) & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known |= grow
    return rgb.clip(0, 255).astype(np.uint8)


def main() -> int:
    src, out = sys.argv[1], sys.argv[2]
    for b in BUCKETS:
        os.makedirs(f"{out}/c{b}", exist_ok=True)
    layout = []
    for path in sorted(glob.glob(f"{src}/*.ppm")):
        name = os.path.splitext(os.path.basename(path))[0]
        if name.startswith("frame_") or name.startswith("hd_frame_"):
            continue
        im = np.asarray(Image.open(path).convert("RGB"))
        opaque = ~((im[..., 0] == 0) & (im[..., 1] == 255) & (im[..., 2] == 0))
        h, w = opaque.shape
        bucket = next((b for b in BUCKETS if max(w, h) + 2 * MARGIN <= b), None)
        if bucket is None:
            print(f"skip {name}: {w}x{h} too large", file=sys.stderr)
            continue
        canvas = np.zeros((bucket, bucket, 3), np.uint8)
        cm = np.zeros((bucket, bucket), bool)
        canvas[MARGIN:MARGIN + h, MARGIN:MARGIN + w] = im
        cm[MARGIN:MARGIN + h, MARGIN:MARGIN + w] = opaque
        Image.fromarray(spread(canvas, cm)).save(f"{out}/c{bucket}/{name}.png")
        Image.fromarray((opaque * 255).astype(np.uint8)).save(f"{out}/c{bucket}/{name}.mask.png")
        layout.append(f"{name} {bucket} {MARGIN} {MARGIN} {w} {h}")
    with open(f"{out}/layout.txt", "w") as f:
        f.write("\n".join(layout) + "\n")
    print(f"{len(layout)} sprites prepared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
