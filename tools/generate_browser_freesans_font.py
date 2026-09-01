#!/usr/bin/env python3
"""Generate BrowserFreeSans.h from the GNU FreeSans TrueType source.

Download freefont-ttf-20120503.zip from https://ftp.gnu.org/gnu/freefont/,
extract FreeSans.ttf, then run:
  python3 tools/generate_browser_freesans_font.py /path/to/FreeSans.ttf > BrowserFreeSans.h
"""

import struct
import sys

from PIL import Image, ImageDraw, ImageFont


def be32(value):
    return struct.pack(">i", value)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_browser_freesans_font.py /path/to/FreeSans.ttf")
    extras = [0x0192, 0x02C6, 0x02DC, 0x0152, 0x0153, 0x0160, 0x0161, 0x0178, 0x017D, 0x017E,
              0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2020, 0x2021,
              0x2022, 0x2026, 0x2030, 0x2039, 0x203A, 0x20AC, 0x2116, 0x2122, 0xFFFD]
    codes = sorted(set(range(0x21, 0x7F)) | set(range(0xA0, 0x100)) |
                   set(range(0x400, 0x500)) | set(extras))
    print("#pragma once\n")
    print("// Shared Unicode UI fonts. Generated from GNU FreeSans 20120503.")
    print("// with TFT_eSPI's VLW metric/bitmap layout. GNU FreeFont is licensed under")
    print("// the GNU GPL v3 with the FreeFont embedding exception; see LICENSE.FreeFont.")
    print("// Glyph coverage: Basic Latin, Latin-1/Windows-1252 additions, Cyrillic.")
    for point_size in (14, 18, 24):
        font = ImageFont.truetype(sys.argv[1], point_size)
        ascent, descent = font.getmetrics()
        metrics = []
        bitmaps = []
        for codepoint in codes:
            character = chr(codepoint)
            bbox = font.getbbox(character, anchor="ls")
            advance = round(font.getlength(character))
            if bbox is None:
                left = top = width = height = 0
                pixels = b""
            else:
                left, top, right, bottom = bbox
                width, height = max(0, right - left), max(0, bottom - top)
                glyph = Image.new("L", (max(1, width), max(1, height)), 0)
                ImageDraw.Draw(glyph).text((-left, -top), character, font=font, fill=255, anchor="ls")
                pixels = glyph.crop((0, 0, width, height)).tobytes() if width and height else b""
            metrics.append((codepoint, height, width, advance, -top, left, 0))
            bitmaps.append(pixels)
        data = bytearray(be32(len(codes)) + be32(11) + be32(point_size) + be32(0) + be32(ascent) + be32(descent))
        for metric in metrics:
            for value in metric:
                data += be32(value)
        for bitmap in bitmaps:
            data += bitmap
        name = f"FreeSans{point_size}Unicode".encode()
        data += bytes([len(name)]) + name + bytes([len(name)]) + name + b"\x01"
        print(f"const uint8_t BrowserFreeSans{point_size}Unicode[] PROGMEM = {{")
        for offset in range(0, len(data), 16):
            print(", ".join(f"0x{value:02X}" for value in data[offset:offset + 16]) + ",")
        print("};\n")


if __name__ == "__main__":
    main()
