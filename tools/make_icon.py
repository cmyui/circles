#!/usr/bin/env python3
"""Draws circles' app icon and writes it to macos/circles.icns.

Three hit circles grow along a diagonal, blue, purple and pink, the largest
on top: a stream of notes in combo colours. Each is a white ring around a
darker rim, a glossy gradient and a white centre dot, glowing in its colour.
They sit on near-black purple inside a rounded square shaped as macOS's app
icons are (a superellipse, 824 of 1024 units across), so macOS shows it as
drawn. There's no text: at 16 px it's a pink circle with a coloured trail.
Each size is drawn on its own, four times over, and scaled down.

Needs Pillow and numpy; run from the repository's root:
    python3 tools/make_icon.py
"""

import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

SUPERSAMPLE = 4
BACK_TOP, BACK_BOTTOM = (0x22, 0x12, 0x38), (0x0B, 0x06, 0x14)
WHITE = (250, 244, 252)
# Each circle, drawn in order: its centre and radius in units of the canvas
# (0 to 1, down from the top left), and its colours: light at its top left,
# dark at its bottom right, and its rim.
CIRCLES = [
    ((0.215, 0.735), 0.090, (0x5C, 0xD6, 0xFF), (0x16, 0x6C, 0xF5),
     (0x0B, 0x4F, 0xC0)),
    ((0.355, 0.605), 0.150, (0xB8, 0x66, 0xFF), (0x74, 0x1A, 0xE0),
     (0x55, 0x10, 0xA8)),
    ((0.600, 0.430), 0.245, (0xFF, 0x6A, 0xB8), (0xD6, 0x00, 0x8E),
     (0xA8, 0x00, 0x6C)),
]


def grid(size: int):
    """Each pixel's centre, x and y, in units of 0 to 1."""
    u = (np.arange(size) + 0.5) / size
    return np.meshgrid(u, u)


def mask(size: int, inside) -> Image.Image:
    """A mask of the pixels whose centres are inside."""
    return Image.fromarray((inside(*grid(size)) * 255).astype(np.uint8))


def gradient(size: int, start, end, t) -> Image.Image:
    """From `start` where t(x, y) is 0 to `end` where it's 1."""
    w = np.clip(t(*grid(size)), 0, 1)[..., None]
    return Image.fromarray(((1 - w) * np.array(start) +
                            w * np.array(end)).astype(np.uint8))


def disc(size: int, cx: float, cy: float, r: float) -> Image.Image:
    return mask(size, lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r)


def blurred(size: int, color, shape: Image.Image, blur: float) -> Image.Image:
    layer = Image.new("RGBA", (size, size))
    layer.paste(color, (0, 0), shape)
    return layer.filter(ImageFilter.GaussianBlur(blur))


def render(px: int) -> Image.Image:
    s = px * SUPERSAMPLE
    half = 412 / 1024
    square = mask(s, lambda x, y: (np.abs(x - 0.5) / half) ** 5
                  + (np.abs(y - 0.5) / half) ** 5 <= 1)
    art = gradient(s, BACK_TOP, BACK_BOTTOM,
                   lambda x, y: (y - (0.5 - half)) / (2 * half)).convert("RGBA")
    for (cx, cy), r, light, dark, rim in CIRCLES:
        art.alpha_composite(
            blurred(s, dark + (210,), disc(s, cx, cy, r * 1.08), s * r * 0.35))
        art.alpha_composite(blurred(
            s, (0, 0, 0, 120), disc(s, cx + r * 0.03, cy + r * 0.06, r),
            s * r * 0.08))
        art.paste(WHITE, (0, 0), disc(s, cx, cy, r))
        art.paste(rim, (0, 0), disc(s, cx, cy, r * 0.82))
        fill = r * 0.76
        art.paste(
            gradient(s, light, dark,
                     lambda x, y: ((x - cx) + (y - cy)) / (1.5 * fill) + 0.5),
            (0, 0), disc(s, cx, cy, fill))
        # The gloss: the fill's top left, outside a disc shifted down right.
        gloss = np.array(disc(s, cx, cy, fill)) & ~np.array(
            disc(s, cx + fill * 0.28, cy + fill * 0.34, fill * 1.02))
        sheen = Image.new("RGBA", (s, s))
        sheen.paste((255, 255, 255, 70), (0, 0), Image.fromarray(gloss))
        art.alpha_composite(sheen)
        art.paste(WHITE, (0, 0), disc(s, cx, cy, r * 0.25))

    # The rounded square, with a soft shadow below, as macOS's icons have.
    icon = blurred(s, (0, 0, 0, 90), square, s * 10 / 1024)
    icon = icon.transform(icon.size, Image.AFFINE,
                          (1, 0, 0, 0, 1, -round(s * 12 / 1024)))
    icon.paste(art, (0, 0), square)
    return icon.resize((px, px), Image.LANCZOS)


def main() -> None:
    out = Path("macos/circles.icns")
    with tempfile.TemporaryDirectory() as folder:
        iconset = Path(folder) / "circles.iconset"
        iconset.mkdir()
        for points in (16, 32, 128, 256, 512):
            render(points).save(iconset / f"icon_{points}x{points}.png")
            render(points * 2).save(iconset / f"icon_{points}x{points}@2x.png")
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out],
                       check=True)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
