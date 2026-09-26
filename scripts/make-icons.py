#!/usr/bin/env python3
"""Write app/icon.png (80x80) and app/largeIcon.png (130x130): a wine glass
drawn from scratch on a dark red tile. Not WineHQ's logo artwork."""
from pathlib import Path
from PIL import Image, ImageDraw

APP = Path(__file__).resolve().parents[1] / "app"
SS = 8


def draw(size):
    s = size * SS
    img = Image.new("RGBA", (s, s))
    d = ImageDraw.Draw(img)
    for y in range(s):
        t = y / s
        d.line([(0, y), (s, y)], fill=(int(120 - 60 * t), int(24 - 10 * t), int(42 - 12 * t), 255))
    u = s / 100
    glass = (236, 236, 244, 255)
    # Bowl: the lower half of an ellipse, filled with wine below the rim.
    d.chord([28 * u, 6 * u, 72 * u, 62 * u], 0, 180, fill=glass)
    d.chord([32 * u, 10 * u, 68 * u, 58 * u], 0, 180, fill=(170, 20, 50, 255))
    d.rectangle([28 * u, 30 * u, 72 * u, 34 * u], fill=glass)
    # Stem and foot.
    d.rectangle([47.5 * u, 62 * u, 52.5 * u, 82 * u], fill=glass)
    d.ellipse([32 * u, 80 * u, 68 * u, 88 * u], fill=glass)
    return img.resize((size, size), Image.LANCZOS)


draw(80).save(APP / "icon.png")
draw(130).save(APP / "largeIcon.png")
print("icons written")
