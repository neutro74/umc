"""Finishes the renders: python3 compose.py <logo render> <banner render> <assets dir>

The logo stays transparent (with its soft ground shadow); the banner gets the green backdrop.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

logo_render = Image.open(sys.argv[1]).convert("RGBA")
banner_render = Image.open(sys.argv[2]).convert("RGBA")
out = Path(sys.argv[3])


def fade_edges(img, frac=0.12):
    """Ramps alpha to zero near the frame so the faint ambient shadow never shows a border."""
    ramp = lambda n: np.clip(np.minimum(np.arange(n), np.arange(n)[::-1]) / (frac * n), 0, 1)
    alpha = np.asarray(img.getchannel("A"), dtype=np.float32) * np.outer(ramp(img.height), ramp(img.width))
    faded = img.copy()
    faded.putalpha(Image.fromarray(alpha.astype(np.uint8)))
    return faded


# Logo: a square around the objects themselves; the feathered edge lets the shadow trail off.
x0, y0, x1, y1 = logo_render.getchannel("A").point(lambda a: 255 if a > 200 else 0).getbbox()
side = int(max(x1 - x0, y1 - y0) * 1.12)
cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
logo = fade_edges(logo_render.crop((cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)), 0.06)
logo.resize((1024, 1024), Image.Resampling.LANCZOS).save(out / "logo.png")
logo.resize((256, 256), Image.Resampling.LANCZOS).save(out / "logo-256.png")

# Banner: trailer-green vertical gradient behind the render, then down to GitHub-friendly width.
top, bottom = np.array([54, 114, 48], np.float32), np.array([31, 74, 29], np.float32)
w, h = banner_render.size
t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
gradient = np.broadcast_to(top + (bottom - top) * t, (h, w, 3)).astype(np.uint8)
banner = Image.fromarray(gradient).convert("RGBA")
banner.alpha_composite(banner_render)
banner.convert("RGB").resize((1280, h * 1280 // w), Image.Resampling.LANCZOS).save(out / "banner.png")

print("wrote logo.png, logo-256.png, banner.png to", out)
