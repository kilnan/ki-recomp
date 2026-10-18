#!/usr/bin/env python3
"""Verify the locally supplied Killer Instinct data against MAME's definitions.

Expected hashes come from MAME src/mame/rare/kinst.cpp, ROM_START(kinst)
(revision ee0b4a91e670a47b55c0d17d2fd8c7522ab6f03c, identical in MAME 0.289
-listxml). Nothing here downloads data: it only checks files you provide.

usage: check_data.py [--roms data/roms/kinst] [--kipack data/kipack-out]
"""
import argparse
import hashlib
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# name: (size, crc32, sha1, role)
KINST = {
    "ki-l15d.u98": (0x80000, "7b65ca3d", "607394d4ba1713f38c2cb5159303cace9cde991e",
                    "U98 boot EPROM v1.5d (default BIOS): boot code + compressed game code"),
    "u10-l1": (0x80000, "b6cc155f", "810d455df8f385d76143e9d7d048f2b555ff8bf0", "DCS sound data"),
    "u11-l1": (0x80000, "0b5e05df", "0595909cb667c38ac7c8c7bd0646b28899e27777", "DCS sound data"),
    "u12-l1": (0x80000, "d05ce6ad", "7a8ee405c118fd176b66353fa7bfab888cc63cd2", "DCS sound data"),
    "u13-l1": (0x80000, "7d0954ea", "ea4d1f153eb284f1bcfc5295fbce316bba6083f4", "DCS sound data"),
    "u33-l1": (0x80000, "8bbe4f0c", "b22e365bc8d58a80eaac226be14b4bb8d9a04844", "DCS sound data"),
    "u34-l1": (0x80000, "b2e73603", "ee439f5162a2b3379d3f802328017bb3c68547d2", "DCS sound data"),
    "u35-l1": (0x80000, "0aaef4fc", "48c4c954ac9db648f28ad64f9845e19ec432eec3", "DCS sound data"),
    "u36-l1": (0x80000, "0577bb60", "cc78070cc41701e9a91fde5cfbdc7e1e83354854", "DCS sound data"),
}
# CHD: MAME lists only the SHA1 of the CHD's *content* (not of the .chd file),
# which `chdman info -i kinst.chd` prints as "SHA1".
KINST_CHD_SHA1 = "81d833236e994528d1482979261401b198d1ca53"

# Images unpacked from ki-l15d.u98 by kipack (tools/extract_game_code.sh).
# rom-0 was also checked word for word against DRAM after MAME's original boot
# ROM decompressed it (docs/TECHNICAL_NOTE.md).
KIPACK = {
    "rom-0.bin": (0x32CB0, "ed875b9f2a0d1bccf79ff63189c37488ee718659", "game code, loaded at 0x88000000"),
    "rom-1.bin": (0x51100, None, "data, loaded at 0x88033900"),
    "rom-2.bin": (0x80, None, "data, loaded at 0x88033880"),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=str(ROOT / "data/roms/kinst"))
    ap.add_argument("--kipack", default=str(ROOT / "data/kipack-out"))
    args = ap.parse_args()
    ok = True
    roms = Path(args.roms)
    print(f"ROM set kinst (BIOS v1.5d) in {roms}:")
    for name, (size, crc, sha1, role) in KINST.items():
        f = roms / name
        if not f.exists():
            print(f"  MISSING  {name:12s} {role}")
            ok = False
            continue
        data = f.read_bytes()
        got_crc = f"{zlib.crc32(data):08x}"
        got_sha = hashlib.sha1(data).hexdigest()
        good = len(data) == size and got_crc == crc and got_sha == sha1
        ok &= good
        print(f"  {'OK      ' if good else 'BAD     '} {name:12s} crc {got_crc} sha1 {got_sha}")
    chd = roms / "kinst.chd"
    if chd.exists():
        print(f"  PRESENT  kinst.chd  (verify: chdman info -i {chd} | grep SHA1 -> {KINST_CHD_SHA1})")
    else:
        print(f"  MISSING  kinst.chd  hard disk image, content SHA1 {KINST_CHD_SHA1} "
              "(optional for the current PoC)")
    kp = Path(args.kipack)
    print(f"Unpacked game images in {kp}:")
    for name, (size, sha1, role) in KIPACK.items():
        f = kp / name
        if not f.exists():
            print(f"  MISSING  {name:10s} {role} (run tools/extract_game_code.sh)")
            ok = False
            continue
        data = f.read_bytes()
        good = len(data) == size and (sha1 is None or hashlib.sha1(data).hexdigest() == sha1)
        ok &= good
        print(f"  {'OK      ' if good else 'BAD     '} {name:10s} {len(data):#x} bytes, {role}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
