#!/usr/bin/env python3
"""Build an HD pack for ki_play from upscaled canvases.

usage: pack.py EXTRACT_DIR PREPARED_DIR UPSCALED_DIR PACK_DIR [--scale 4] [--recolor]
- EXTRACT_DIR: ki_run --extract-sprites output (keys.txt)
- PREPARED_DIR: prepare.py output (layout.txt, masks)
- UPSCALED_DIR: c160/<image>.png ... upscaled by the scale factor
writes PACK_DIR/<image>.ki4 and PACK_DIR/index.txt
The alpha channel comes from the original mask, upscaled smoothly; the
upscaled colours are kept only where the sprite is.

--recolor: a sprite drawn with another palette (same header in index.txt, as
written by ki_run --extract-character with several palettes) that has no
upscaled canvas of its own gets the upscaled image of the first one with the
difference between the two originals added, upscaled smoothly. Inside an area
of one colour that is the exact palette swap; the detail the upscaler added
is kept.
"""
import os
import shutil
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageFilter


def original(ext: str, name: str) -> np.ndarray:
    return np.asarray(Image.open(f"{ext}/{name}.ppm").convert("RGB"), np.float32)


def recolor(ext: str, layout: dict, upscaled: dict, scale: int) -> None:
    by_header = {}
    for line in open(f"{ext}/index.txt"):
        p = line.split()
        by_header.setdefault((p[2], p[4]), []).append(p[0])  # header address, size
    made = 0
    for names in by_header.values():
        base = next((n for n in names if n in upscaled), None)
        if base is None:
            continue
        a = original(ext, base)
        hd = upscaled[base].astype(np.float32)
        for n in names:
            if n in upscaled or n not in layout:
                continue
            diff = original(ext, n) - a
            h, w = diff.shape[:2]
            up = np.stack([np.asarray(Image.fromarray(diff[..., ch], "F").resize((w * scale, h * scale), Image.BICUBIC))
                           for ch in range(3)], axis=-1)
            upscaled[n] = np.clip(hd + up, 0, 255).astype(np.uint8)
            made += 1
    print(f"{made} images recoloured from another palette")


def main() -> int:
    ext, prep, up, pack = sys.argv[1:5]
    scale = int(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 4
    os.makedirs(pack, exist_ok=True)
    layout = {}
    for line in open(f"{prep}/layout.txt"):
        name, bucket, x, y, w, h = line.split()
        layout[name] = (int(bucket), int(x), int(y), int(w), int(h))
    written = 0
    upscaled = {}
    for name, (bucket, x, y, w, h) in layout.items():
        src = f"{up}/c{bucket}/{name}.png"
        if not os.path.exists(src):
            continue
        big = Image.open(src).convert("RGB")
        if big.size != (bucket * scale, bucket * scale):
            big = big.resize((bucket * scale, bucket * scale), Image.LANCZOS)
        upscaled[name] = np.asarray(big.crop((x * scale, y * scale, (x + w) * scale, (y + h) * scale)))
    real = set(upscaled)  # upscaled themselves, not recoloured
    if "--recolor" in sys.argv:
        recolor(ext, layout, upscaled, scale)
    for name, rgb in upscaled.items():
        bucket, x, y, w, h = layout[name]
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
        for key, image, ox, oy, w, h, *_ in keys:
            if os.path.exists(f"{pack}/{image}.ki4"):
                f.write(f"{key} {image} {ox} {oy} {w} {h}\n")
    # shapes.txt: for a sprite drawn with a palette the pack has no image for,
    # the player recolours the HD image of the same shape (with its .kix:
    # palette and the palette entry of each original pixel).
    shapes = {}
    for key, image, ox, oy, w, h, *rest in keys:
        if rest and image in real and os.path.exists(f"{ext}/{image}.kix") and rest[0] not in shapes:
            shapes[rest[0]] = (image, ox, oy, w, h)
            shutil.copyfile(f"{ext}/{image}.kix", f"{pack}/{image}.kix")
    with open(f"{pack}/shapes.txt", "w") as f:
        for shape, (image, ox, oy, w, h) in shapes.items():
            f.write(f"{shape} {image} {ox} {oy} {w} {h}\n")
    print(f"{written} images, {sum(1 for _ in open(f'{pack}/index.txt'))} keys, {len(shapes)} shapes in {pack}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
