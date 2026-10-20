#!/usr/bin/env python3
"""HD panoramas for the stages the game draws by copying a wide picture
(0x88018c98; see docs/TECHNICAL_NOTE.md "Stages").

The layers of a stage are stacked in its stage file, rows of 4 margin pixels
plus WIDTH columns that wrap around. `ki_run --hd-pack DIR` with KI_HD_DEBUG=1
prints each copy's source address; minus the stage's load address it gives
the layer's offset in the file.

  panorama.py extract DISK_IMG LBA OFFSET ROWS OUT.png [--width 960] [--pad 32]
      writes the layers (OFFSET ROWS, all layers stacked) as one picture, with
      PAD columns wrapped onto each side so the upscaler sees no seam
  panorama.py pack DISK_IMG LBA UPSCALED.png PACK_DIR OFFSET:ROWS [OFFSET:ROWS ...] [--width 960] [--pad 32]
      cuts the 4x upscaled picture back into layers (in the same order and
      stacking) and writes pano_<key>.ki4 / .raw and panoramas.txt lines
"""
import struct
import sys
import zlib

import numpy as np
from PIL import Image


def opt(name: str, default: int) -> int:
    return int(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def read_rows(disk: str, lba: int, offset: int, rows: int, width: int) -> bytes:
    with open(disk, "rb") as f:
        f.seek(lba * 512 + offset)
        return f.read((width + 4) * rows * 2)


def to_rgb(raw: bytes, rows: int, width: int) -> np.ndarray:
    h = np.frombuffer(raw, "<u2").reshape(rows, width + 4)[:, 4:]
    e = lambda v: ((v << 3) | (v >> 2)).astype(np.uint8)  # noqa: E731
    return np.stack([e(h & 31), e((h >> 5) & 31), e((h >> 10) & 31)], -1)


def fnv(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main() -> int:
    width, pad, scale = opt("--width", 960), opt("--pad", 32), 4
    if sys.argv[1] == "extract":
        disk, lba, offset, rows, out = sys.argv[2], int(sys.argv[3]), int(sys.argv[4], 0), int(sys.argv[5]), sys.argv[6]
        rgb = to_rgb(read_rows(disk, lba, offset, rows, width), rows, width)
        Image.fromarray(np.concatenate([rgb[:, -pad:], rgb, rgb[:, :pad]], 1)).save(out)
        return 0
    if sys.argv[1] == "pack":
        disk, lba, upscaled, pack = sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
        layers = [a for a in sys.argv[6:] if ":" in a]
        hd = np.asarray(Image.open(upscaled).convert("RGB"))[:, pad * scale:(pad + width) * scale]
        row = 0
        with open(f"{pack}/panoramas.txt", "a") as index:
            for layer in layers:
                offset, rows = (int(x, 0) for x in layer.split(":"))
                raw = read_rows(disk, lba, offset, rows, width)
                key = f"{fnv(raw[:(width + 4) * 2]):016x}"  # the player's key: the first row
                with open(f"{pack}/pano_{key}.raw", "wb") as f:
                    f.write(raw)
                part = hd[row * scale:(row + rows) * scale]
                argb = (np.uint32(255) << 24) | (part[..., 0].astype(np.uint32) << 16) | \
                       (part[..., 1].astype(np.uint32) << 8) | part[..., 2].astype(np.uint32)
                data = zlib.compress(argb.astype("<u4").tobytes(), 6)
                with open(f"{pack}/pano_{key}.ki4", "wb") as f:
                    f.write(b"KIHD" + struct.pack("<III", width * scale, rows * scale, len(data)) + data)
                index.write(f"{key} {width} {width + 4} {rows}\n")
                print(f"layer {offset:#x} ({rows} rows): pano_{key}")
                row += rows
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
