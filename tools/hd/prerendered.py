#!/usr/bin/env python3
"""HD frames for the pre-rendered stages (renderer 0x88030b9c; see
docs/TECHNICAL_NOTE.md "Stages").

A pre-rendered stage file starts with the offset of its frames (u32); the
frames are 320x240, 16-bit, one after another (8 per stage, 4 for stage 13).

  prerendered.py extract DISK_IMG LBA OUT_DIR NAME [--frames 8]   writes NAME_f<k>.png
  prerendered.py pack DISK_IMG LBA UPSCALED_DIR NAME PACK_DIR [--frames 8]
      UPSCALED_DIR/NAME_f<k>.png (4x) -> pre_<key>_<k>.ki4 + a prerendered.txt line
"""
import struct
import sys
import zlib

import numpy as np
from PIL import Image

FRAME = 320 * 240 * 2


def frames_start(f, lba: int) -> int:
    f.seek(lba * 512)
    return lba * 512 + struct.unpack("<I", f.read(4))[0]


def main() -> int:
    count = int(sys.argv[sys.argv.index("--frames") + 1]) if "--frames" in sys.argv else 8
    with open(sys.argv[2], "rb") as f:
        start = frames_start(f, int(sys.argv[3]))
        if sys.argv[1] == "extract":
            out, name = sys.argv[4], sys.argv[5]
            for k in range(count):
                f.seek(start + k * FRAME)
                h = np.frombuffer(f.read(FRAME), "<u2").reshape(240, 320)
                e = lambda v: ((v << 3) | (v >> 2)).astype(np.uint8)  # noqa: E731
                Image.fromarray(np.stack([e(h & 31), e((h >> 5) & 31), e((h >> 10) & 31)], -1)).save(f"{out}/{name}_f{k}.png")
            return 0
        if sys.argv[1] == "pack":
            up, name, pack = sys.argv[4], sys.argv[5], sys.argv[6]
            f.seek(start)
            head = f.read(320 * 2 * 8)  # the player's key: frame 0's first 8 rows
            h = 1469598103934665603
            for b in head:
                h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            key = f"{h:016x}"
            for k in range(count):
                rgb = np.asarray(Image.open(f"{up}/{name}_f{k}.png").convert("RGB").resize((1280, 960), Image.LANCZOS))
                argb = (np.uint32(255) << 24) | (rgb[..., 0].astype(np.uint32) << 16) | \
                       (rgb[..., 1].astype(np.uint32) << 8) | rgb[..., 2].astype(np.uint32)
                data = zlib.compress(argb.astype("<u4").tobytes(), 6)
                with open(f"{pack}/pre_{key}_{k}.ki4", "wb") as o:
                    o.write(b"KIHD" + struct.pack("<III", 1280, 960, len(data)) + data)
            with open(f"{pack}/prerendered.txt", "a") as o:
                o.write(f"{key} {count}\n")
            print(f"{name}: pre_{key} ({count} frames)")
            return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
