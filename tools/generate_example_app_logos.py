#!/usr/bin/env python3
"""Generate the 64x64 transparent logos shipped with the example Lua apps.

Pass app names (for example `dvach`) to regenerate only those logos.
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
SIZE = 64


def canvas():
    return Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))


def save(image, relative_path):
    image.resize((SIZE, SIZE), Image.Resampling.LANCZOS).save(ROOT / relative_path)


def counter_logo():
    image = canvas()
    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    draw.rounded_rectangle((48, 48, 208, 208), radius=32, outline=white, width=18)
    draw.rounded_rectangle((119, 76, 137, 180), radius=9, fill=white)
    draw.rounded_rectangle((76, 119, 180, 137), radius=9, fill=white)
    save(image, "examples/apps/counter/logo.png")


def workout_logo():
    image = canvas()
    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    draw.rounded_rectangle((51, 108, 205, 148), radius=18, fill=white)
    for x0, x1 in ((37, 67), (189, 219)):
        draw.rounded_rectangle((x0, 75, x1, 181), radius=14, fill=white)
    for x0, x1 in ((22, 43), (213, 234)):
        draw.rounded_rectangle((x0, 91, x1, 165), radius=10, fill=white)
    save(image, "examples/apps/workout/logo.png")


def recorder_logo():
    image = canvas()
    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    draw.rounded_rectangle((98, 36, 158, 142), radius=30, fill=white)
    draw.arc((68, 80, 188, 180), start=0, end=180, fill=white, width=14)
    draw.rounded_rectangle((121, 170, 135, 208), radius=7, fill=white)
    draw.rounded_rectangle((88, 202, 168, 218), radius=8, fill=white)
    save(image, "examples/apps/recorder/logo.png")


def files_logo():
    image = canvas()
    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    draw.rounded_rectangle((40, 64, 132, 110), radius=16, fill=white)
    draw.rounded_rectangle((40, 86, 216, 198), radius=18, fill=white)
    save(image, "examples/apps/files/logo.png")


def images_logo():
    image = canvas()
    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    draw.rounded_rectangle((36, 52, 220, 204), radius=22, outline=white, width=16)
    draw.ellipse((70, 82, 106, 118), fill=white)
    draw.polygon(((58, 184), (112, 126), (142, 156), (166, 132), (198, 184)), fill=white)
    save(image, "examples/apps/images/logo.png")


def dvach_logo():
    image = canvas()
    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    clear = (0, 0, 0, 0)
    # A speech bubble with three lines of text cut out of it.
    draw.rounded_rectangle((30, 46, 226, 176), radius=30, fill=white)
    draw.polygon(((70, 170), (62, 216), (112, 172)), fill=white)
    for y, right in ((78, 194), (106, 194), (134, 150)):
        draw.rounded_rectangle((62, y, right, y + 14), radius=7, fill=clear)
    save(image, "examples/apps/dvach/logo.png")


def launcher_logo():
    image = canvas()
    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    # A 2x2 grid of tiles with an arrow leaving the top-right one.
    for x, y in ((40, 40), (40, 136), (136, 136)):
        draw.rounded_rectangle((x, y, x + 80, y + 80), radius=18, fill=white)
    draw.rounded_rectangle((136, 40, 216, 120), radius=18, outline=white, width=14)
    draw.line((160, 96, 214, 42), fill=white, width=16)
    draw.polygon(((228, 28), (228, 92), (164, 28)), fill=white)
    save(image, "examples/apps/launcher/logo.png")


LOGOS = {
    "counter": counter_logo,
    "workout": workout_logo,
    "recorder": recorder_logo,
    "files": files_logo,
    "images": images_logo,
    "dvach": dvach_logo,
    "launcher": launcher_logo,
}


if __name__ == "__main__":
    for name in sys.argv[1:] or LOGOS:
        LOGOS[name]()
