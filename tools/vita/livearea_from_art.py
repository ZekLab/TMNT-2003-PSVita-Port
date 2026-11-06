#!/usr/bin/env python3
"""Builds the Vita bubble icon and LiveArea images in vita/sce_sys/ from the game's own art.

The art is copyrighted: neither the sources nor the PNGs are committed (.gitignore), and the VPK gets them only when they exist.
Usage: .venv/bin/python tools/vita/livearea_from_art.py <logo.png> <title_screen.png>
  logo.png:         the title logo texture on white (game/app/ref_05_tit_logo.png) -> icon0.png shaped like the logo
  title_screen.png: an 800x600 capture of the title screen without text -> bg0.png; the logo also gives frame1.png and startup.png
The Vita accepts only 8-bit palette PNGs here.
"""

import collections
import os
import sys

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "vita", "sce_sys")


def save(img, relative):
    path = os.path.normpath(os.path.join(OUT, relative))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.quantize(colors=256, method=Image.Quantize.FASTOCTREE if img.mode == "RGBA" else Image.Quantize.MEDIANCUT,
                 dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)
    print(f"wrote {path}")


def logo_cutout(path):
    """The white around the logo becomes transparent (flood fill from the edges; the logo's black outline stops it)."""
    src = Image.open(path).convert("RGB")
    w, h = src.size
    px = src.load()
    mask = Image.new("L", (w, h), 255)
    m = mask.load()
    queue = collections.deque([(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)])
    while queue:
        x, y = queue.popleft()
        if not (0 <= x < w and 0 <= y < h) or m[x, y] == 0 or min(px[x, y]) <= 200:
            continue
        m[x, y] = 0
        queue.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    logo = src.convert("RGBA")
    logo.putalpha(mask)
    return logo.crop(mask.getbbox())


logo = logo_cutout(sys.argv[1])
side = max(logo.size)
square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
square.paste(logo, ((side - logo.width) // 2, (side - logo.height) // 2), logo)
save(square.resize((128, 128), Image.LANCZOS), "icon0.png")

title = Image.open(sys.argv[2]).convert("RGB")
height = round(title.width * 500 / 840)
top = (title.height - height) // 2
city = title.crop((0, top, title.width, top + height))
save(city.resize((840, 500), Image.LANCZOS), "livearea/contents/bg0.png")

# Like the Budokai Tenkaichi 3 port: the logo itself is the button that starts the game (template.xml, style ad0, frame1).
button = logo.copy()
button.thumbnail((414, 189), Image.LANCZOS)
frame = Image.new("RGBA", (414, 189), (0, 0, 0, 0))
frame.paste(button, ((414 - button.width) // 2, (189 - button.height) // 2), button)
path = os.path.normpath(os.path.join(OUT, "livearea/contents/frame1.png"))
frame.save(path, optimize=True)
print(f"wrote {path}")

startup = logo.copy()
startup.thumbnail((280, 158), Image.LANCZOS)
gate = Image.new("RGBA", (280, 158), (0, 0, 0, 0))
gate.paste(startup, ((280 - startup.width) // 2, (158 - startup.height) // 2), startup)
save(gate, "livearea/contents/startup.png")
