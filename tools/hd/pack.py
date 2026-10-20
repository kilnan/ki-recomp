#!/usr/bin/env python3
"""Build an HD pack for ki_play from upscaled canvases.

usage: pack.py EXTRACT_DIR PREPARED_DIR UPSCALED_DIR PACK_DIR [--scale 4]
- EXTRACT_DIR: ki_run --extract-sprites output (keys.txt)
- PREPARED_DIR: prepare.py output (layout.txt, masks)
- UPSCALED_DIR: c160/<image>.png ... upscaled by the scale factor
writes PACK_DIR/<image>.ki4 and PACK_DIR/index.txt
The alpha channel comes from the original mask, upscaled smoothly; the
upscaled colours are kept only where the sprite is.
"""
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageFilter


def main() -> int:
    ext, prep, up, pack = sys.argv[1:5]
    scale = int(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 4
    os.makedirs(pack, exist_ok=True)
    layout = {}
    for line in open(f"{prep}/layout.txt"):
        name, bucket, x, y, w, h = line.split()
        layout[name] = (int(bucket), int(x), int(y), int(w), int(h))
    written = 0
    for name, (bucket, x, y, w, h) in layout.items():
        src = f"{up}/c{bucket}/{name}.png"
        if not os.path.exists(src):
            continue
        big = Image.open(src).convert("RGB")
        if big.size != (bucket * scale, bucket * scale):
            big = big.resize((bucket * scale, bucket * scale), Image.LANCZOS)
        rgb = np.asarray(big.crop((x * scale, y * scale, (x + w) * scale, (y + h) * scale)))
        mask = Image.open(f"{prep}/c{bucket}/{name}.mask.png").convert("L")
        m = np.asarray(mask.resize((w * scale, h * scale), Image.BICUBIC).filter(ImageFilter.GaussianBlur(0.6)),
                       np.float32) / 255
        alpha = np.clip((m - 0.35) / 0.3, 0, 1)  # smooth edge around the 50% contour
        argb = (np.round(alpha * 255).astype(np.uint32) << 24) | (rgb[..., 0].astype(np.uint32) << 16) | \
               (rgb[..., 1].astype(np.uint32) << 8) | rgb[..., 2].astype(np.uint32)
        data = zlib.compress(argb.astype("<u4").tobytes(), 6)
        with open(f"{pack}/{name}.ki4", "wb") as f:
            f.write(b"KIHD" + struct.pack("<III", w * scale, h * scale, len(data)) + data)
        written += 1
    keys = [l.split() for l in open(f"{ext}/keys.txt")]
    with open(f"{pack}/index.txt", "w") as f:
        for key, image, ox, oy, w, h in keys:
            if os.path.exists(f"{pack}/{image}.ki4"):
                f.write(f"{key} {image} {ox} {oy} {w} {h}\n")
    print(f"{written} images, {sum(1 for _ in open(f'{pack}/index.txt'))} keys in {pack}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
