#!/usr/bin/env python3
"""Draws the app icon (original artwork: polygons only, no game graphics or fonts)
and writes AppIcon.icns with iconutil. usage: make_icon.py"""
import math, os, shutil, subprocess, tempfile
from PIL import Image, ImageDraw, ImageFilter

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
# rounded square with a dark radial gradient
bg = Image.new("RGBA", (S, S))
px = bg.load()
for y in range(S):
    for x in range(S):
        d = math.hypot(x - S * 0.5, y - S * 0.42) / (S * 0.75)
        t = max(0.0, min(1.0, d))
        px[x, y] = (int(40 * (1 - t) + 8 * t), int(30 * (1 - t) + 6 * t), int(70 * (1 - t) + 18 * t), 255)
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((40, 40, S - 40, S - 40), radius=200, fill=255)
img.paste(bg, (0, 0), mask)

def letters(draw, dx, dy, fill):
    # K
    draw.polygon([(250 + dx, 260 + dy), (360 + dx, 260 + dy), (360 + dx, 764 + dy), (250 + dx, 764 + dy)], fill=fill)
    draw.polygon([(360 + dx, 520 + dy), (560 + dx, 260 + dy), (690 + dx, 260 + dy), (440 + dx, 560 + dy)], fill=fill)
    draw.polygon([(360 + dx, 600 + dy), (360 + dx, 520 + dy), (455 + dx, 405 + dy), (700 + dx, 764 + dy), (570 + dx, 764 + dy)], fill=fill)
    # I
    draw.polygon([(720 + dx, 260 + dy), (830 + dx, 260 + dy), (830 + dx, 764 + dy), (720 + dx, 764 + dy)], fill=fill)

glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
letters(ImageDraw.Draw(glow), 0, 0, (255, 120, 40, 255))
glow = glow.filter(ImageFilter.GaussianBlur(28))
img.alpha_composite(glow)
shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
letters(ImageDraw.Draw(shadow), 10, 14, (0, 0, 0, 200))
img.alpha_composite(shadow)
metal = Image.new("RGBA", (S, S), (0, 0, 0, 0))
letters(ImageDraw.Draw(metal), 0, 0, (255, 255, 255, 255))
grad = Image.new("RGBA", (S, S))
gp = grad.load()
for y in range(S):
    t = y / S
    c = (int(235 - 120 * t), int(240 - 110 * t), int(255 - 60 * t), 255)
    for x in range(S):
        gp[x, y] = c
img.paste(grad, (0, 0), metal)
# a thin slash across, the "recompiled" mark
ImageDraw.Draw(img).polygon([(180, 820), (860, 690), (870, 720), (190, 852)], fill=(255, 140, 50, 255))

here = os.path.dirname(os.path.abspath(__file__))
with tempfile.TemporaryDirectory() as td:
    iconset = os.path.join(td, "AppIcon.iconset")
    os.mkdir(iconset)
    for size in (16, 32, 128, 256, 512):
        img.resize((size, size), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}.png"))
        img.resize((size * 2, size * 2), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}@2x.png"))
    subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(here, "AppIcon.icns")], check=True)

print("wrote AppIcon.icns")
