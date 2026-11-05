#!/usr/bin/env python3
"""Writes deterministic tagged MP3/WAV fixtures for tests/audio_tags.

Standard library only. The MP3 audio is silent MPEG-1 Layer III mono frames
(128 kbit/s, 44.1 kHz) and the WAV audio is 8 kHz 16-bit mono silence, so
every fixture also plays on the device. Embedded pictures are real 24x24
PNGs. Expected values live in tests/audio_tags/audio_tags.cpp.

Usage: tools/generate_audio_tag_fixtures.py [output directory]
"""

import os
import struct
import sys
import zlib

DEFAULT_OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "audio_tags", "fixtures")


# ---- audio payloads ------------------------------------------------------

def mp3_frames(count=40):
    # FF FB 90 C4: MPEG-1, Layer III, no CRC, 128 kbit/s, 44.1 kHz, no
    # padding, mono. 144 * 128000 / 44100 = 417 bytes. Zero side info and
    # main data decode to silence.
    frame = bytes([0xFF, 0xFB, 0x90, 0xC4]) + bytes(417 - 4)
    return frame * count


def wav_file(extra_chunks=b"", frames=4000):
    fmt = struct.pack("<HHIIHH", 1, 1, 8000, 16000, 2, 16)
    body = b"WAVE" + chunk(b"fmt ", fmt) + extra_chunks + chunk(b"data", bytes(frames * 2))
    return b"RIFF" + struct.pack("<I", len(body)) + body


def chunk(identifier, payload):
    padded = payload + (b"\0" if len(payload) % 2 else b"")
    return identifier + struct.pack("<I", len(payload)) + padded


def png(rgb=(200, 40, 60), size=24):
    def png_chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + bytes(rgb) * size for _ in range(size))
    return (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(rows, 9))
            + png_chunk(b"IEND", b""))


# ---- ID3 building blocks -------------------------------------------------

def synchsafe(value):
    return bytes([(value >> 21) & 0x7F, (value >> 14) & 0x7F, (value >> 7) & 0x7F, value & 0x7F])


def unsync(data):
    out = bytearray()
    for i, b in enumerate(data):
        out.append(b)
        if b == 0xFF and (i + 1 == len(data) or data[i + 1] == 0x00 or data[i + 1] >= 0xE0):
            out.append(0x00)
    return bytes(out)


def text(value, encoding):
    if encoding == 0:
        return b"\0" + value.encode("latin-1")
    if encoding == 1:
        return b"\1" + value.encode("utf-16")  # with BOM (little-endian here)
    if encoding == 2:
        return b"\2" + value.encode("utf-16-be")
    return b"\3" + value.encode("utf-8")


def raw_text(value_bytes, encoding=0):
    return bytes([encoding]) + value_bytes


def apic(picture, picture_type, mime=b"image/png", description="cover", encoding=0):
    if encoding in (1, 2):
        desc = (description.encode("utf-16") if encoding == 1 else description.encode("utf-16-be")) + b"\0\0"
    else:
        desc = description.encode("latin-1") + b"\0"
    return bytes([encoding]) + mime + b"\0" + bytes([picture_type]) + desc + picture


def pic22(picture, picture_type):
    return b"\0" + b"PNG" + bytes([picture_type]) + b"front\0" + picture


def frame23(identifier, body, flags=b"\0\0"):
    return identifier + struct.pack(">I", len(body)) + flags + body


def frame24(identifier, body, flags=b"\0\0"):
    return identifier + synchsafe(len(body)) + flags + body


def frame22(identifier, body):
    return identifier + struct.pack(">I", len(body))[1:] + body


def id3(version, frames, flags=0, padding=0, extended=b"", size_override=None):
    body = extended + frames + bytes(padding)
    size = len(body) if size_override is None else size_override
    return b"ID3" + bytes([version, 0, flags]) + synchsafe(size) + body


def id3v1(title=b"", artist=b"", album=b"", year=b"", genre=255):
    def field(value, length):
        return value[:length] + b"\0" * (length - len(value[:length]))
    return b"TAG" + field(title, 30) + field(artist, 30) + field(album, 30) + field(year, 4) + bytes(30) + bytes([genre])


# ---- fixtures ------------------------------------------------------------

def fixtures():
    red = png((200, 40, 60))
    blue = png((40, 60, 200))
    green = png((40, 200, 60))
    files = {}

    # ID3v2.3: Latin-1, UTF-16 with BOM, a back cover before the front cover.
    frames = (frame23(b"TIT2", text("Blue Monday", 0))
              + frame23(b"TPE1", text("Кино", 1))
              + frame23(b"TALB", text("Beyoncé Live", 0))
              + frame23(b"TYER", text("1983", 0))
              + frame23(b"APIC", apic(blue, 4))
              + frame23(b"APIC", apic(red, 3, description="Front", encoding=1))
              + frame23(b"TXXX", text("ignored", 0)))
    files["v23_basic.mp3"] = id3(3, frames, padding=64) + mp3_frames()

    # ID3v2.4: extended header, UTF-8, TPE2 before TPE1, a per-frame
    # unsynchronised UTF-16 title behind a data-length indicator, TDRC date.
    title_body = text("ÿes, Ünïcode", 1)
    extended = synchsafe(6) + b"\x01\x00"
    frames = (frame24(b"TPE2", text("Album Artist", 3))
              + frame24(b"TPE1", text("Lead Artist", 3))
              + frame24(b"TIT2", synchsafe(len(title_body)) + unsync(title_body), flags=b"\x00\x03")
              + frame24(b"TALB", text("Привет мир", 3))
              + frame24(b"TDRC", text("2004-05-06", 3))
              + frame24(b"APIC", apic(green, 3, mime=b"image/png", encoding=3)))
    files["v24_utf8.mp3"] = id3(4, frames, flags=0x40, extended=extended, padding=32) + mp3_frames()

    # ID3v2.2 three-character frames and PIC.
    frames = (frame22(b"TT2", text("Old Format", 0))
              + frame22(b"TP1", text("Legacy Band", 2))
              + frame22(b"TAL", text("Twenty Two", 0))
              + frame22(b"TYE", text("1999", 0))
              + frame22(b"PIC", pic22(red, 3)))
    files["v22.mp3"] = id3(2, frames) + mp3_frames()

    # ID3v2.3 with tag-level unsynchronisation: text survives, the picture
    # is not offered because its bytes on disk are not the raw image.
    frames = (frame23(b"TIT2", text("ÿÿ Sync", 1))
              + frame23(b"TPE1", text("Unsync Artist", 0))
              + frame23(b"APIC", apic(red, 3)))
    files["v23_unsync.mp3"] = id3(3, unsync(frames), flags=0x80) + mp3_frames()

    # ID3v1 only, Windows-1251 Cyrillic and a Latin-1 accent.
    files["v1_cp1251.mp3"] = mp3_frames() + id3v1(
        title="Группа крови".encode("cp1251"),
        artist="Кино".encode("cp1251"),
        album="Café".encode("latin-1"),
        year=b"1988")

    # ID3v2 title only; ID3v1 fills the artist but cannot replace the title.
    files["v2_plus_v1.mp3"] = (id3(3, frame23(b"TIT2", text("From V2", 0)))
                               + mp3_frames()
                               + id3v1(title=b"From V1", artist=b"V1 Artist", year=b"2010"))

    files["untagged.mp3"] = mp3_frames()

    # A frame claiming more bytes than the tag holds ends the tag; the
    # title read before it is kept. The tag size also overstates the file.
    frames = frame23(b"TIT2", text("Kept Title", 0)) + b"TPE1" + struct.pack(">I", 0x7FFFFFF0) + b"\0\0" + b"x" * 16
    files["malformed.mp3"] = id3(3, frames, size_override=len(frames) + 100000) + mp3_frames(4)

    # WAV with LIST/INFO only.
    info = b"INFO" + chunk(b"INAM", b"Wave Title\0") + chunk(b"IART", b"Wave Artist\0") \
        + chunk(b"IPRD", b"Wave Album\0") + chunk(b"ICRD", b"2015-01-01\0") + chunk(b"ICMT", b"comment\0")
    files["wav_info.wav"] = wav_file(chunk(b"LIST", info))

    # WAV with LIST/INFO and an `id3 ` chunk: ID3 wins where both exist.
    tag = id3(3, frame23(b"TIT2", text("ID3 Title", 0)) + frame23(b"APIC", apic(blue, 3)))
    info = b"INFO" + chunk(b"INAM", b"Info Title\0") + chunk(b"IART", b"Info Artist\0")
    files["wav_id3.wav"] = wav_file(chunk(b"LIST", info) + chunk(b"id3 ", tag))
    return files


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT)
    os.makedirs(out, exist_ok=True)
    for name, data in sorted(fixtures().items()):
        with open(os.path.join(out, name), "wb") as handle:
            handle.write(data)
        print(f"{name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
