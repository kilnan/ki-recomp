#!/usr/bin/env python3
"""Group upscaler canvases into atlases (fewer, larger upscaler runs: the
per-image overhead of SeedVR2 is ~10 s, the work per pixel is small).

  atlas.py pack CANVAS_DIR ATLAS_DIR [--grid 4]   CANVAS_DIR/*.png (same size) -> ATLAS_DIR/atlas_NNNN.png + atlas.txt
  atlas.py unpack ATLAS_DIR UPSCALED_DIR OUT_DIR [--scale 4]   upscaled atlases -> OUT_DIR/<canvas name>.png
Each canvas already has a margin (prepare.py), so neighbours do not bleed into each other.
"""
import glob
import os
import sys

from PIL import Image


def main() -> int:
    grid = int(sys.argv[sys.argv.index("--grid") + 1]) if "--grid" in sys.argv else 4
    scale = int(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 4
    if sys.argv[1] == "pack":
        src, out = sys.argv[2], sys.argv[3]
        os.makedirs(out, exist_ok=True)
        names = sorted(n for n in glob.glob(f"{src}/*.png") if not n.endswith(".mask.png"))
        if not names:
            return 0
        size = Image.open(names[0]).size[0]
        with open(f"{out}/atlas.txt", "w") as index:
            for a in range(0, len(names), grid * grid):
                atlas = Image.new("RGB", (size * grid, size * grid))
                for i, n in enumerate(names[a:a + grid * grid]):
                    atlas.paste(Image.open(n).convert("RGB"), ((i % grid) * size, (i // grid) * size))
                    index.write(f"atlas_{a // (grid * grid):04d} {i % grid} {i // grid} {size} {os.path.basename(n)}\n")
                atlas.save(f"{out}/atlas_{a // (grid * grid):04d}.png")
        return 0
    if sys.argv[1] == "unpack":
        atlases, up, out = sys.argv[2], sys.argv[3], sys.argv[4]
        os.makedirs(out, exist_ok=True)
        cache = {}
        for line in open(f"{atlases}/atlas.txt"):
            a, gx, gy, size, name = line.split()
            gx, gy, size = int(gx), int(gy), int(size) * scale
            if a not in cache:
                path = f"{up}/{a}.png"
                if not os.path.exists(path):
                    continue
                cache = {a: Image.open(path).convert("RGB")}
            cache[a].crop((gx * size, gy * size, (gx + 1) * size, (gy + 1) * size)).save(f"{out}/{name}")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
