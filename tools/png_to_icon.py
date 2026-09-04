#!/usr/bin/env python3
"""Convert a 16x16 black-and-white PNG to a TFT_eSPI XBM icon array.

Requires Pillow: python3 -m pip install Pillow

By default, light pixels are treated as the icon and dark pixels as the
background. Use --invert for dark icons on a light background.
"""

import argparse
import re
import sys
from typing import Any


ICON_SIZE = 16


def identifier(value: str) -> str:
    name = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if not name or name[0].isdigit():
        name = f"ic_{name}"
    return name


def make_bitmap(image: Any, threshold: int, invert: bool) -> list[int]:
    if image.size != (ICON_SIZE, ICON_SIZE):
        raise ValueError(
            f"Expected a {ICON_SIZE}x{ICON_SIZE} image, got {image.width}x{image.height}."
        )

    rgba = image.convert("RGBA")
    data = []
    for y in range(ICON_SIZE):
        for byte_x in range(0, ICON_SIZE, 8):
            value = 0
            for bit in range(8):
                red, green, blue, alpha = rgba.getpixel((byte_x + bit, y))
                luminance = (299 * red + 587 * green + 114 * blue) // 1000
                is_foreground = alpha != 0 and luminance >= threshold
                if invert:
                    is_foreground = alpha != 0 and luminance < threshold
                if is_foreground:
                    # XBM stores pixels least-significant bit first within each byte.
                    value |= 1 << bit
            data.append(value)
    return data


def format_array(name: str, data: list[int]) -> str:
    rows = [", ".join(f"0x{value:02X}" for value in data[index:index + 16])
            for index in range(0, len(data), 16)]
    body = ",\n    ".join(rows)
    return f"const uint8_t {name}[{len(data)}] PROGMEM = {{\n    {body}\n}};"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="16x16 PNG input")
    parser.add_argument("--name", default="ic_file", help="C array name (default: ic_file)")
    parser.add_argument("--threshold", type=int, default=128,
                        help="light pixels at or above this value are foreground (0-256, default: 128)")
    parser.add_argument("--invert", action="store_true", help="treat dark pixels as foreground")
    args = parser.parse_args()

    if not 0 <= args.threshold <= 256:
        parser.error("--threshold must be between 0 and 256")

    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow is required. Install it with: python3 -m pip install Pillow")

    try:
        with Image.open(args.input) as image:
            data = make_bitmap(image, args.threshold, args.invert)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print(format_array(identifier(args.name), data))


if __name__ == "__main__":
    main()
