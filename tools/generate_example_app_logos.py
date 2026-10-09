#!/usr/bin/env python3
"""Generate the 64x64 transparent logos shipped with the example Lua apps."""

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


if __name__ == "__main__":
    counter_logo()
    workout_logo()
