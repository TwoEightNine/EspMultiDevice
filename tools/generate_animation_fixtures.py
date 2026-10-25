#!/usr/bin/env python3
"""Generate deterministic animated GIF/WebP fixtures for the Image Viewer.

Every test frame shows a three-digit frame number, a sweeping bottom marker,
a colour square that cycles red/green/blue, and a 32-step grey ramp, so
dropped frames, stalls, cropping, and RGB444 banding are visible by eye.

GIFs are written by the small GIF89a encoder below, so frame rectangles,
disposal, transparency, loop extensions, and truncation are exact and do not
depend on an image library. WebP needs libwebp through the pinned Pillow
release. Each fixture is decoded again with Pillow before the script exits.

Usage: tools/generate_animation_fixtures.py [output-directory]
"""

import struct
import sys
from pathlib import Path

import PIL
from PIL import Image, features

PILLOW_VERSION = "12.3.0"
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "tests" / "animation" / "fixtures"

# Palette indices. The transparent index is magenta so a decoder that ignores
# transparency shows an obvious error instead of a plausible image.
BLACK = 0
BACKGROUND = 1
WHITE = 2
RED = 3
GREEN = 4
BLUE = 5
YELLOW = 6
CHECK_LIGHT = 7
CHECK_DARK = 8
RAMP_START = 16
RAMP_LEVELS = 32
TRANSPARENT = 255

PALETTE = [(0, 0, 0)] * 256
PALETTE[BLACK] = (0, 0, 0)
PALETTE[BACKGROUND] = (24, 40, 64)
PALETTE[WHITE] = (255, 255, 255)
PALETTE[RED] = (230, 40, 40)
PALETTE[GREEN] = (40, 200, 60)
PALETTE[BLUE] = (50, 90, 240)
PALETTE[YELLOW] = (250, 210, 40)
PALETTE[CHECK_LIGHT] = (150, 150, 150)
PALETTE[CHECK_DARK] = (90, 90, 90)
for level in range(RAMP_LEVELS):
    value = level * 255 // (RAMP_LEVELS - 1)
    PALETTE[RAMP_START + level] = (value, value, value)
PALETTE[TRANSPARENT] = (255, 0, 255)

CYCLE_COLOURS = (RED, GREEN, BLUE)

SEGMENTS = {
    0: "abcdef", 1: "bc", 2: "abged", 3: "abgcd", 4: "fgbc",
    5: "afgcd", 6: "afgedc", 7: "abc", 8: "abcdefg", 9: "abcdfg",
}


class Canvas:
    """An 8-bit palette-index raster."""

    def __init__(self, width, height, fill):
        self.width = width
        self.height = height
        self.pixels = bytearray([fill]) * (width * height)

    def rect(self, x0, y0, x1, y1, colour):
        x0, x1 = max(0, x0), min(self.width, x1)
        y0, y1 = max(0, y0), min(self.height, y1)
        if x0 >= x1:
            return
        run = bytes([colour]) * (x1 - x0)
        for y in range(y0, y1):
            start = y * self.width
            self.pixels[start + x0:start + x1] = run

    def disk(self, cx, cy, radius, colour):
        for dy in range(-radius, radius + 1):
            half = int((radius * radius - dy * dy) ** 0.5)
            self.rect(cx - half, cy + dy, cx + half + 1, cy + dy + 1, colour)

    def outline(self, colour):
        self.rect(0, 0, self.width, 1, colour)
        self.rect(0, self.height - 1, self.width, self.height, colour)
        self.rect(0, 0, 1, self.height, colour)
        self.rect(self.width - 1, 0, self.width, self.height, colour)

    def crop(self, x, y, width, height):
        rows = []
        for row in range(y, y + height):
            start = row * self.width + x
            rows.append(bytes(self.pixels[start:start + width]))
        return b"".join(rows)

    def rgb(self):
        flat = [channel for entry in PALETTE for channel in entry]
        image = Image.frombytes("P", (self.width, self.height), bytes(self.pixels))
        image.putpalette(flat)
        return image.convert("RGB")


def draw_digit(canvas, x, y, width, height, digit, colour):
    thickness = max(2, height // 9)
    half = height // 2
    boxes = {
        "a": (x + thickness, y, x + width - thickness, y + thickness),
        "b": (x + width - thickness, y + thickness, x + width, y + half),
        "c": (x + width - thickness, y + half, x + width, y + height - thickness),
        "d": (x + thickness, y + height - thickness, x + width - thickness, y + height),
        "e": (x, y + half, x + thickness, y + height - thickness),
        "f": (x, y + thickness, x + thickness, y + half),
        "g": (x + thickness, y + half - thickness // 2,
              x + width - thickness, y + half - thickness // 2 + thickness),
    }
    for segment in SEGMENTS[digit]:
        canvas.rect(*boxes[segment], colour)


def draw_number(canvas, centre_x, centre_y, height, value, colour, digits=3):
    digit_width = max(5, height // 2)
    gap = max(2, digit_width // 4)
    total = digits * digit_width + (digits - 1) * gap
    x = centre_x - total // 2
    y = centre_y - height // 2
    text = str(value).rjust(digits, "0")[-digits:]
    for character in text:
        draw_digit(canvas, x, y, digit_width, height, int(character), colour)
        x += digit_width + gap


def strip_height(height):
    return max(4, height // 8)


def test_frame(width, height, index, count):
    canvas = Canvas(width, height, BACKGROUND)
    ramp = strip_height(height)
    for level in range(RAMP_LEVELS):
        x0 = level * width // RAMP_LEVELS
        x1 = (level + 1) * width // RAMP_LEVELS
        canvas.rect(x0, 1, x1, ramp, RAMP_START + level)
    square = max(6, height // 8)
    canvas.rect(width - square - 4, ramp + 4, width - 4, ramp + 4 + square,
                CYCLE_COLOURS[index % len(CYCLE_COLOURS)])
    sweep = max(4, height // 10)
    canvas.rect(1, height - sweep - 1, width - 1, height - 1, BLACK)
    x0 = index * width // count
    x1 = max(x0 + 2, (index + 1) * width // count)
    canvas.rect(x0, height - sweep - 1, x1, height - 1, YELLOW)
    usable = height - ramp - sweep
    draw_number(canvas, width // 2, ramp + usable // 2, max(9, usable * 11 // 20), index, WHITE)
    canvas.outline(WHITE)
    return canvas


def noise_tile(size=64, seed=0x2545F491):
    """Integer LCG noise, identical on every platform."""
    values = bytearray()
    state = seed
    for _ in range(size * size):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        values.append(RAMP_START + (state >> 16) % 12)
    return size, bytes(values)


def textured_frame(width, height, index, count):
    """A test frame whose flat background is replaced by scrolling noise.

    Noise defeats GIF LZW runs and WebP's flat-macroblock and sub-frame
    shortcuts, so this is a worst-case decode-speed fixture like dithered
    photographic content.
    """
    canvas = test_frame(width, height, index, count)
    size, tile = noise_tile()
    offset_x, offset_y = index * 3, index * 2
    pixels = canvas.pixels
    for y in range(height):
        row = y * width
        tile_row = ((y + offset_y) % size) * size
        for x in range(width):
            if pixels[row + x] == BACKGROUND:
                pixels[row + x] = tile[tile_row + (x + offset_x) % size]
    return canvas


def lzw_encode(indices, min_code_size=8):
    """GIF LZW with giflib's code-size and table-reset timing."""
    clear = 1 << min_code_size
    end = clear + 1
    output = bytearray()
    accumulator = 0
    bit_count = 0

    def emit(code, size):
        nonlocal accumulator, bit_count
        accumulator |= code << bit_count
        bit_count += size
        while bit_count >= 8:
            output.append(accumulator & 0xFF)
            accumulator >>= 8
            bit_count -= 8

    size = min_code_size + 1
    next_code = end + 1
    table = {}
    emit(clear, size)
    prefix = indices[0]
    for value in indices[1:]:
        key = (prefix << 8) | value
        code = table.get(key)
        if code is not None:
            prefix = code
            continue
        emit(prefix, size)
        if next_code >= (1 << size) and size < 12:
            size += 1
        if next_code >= 4095:
            emit(clear, size)
            table.clear()
            next_code = end + 1
            size = min_code_size + 1
        else:
            table[key] = next_code
            next_code += 1
        prefix = value
    emit(prefix, size)
    if next_code >= (1 << size) and size < 12:
        size += 1
    emit(end, size)
    if bit_count:
        output.append(accumulator & 0xFF)
    return bytes(output)


def sub_blocks(data):
    blocks = bytearray()
    for offset in range(0, len(data), 255):
        chunk = data[offset:offset + 255]
        blocks.append(len(chunk))
        blocks += chunk
    blocks.append(0)
    return bytes(blocks)


def gif_bytes(width, height, frames, loop):
    """frames: dicts with x, y, width, height, pixels, delay_ms, disposal, transparent.

    loop=None omits the NETSCAPE2.0 extension (the animation plays once).
    """
    output = bytearray(b"GIF89a")
    # Global table: 256 entries, 8-bit colour resolution, background index 0.
    output += struct.pack("<HHBBB", width, height, 0xF7, BLACK, 0)
    for entry in PALETTE:
        output += bytes(entry)
    if loop is not None:
        output += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\x00"
    for frame in frames:
        transparent = frame.get("transparent")
        packed = (frame.get("disposal", 1) & 7) << 2 | (1 if transparent is not None else 0)
        output += struct.pack("<BBBBHBB", 0x21, 0xF9, 4, packed,
                              frame["delay_ms"] // 10, transparent or 0, 0)
        output += struct.pack("<BHHHHB", 0x2C, frame["x"], frame["y"],
                              frame["width"], frame["height"], 0)
        output.append(8)
        output += sub_blocks(lzw_encode(frame["pixels"]))
    output.append(0x3B)
    return bytes(output)


def full_frames(width, height, count, delay_ms, painter=test_frame):
    frames, canvases = [], []
    for index in range(count):
        canvas = painter(width, height, index, count)
        canvases.append(canvas)
        frames.append({"x": 0, "y": 0, "width": width, "height": height,
                       "pixels": bytes(canvas.pixels), "delay_ms": delay_ms, "disposal": 1})
    return frames, canvases


def disposal_fixture():
    """Sprites over a checkerboard; the sprite colour names its disposal.

    red   = 1 (leave in place): red disks accumulate into a trail.
    green = 2 (restore to background): leaves a black hole in the board.
    blue  = 3 (restore previous): vanishes, leaving the earlier content.
    The disk sits on transparent corners (index 255) inside its 64x64 frame.
    """
    width, height, sprite = 320, 240, 64
    board = Canvas(width, height, CHECK_DARK)
    for y in range(0, height, 20):
        for x in range(0, width, 20):
            if (x // 20 + y // 20) % 2 == 0:
                board.rect(x, y, x + 20, y + 20, CHECK_LIGHT)
    draw_number(board, width // 2, 32, 40, 0, WHITE)
    board.outline(WHITE)
    frames = [{"x": 0, "y": 0, "width": width, "height": height,
               "pixels": bytes(board.pixels), "delay_ms": 500, "disposal": 1}]
    disposals = (1, 2, 3)
    colours = {1: RED, 2: GREEN, 3: BLUE}
    for step in range(9):
        disposal = disposals[step % 3]
        tile = Canvas(sprite, sprite, TRANSPARENT)
        tile.disk(sprite // 2, sprite // 2, sprite // 2 - 2, colours[disposal])
        draw_number(tile, sprite // 2, sprite // 2, 22, step + 1, WHITE, digits=1)
        frames.append({"x": 8 + step * 34, "y": 96 + (step % 2) * 40, "width": sprite,
                       "height": sprite, "pixels": bytes(tile.pixels), "delay_ms": 400,
                       "disposal": disposal, "transparent": TRANSPARENT})
    return gif_bytes(width, height, frames, loop=0), len(frames)


def webp_frames(width, height, count, painter=test_frame):
    """RGB test frames whose ramp strip is a smooth 0..255 gradient."""
    images = []
    ramp = strip_height(height)
    gradient = Image.frombytes(
        "L", (width - 2, ramp - 1),
        bytes(x * 255 // (width - 3) for x in range(width - 2)) * (ramp - 1)).convert("RGB")
    for index in range(count):
        image = painter(width, height, index, count).rgb()
        image.paste(gradient, (1, 1))
        images.append(image)
    return images


def alpha_webp_frames(count):
    """Left half fully transparent, right half an opaque board, and a moving
    50%-alpha white square. On the device's black canvas the transparent
    half is black and the square over it is mid grey."""
    width, height = 320, 240
    images = []
    for index in range(count):
        base = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        board = Canvas(width // 2, height, CHECK_DARK)
        for y in range(0, height, 20):
            for x in range(0, width // 2, 20):
                if (x // 20 + y // 20) % 2 == 0:
                    board.rect(x, y, x + 20, y + 20, CHECK_LIGHT)
        base.paste(board.rgb().convert("RGBA"), (width // 2, 0))
        overlay = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        x = index * (width - 80) // max(1, count - 1)
        overlay.paste(Image.new("RGBA", (80, 80), (255, 255, 255, 128)), (x, 80))
        number = Canvas(90, 40, TRANSPARENT)
        draw_number(number, 45, 20, 36, index, WHITE)
        mask = Image.frombytes("L", (90, 40), bytes(0 if p == TRANSPARENT else 255
                                                       for p in number.pixels))
        overlay.paste(Image.new("RGBA", (90, 40), (255, 255, 255, 255)), (115, 10), mask)
        images.append(Image.alpha_composite(base, overlay))
    return images


def save_webp(path, images, delay_ms, **options):
    images[0].save(path, format="WEBP", save_all=True, append_images=images[1:],
                   duration=delay_ms, loop=0, method=4, **options)


def riff_chunk(path):
    data = path.read_bytes()[:21]
    return data[12:16].decode("ascii"), data[20] if len(data) > 20 else 0


def verify_gif(path, expected_frames, canvases=None, loop=None):
    with Image.open(path) as image:
        if image.n_frames != expected_frames:
            raise SystemExit(f"{path.name}: {image.n_frames} frames, expected {expected_frames}")
        if loop is not None and image.info.get("loop") != loop:
            raise SystemExit(f"{path.name}: loop {image.info.get('loop')}, expected {loop}")
        if loop is None and "loop" in image.info:
            raise SystemExit(f"{path.name}: unexpected NETSCAPE loop extension")
        for index in range(expected_frames):
            image.seek(index)
            if canvases and image.convert("RGB").tobytes() != canvases[index].rgb().tobytes():
                raise SystemExit(f"{path.name}: frame {index} does not round-trip")


def verify_webp(path, expected_frames, lossless_images=None):
    with Image.open(path) as image:
        frames = getattr(image, "n_frames", 1)
        if frames != expected_frames:
            raise SystemExit(f"{path.name}: {frames} frames, expected {expected_frames}")
        if lossless_images:
            for index, expected in enumerate(lossless_images):
                image.seek(index)
                if image.convert(expected.mode).tobytes() != expected.tobytes():
                    raise SystemExit(f"{path.name}: lossless frame {index} differs")


def main():
    if PIL.__version__ != PILLOW_VERSION:
        raise SystemExit(f"Pillow {PILLOW_VERSION} is required; found {PIL.__version__}")
    if not features.check("webp"):
        raise SystemExit("Pillow was built without WebP support")
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUTPUT
    output.mkdir(parents=True, exist_ok=True)

    full_gifs = (
        ("gif_160x120_100ms.gif", 160, 120, 30, 100, 0),
        ("gif_320x240_100ms.gif", 320, 240, 30, 100, 0),
        ("gif_320x240_40ms.gif", 320, 240, 50, 40, 0),
        ("gif_320x240_0ms.gif", 320, 240, 30, 0, 0),
        ("gif_317x237_100ms.gif", 317, 237, 30, 100, 0),
        ("gif_640x480_100ms.gif", 640, 480, 20, 100, 0),
        ("gif_1000x1000_100ms.gif", 1000, 1000, 12, 100, 0),
        ("gif_no_loop.gif", 320, 240, 10, 200, None),
        ("gif_loop2.gif", 320, 240, 10, 200, 2),
        ("gif_single_frame.gif", 320, 240, 1, 100, 0),
        ("gif_320x240_textured_100ms.gif", 320, 240, 20, 100, 0, textured_frame),
    )
    for name, width, height, count, delay_ms, loop, *painter in full_gifs:
        frames, canvases = full_frames(width, height, count, delay_ms, *painter)
        path = output / name
        path.write_bytes(gif_bytes(width, height, frames, loop))
        verify_gif(path, count, canvases, loop)
        print(f"{name}: {count} frame(s), {path.stat().st_size} bytes")

    # Cut the 320x240 loop inside a frame's image data and drop the trailer.
    complete = (output / "gif_320x240_100ms.gif").read_bytes()
    truncated = output / "gif_truncated.gif"
    truncated.write_bytes(complete[:len(complete) * 3 // 5])
    print(f"gif_truncated.gif: first {truncated.stat().st_size} of {len(complete)} bytes")

    disposal, disposal_frames = disposal_fixture()
    path = output / "gif_disposal_alpha.gif"
    path.write_bytes(disposal)
    verify_gif(path, disposal_frames, loop=0)
    print(f"gif_disposal_alpha.gif: {disposal_frames} frame(s), {len(disposal)} bytes")

    lossy = webp_frames(320, 240, 30)
    save_webp(output / "webp_anim_lossy_320x240.webp", lossy, 100, quality=80)
    verify_webp(output / "webp_anim_lossy_320x240.webp", 30)
    lossless = webp_frames(320, 240, 30)
    save_webp(output / "webp_anim_lossless_320x240.webp", lossless, 100, lossless=True)
    verify_webp(output / "webp_anim_lossless_320x240.webp", 30, lossless)
    large = webp_frames(640, 480, 20)
    save_webp(output / "webp_anim_640x480_lossy.webp", large, 100, quality=80)
    verify_webp(output / "webp_anim_640x480_lossy.webp", 20)
    textured = webp_frames(320, 240, 20, textured_frame)
    save_webp(output / "webp_anim_textured_320x240_lossy.webp", textured, 100, quality=80)
    verify_webp(output / "webp_anim_textured_320x240_lossy.webp", 20)
    alpha = alpha_webp_frames(20)
    save_webp(output / "webp_anim_alpha_blend.webp", alpha, 150, lossless=True)
    verify_webp(output / "webp_anim_alpha_blend.webp", 20, alpha)

    # A wide still image keeps the static .em16 path, crop zoom, and the
    # simple (VP8, no VP8X) and extended-with-alpha (VP8X, no animation) layouts.
    still = webp_frames(480, 200, 1)[0]
    still.save(output / "webp_static.webp", format="WEBP", quality=85, method=4)
    still_alpha = still.convert("RGBA")
    still_alpha.putalpha(Image.linear_gradient("L").rotate(90).resize(still.size))
    still_alpha.save(output / "webp_static_alpha.webp", format="WEBP", quality=85, method=4)
    for name, expected_chunk in (("webp_static.webp", "VP8 "), ("webp_static_alpha.webp", "VP8X")):
        chunk, flags = riff_chunk(output / name)
        if chunk != expected_chunk or (chunk == "VP8X" and flags & 0x02):
            raise SystemExit(f"{name}: unexpected first chunk {chunk!r} flags {flags:#04x}")
        verify_webp(output / name, 1)
    for name in ("webp_anim_lossy_320x240.webp", "webp_anim_lossless_320x240.webp",
                 "webp_anim_640x480_lossy.webp", "webp_anim_textured_320x240_lossy.webp",
                 "webp_anim_alpha_blend.webp"):
        chunk, flags = riff_chunk(output / name)
        if chunk != "VP8X" or not flags & 0x02:
            raise SystemExit(f"{name}: missing VP8X animation flag")
    for path in sorted(output.glob("webp_*.webp")):
        print(f"{path.name}: {path.stat().st_size} bytes")
    print(f"Pillow {PIL.__version__}, libwebp {features.version('webp')}")


if __name__ == "__main__":
    main()
