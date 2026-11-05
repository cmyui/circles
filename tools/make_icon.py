#!/usr/bin/env python3
"""Draws circles' app icon and writes it to macos/circles.icns.

The icon is the game's hit circle: a white ring around the main menu logo's
pink gradient, on a dark rounded square shaped as macOS's app icons are (a
superellipse, 824 of 1024 units across). It has no text, so it reads at
16 px. Each size is drawn on its own, four times over, and scaled down.

Needs Pillow and numpy; run from the repository's root:
    python3 tools/make_icon.py
"""

import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

SUPERSAMPLE = 4
# The menu logo's gradient, top to bottom.
PINK_TOP, PINK_BOTTOM = (0xFF, 0x77, 0xB4), (0xB2, 0x3A, 0x8E)
# The settings panel's navy, lightening toward the top.
NAVY_TOP, NAVY_BOTTOM = (0x2C, 0x26, 0x56), (0x12, 0x12, 0x2A)


def vertical_gradient(size: int, top, bottom, y0=0.0, y1=1.0) -> Image.Image:
    """From `top` at y0 to `bottom` at y1, in units of 0 to 1 down."""
    y = (np.arange(size) + 0.5) / size
    t = np.clip((y - y0) / (y1 - y0), 0, 1)[:, None, None]
    rows = (1 - t) * np.array(top) + t * np.array(bottom)
    return Image.fromarray(np.broadcast_to(rows, (size, size, 3)).astype(np.uint8))


def mask(size: int, inside) -> Image.Image:
    """A mask of the pixels whose centres, in units of 0 to 1, are inside."""
    u = (np.arange(size) + 0.5) / size
    x, y = np.meshgrid(u, u)
    return Image.fromarray((inside(x, y) * 255).astype(np.uint8))


def render(px: int) -> Image.Image:
    s = px * SUPERSAMPLE
    icon = Image.new("RGBA", (s, s))

    # The rounded square, as macOS's icon grid places it: 824/1024 across,
    # centred, with a soft shadow below.
    half = 412 / 1024
    square = mask(s, lambda x, y: (np.abs(x - 0.5) / half) ** 5
                  + (np.abs(y - 0.5) / half) ** 5 <= 1)
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 90), (0, round(s * 12 / 1024)), square)
    icon.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(s * 10 / 1024)))
    icon.paste(vertical_gradient(s, NAVY_TOP, NAVY_BOTTOM, 0.5 - half, 0.5 + half),
               (0, 0), square)

    # The hit circle: a white ring, 12% of its radius, around the gradient,
    # over a soft shadow.
    radius = 0.29

    def disc(r, dy=0.0):
        return mask(s, lambda x, y: (x - 0.5) ** 2 + (y - 0.5 - dy) ** 2 <= r * r)

    circle_shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    circle_shadow.paste((0, 0, 0, 110), (0, 0), disc(radius, 0.02))
    icon.alpha_composite(
        circle_shadow.filter(ImageFilter.GaussianBlur(s * 0.025)))
    icon.paste((255, 255, 255, 255), (0, 0), disc(radius))
    inner = radius * 0.88
    icon.paste(vertical_gradient(s, PINK_TOP, PINK_BOTTOM, 0.5 - inner, 0.5 + inner),
               (0, 0), disc(inner))
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
