"""Generates the block textures and the screen image used by render.py."""
import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

out = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
out.mkdir(parents=True, exist_ok=True)
rng = random.Random(7)

# Flat, saturated palette in the style of the official trailers.
LIME, LIME_DARK, LIME_LIGHT = (134, 194, 64), (112, 166, 52), (154, 210, 84)
DIRT, DIRT_DARK, DIRT_SPECK = (166, 112, 74), (146, 97, 63), (200, 156, 118)


def dirt():
    img = Image.new("RGB", (16, 16), DIRT)
    for y in range(16):
        for x in range(16):
            r = rng.random()
            if r < 0.06:
                img.putpixel((x, y), DIRT_SPECK)
            elif r < 0.16:
                img.putpixel((x, y), DIRT_DARK)
    return img


def grass_top():
    img = Image.new("RGB", (16, 16), LIME)
    for y in range(16):
        for x in range(16):
            r = rng.random()
            if r < 0.08:
                img.putpixel((x, y), LIME_LIGHT)
            elif r < 0.16:
                img.putpixel((x, y), LIME_DARK)
    return img


def grass_side():
    img = dirt()
    depths = [3, 4, 3, 5, 4, 3, 3, 4, 6, 4, 3, 4, 5, 3, 4, 3]
    for x, depth in enumerate(depths):
        for y in range(depth):
            img.putpixel((x, y), LIME)
        img.putpixel((x, depth - 1), LIME_DARK)  # shaded lip where grass meets dirt
    return img


def screen():
    w, h = 960, 720
    img = Image.new("RGB", (w, h), (10, 13, 17))
    d = ImageDraw.Draw(img)
    mono = "/usr/share/fonts/jetbrains-mono-fonts/JetBrainsMono-Regular.otf"
    bold = "/usr/share/fonts/jetbrains-mono-fonts/JetBrainsMono-Bold.otf"
    f, fb = ImageFont.truetype(mono, 38), ImageFont.truetype(bold, 38)
    amber, white, gray, green, blue = (245, 165, 36), (230, 232, 235), (125, 133, 144), (126, 231, 135), (121, 192, 255)
    lines = [
        [("$ ", amber), ("umc attach", white)],
        [("  injected ", gray), ("minecraft 26.3", green)],
        [],
        [("umc> ", amber), ("hook ", white), ("Minecraft", blue), (".tick", white)],
        [("  patched ", gray), ("net.minecraft.client", green)],
        [],
        [("umc> ", amber), ("trace ", white), ("Player", blue), (".jump", white)],
        [("  jump() ", gray), ("x=12 y=64", green)],
        [],
        [("umc> ", amber), ("█", white)],
    ]
    y = 46
    for line in lines:
        x = 52
        for text, color in line:
            font = fb if color == amber else f
            d.text((x, y), text, font=font, fill=color)
            x += d.textlength(text, font=font)
        y += 62
    return img


nearest = Image.Resampling.NEAREST
grass_top().resize((256, 256), nearest).save(out / "grass_top.png")
grass_side().resize((256, 256), nearest).save(out / "grass_side.png")
dirt().resize((256, 256), nearest).save(out / "dirt.png")
screen().save(out / "screen.png")
print("textures ->", out)
