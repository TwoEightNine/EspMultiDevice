#!/usr/bin/env python3
"""Synthetic 2ch API for testing examples/apps/dvach without the real site.

  mock_dvach.py write DIR        write the fixture files the host harness uses
  mock_dvach.py serve [--port N] [--challenge]
                                 serve them over HTTP for a device on the LAN

Every response mirrors the shape of the real endpoints probed on 2026-10-26:
board catalogs, the posts-after API, the emoji captcha (id, show, click), and
multipart posting, plus thumbnails (/pr/thumb/...) and full files
(/pr/src/...). Posts in thread 100 carry a JPEG, PNG, and MP4; a WebM; a
30 MiB MP4 for testing cancel; a .jpg that is really a PNG, which the device
must refuse; four files in one post; and three MP4s in one post. The MP4s are only an ISO-BMFF header
and zero padding, enough for the download check but not playable. The
content is generated; no real posts are included. For a
device test, copy the app with a manifest whose "hosts" lists this machine's IP
and put {"mirrors": ["http://IP:PORT"]} in /.system/appdata/dvach/config.json.
With --challenge every GET answers like a Cloudflare browser check.
"""

import argparse
import base64
import hashlib
import json
import re
import struct
import sys
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BOARD = "pr"
THREAD = 100
BIG_KB = 30 * 1024
MISMATCH = 66  # file index whose .jpg is served as a PNG
POW_TEMPLATE = "mockChallenge%dEnd"
POW_ANSWER = 4242
CAPTCHA_ID = "mock-captcha-token"
SUCCESS_TOKEN = "mock-success-token"


def png(width, height, color):
    """A solid RGBA PNG with a darker border, built without third-party modules."""
    r, g, b = color
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            edge = x < 2 or y < 2 or x >= width - 2 or y >= height - 2
            row += bytes((r // 2, g // 2, b // 2, 255) if edge else (r, g, b, 255))
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")


def b64png(width, height, color):
    return base64.b64encode(png(width, height, color)).decode("ascii")


# A 216x250 baseline JPEG (a yellow ellipse on blue), made once with Pillow
# 12.3.0 and embedded so this script needs only the standard library. It is
# every thumbnail and every full .jpg file.
JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAA0JCgsKCA0LCgsODg0PEyAVExISEyccHhcgLikxMC4pLSwzOko+MzZGNywtQFdB"
    "RkxOUlNSMj5aYVpQYEpRUk//2wBDAQ4ODhMREyYVFSZPNS01T09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09P"
    "T09PT09PT09PT09PT0//wAARCAD6ANgDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
    "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
    "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
    "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
    "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
    "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
    "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDDooor2TzQooooAKKKKACiiigAooooAKKK"
    "KACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKK"
    "KACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKK"
    "KACiiigAooooAKKKKACiiigAooooAKKKKACirMVjdzFfLt5CGGQSuAfxPFTf2NqH/Pv/AOPr/jWEsTQg7Sml80aKlUeqi/uK"
    "FFWpNNvYm2tbSE4z8o3D9Kq1pCpCavBp+hMoyjurBRRRVkhRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABR"
    "SqpZgqglicADqa6bStJS2QS3Cq855weQn09/euPGY2nhYc0t+iN6FCVaVkZ9jocspD3eYo8fdB+Y/wCFbttZW1rnyIVQnqep"
    "/M1PRXyWJx9fEv3nZdlsezSw9OlstQoooriNwqK4toLpNs8SuB0z1H0PapaKqMpRfNF2Ymk1ZmBf6CwO+xO4d42PI57H/H0r"
    "EZSrFWBDA4IPUV3VUdS0yK9jLKFScch8dfY17uBzmUWoV9V36/M8/EYFP3qe/Y5KinyxPDK0cqlXU4INMr6ZNNXR5TVtGFFF"
    "FMQUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUVc0q0F7erGxARRvcHuARxWdWpGlBzlsioRc5KK6mtoOn+WgvJfvOPkUjoPX"
    "8f5fWtmiivhcTiJ4io6kj6ClSVKKigooornNQooooAKKKKACiiigDN1rT/tcHmx/62IHAA+8PT/CuWru65jXrQW935qkbJyW"
    "x3B7/wA6+jyXGu/1efy/y/U8zHUP+Xi+Zl0UUV9GeWFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFdRoFsIbHzSCHmOTkY4HT/"
    "AB/GuYVSzBVBLE4AHU13EMYhhSJSSqKFGevFeFntZxpRprq/yPQy+F5uXYdRRRXyx64UUUUAFFFFABRRRQAUUUUAFU9VthdW"
    "EiYJdRvTAycj/Hp+NXKK0pVJU5qcd0TOKlFxfU4SipruH7Pdyw4YBHIG7rjt+lQ1+gRkpRUl1Pm2rOzCiiiqEFFFFABRRRQA"
    "UUUUAFFFFABRRRQBa0uMy6lbqpGQ4bn25/pXY1yeif8AIWg/4F/6Ca6yvlc9k/bxXl+rPXy9fu2/MKKKK8M9AKKKKACiiigA"
    "ooooAKKKKACiiigDltejKao7EjEihhj6Y/pWbWt4k/5CEf8A1yH8zWTX3WAk5YaDfY+fxKtVl6hRRRXYYBRRRQAUUUUAFFFF"
    "ABRRRQAUUUUAXdHdY9VgZzgEkfiQQP1NddXEQSeTcRy43bHDYzjODXb18vn0LVYT7q33f8Oetl0vccQooorwT0QooooAKKKK"
    "ACiiigAooooAKKKKAOZ8ROraiFU5KRgN7HJP8iKyqt6rJ5up3DYxh9vX04/pVSvvMHDkw8I+SPna8uapJ+YUUUV1GQUUUUAF"
    "FFFABRRRQAUUUUAFFFFABXX6TMJtNhIxlV2EA5xjj/6/41yFa/h66EVy0EjgJIPlB/vf/X/oK8rN8O6uHut46/5nZgqnJUs+"
    "p0lFFFfHHthRRRQAUUUUAFFFFABRRRQAVHczC3tpJmxhFJwTjPtUlYfiO6G1LWNxnO6QDqPQH+f5V1YPDvEVo0+nX0Ma9T2d"
    "NyMCiiivvD54KKKKACiiigAooooAKKKKACiiigAooooAKVWKsGUkMDkEdRSUUgOw029W+tg/yiQcOo7H/wCvVuuNsbt7K5E0"
    "YB4wynuK621uYruASwtlT27g+hr47MsA8NPmivdf4eX+R7mFxCqxs90S0UUV5Z1hRRRQAUUUUAFFFNllSGJpJWCooySaaTbs"
    "hN21ZHeXKWls80hHA4BP3j2FcdPM9xO80hyznJqzqd+1/ODjbGmQg7/U1Sr7DK8D9Wp80/if4eR4uLxHtZWWyCiiivVOMKKK"
    "KACiiigAooooAKKKKACiiigAooooAKKKKACrFpeT2cheB8Z+8DyG+tV6KicIzi4yV0OMnF3R11jqlvekIpKS4+439D3q7XCV"
    "fttXvbfI8zzQe0mW/XrXz+JyPXmoP5P/ADPTpZh0qI6yisOLxEMqJrYgY+ZkbP5A/wCNTf8ACQWn/POf/vkf415cssxcXZwO"
    "pYui/tGtRWLJ4ijDfurZ2XHVmCn+tZ9xrV7Om0MsQ7+WCCfxralk+Km9VZeZE8bSjs7nQXuoW9kB5zEuRkIoyTXM3+oT3z/v"
    "DiMHKoOg/wAaqsxZizElicknqaSvfwWWUsN728u/+R51fFTq6bIKKKK9I5QooooAKKKKACiiigAooooAKKKKACiiigAooooA"
    "KKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA"
    "KKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA"
    "KKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigD/2Q=="
)

# 2ch file type codes.
TYPES = {"jpg": 1, "png": 2, "gif": 4, "webm": 6, "webp": 9, "mp4": 10}


def file_entry(thread, index, ext, size_kb=112):
    name = f"{thread}{index:04d}.{ext}"
    return {"name": name, "fullname": f"file.{ext}", "path": f"/{BOARD}/src/{thread}/{name}",
            "thumbnail": f"/{BOARD}/thumb/{thread}/{thread}{index:04d}s.jpg", "type": TYPES[ext],
            "size": size_kb, "width": 516, "height": 595, "tn_width": 216, "tn_height": 250}


def mp4_header():
    return struct.pack(">I", 24) + b"ftypisom" + struct.pack(">I", 0x200) + b"isomiso2"


def full_file(name):
    """Bytes of a full file and its content type, or None."""
    match = re.fullmatch(r"(\d+?)(\d{4})\.(\w+)", name)
    if not match:
        return None
    index, ext = int(match.group(2)), match.group(3)
    if ext == "jpg":
        return (png(64, 48, (200, 60, 60)), "image/jpeg") if index == MISMATCH else (JPEG, "image/jpeg")
    if ext == "png":
        return png(516, 595, (60, 160, 90)), "image/png"
    if ext == "mp4":
        size = BIG_KB * 1024 if index == 9 else 64 * 1024
        return mp4_header() + bytes(size - 24), "video/mp4"
    return None


def catalog():
    threads = []
    for i in range(30):
        subject = "" if i % 7 == 3 else f"Тред про программирование №{i}"
        comment = (f"Обсуждаем <b>тему {i}</b><br>&gt;цитата<br>"
                   f"<a href=\"/{BOARD}/res/{THREAD + i}.html\">&gt;&gt;{THREAD + i}</a> "
                   + "длинный текст " * 30)
        threads.append({
            "num": THREAD + i * 1000, "subject": subject, "comment": comment,
            "posts_count": 10 + i, "files_count": i % 3, "lasthit": 1793000000 - i * 600,
            "sticky": 1 if i == 0 else 0, "closed": 1 if i == 5 else 0,
            "files": [] if i % 7 == 3 else [file_entry(THREAD + i * 1000, 1, "mp4" if i in (1, 2) else "jpg")],
            "name": "Аноним", "views": 100 + i,
        })
    return {"advert_top_image": "/banners/x.png",
            "board": {"id": BOARD, "name": "Программирование", "max_pages": 10},
            "threads": threads}


POST_FILES = {
    THREAD: ["jpg", "png", "mp4"],
    THREAD + 1: ["webm"],
    THREAD + 2: ["mp4-big"],
    THREAD + 3: ["jpg-mismatch"],
    THREAD + 4: ["jpg", "png", "jpg", "png"],
    THREAD + 5: ["mp4", "mp4", "mp4"],
}


def post_files(num):
    out = []
    for k, kind in enumerate(POST_FILES.get(num, []), start=1):
        if kind == "mp4-big":
            out.append(file_entry(num, 9, "mp4", BIG_KB))
        elif kind == "jpg-mismatch":
            out.append(file_entry(num, MISMATCH, "jpg"))
        else:
            out.append(file_entry(num, k, kind))
    return out


def posts(start, count):
    out = []
    for n in range(count):
        num = start + n
        comment = (f"Пост номер {num}.<br>Вторая строка с &quot;кавычками&quot; и "
                   f"<span class=\"unkfunc\">&gt;гринтекстом</span>.<br>" + "слово " * (n * 7 % 60))
        out.append({"num": num, "number": num - THREAD + 1, "date": "15/10/26 Чтв 18:48:46",
                    "name": "Аноним", "comment": comment, "op": 1 if num == THREAD else 0,
                    "files": post_files(num), "parent": THREAD, "trip": ""})
    return out


def after(start):
    # The thread holds posts THREAD..THREAD+34; the API returns all from `start`.
    first = max(start, THREAD)
    last = THREAD + 34
    return {"posts": posts(first, max(0, last - first + 1)), "result": 1, "unique_posters": 5}


def captcha_id():
    digest = hashlib.sha512(POW_TEMPLATE.replace("%d", str(POW_ANSWER)).encode()).hexdigest()
    challenge = "{'hash': '%s', 'limit': 20000, 'template': '%s'}" % (digest, POW_TEMPLATE)
    return {"result": 1, "type": "emoji", "input": "emoji", "id": CAPTCHA_ID, "challenge": challenge}


def captcha_step(shade):
    keys = [b64png(50, 45, (40 * k % 256, 200 - shade, 120)) for k in range(8)]
    return {"image": b64png(300, 100, (shade, 90, 160)), "keyboard": keys}


FIXTURES = {
    "catalog.json": catalog(),
    "after_first.json": after(THREAD),
    "after_rest.json": after(THREAD + 25),
    "captcha_id.json": captcha_id(),
    "captcha_show.json": captcha_step(60),
    "captcha_next.json": captcha_step(120),
    "captcha_success.json": {"success": SUCCESS_TOKEN},
    "posting_ok.json": {"result": 1, "num": THREAD + 35},
}


def write(directory):
    target = Path(directory)
    target.mkdir(parents=True, exist_ok=True)
    for name, value in FIXTURES.items():
        (target / name).write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
    (target / "thumb.jpg").write_bytes(JPEG)
    print(f"wrote {len(FIXTURES) + 1} fixtures to {target}")


class Handler(BaseHTTPRequestHandler):
    challenge = False
    clicks = 0

    def reply(self, status, value, extra=None):
        body = (json.dumps(value, ensure_ascii=False) if not isinstance(value, bytes) else value)
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8" if not isinstance(value, bytes)
                         else "text/html; charset=utf-8")
        for name, content in (extra or {}).items():
            self.send_header(name, content)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_bytes(self, data, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            for offset in range(0, len(data), 65536):
                self.wfile.write(data[offset:offset + 65536])
        except (BrokenPipeError, ConnectionResetError):
            print("client stopped the transfer")

    def do_GET(self):
        path = self.path.split("?")[0]
        if Handler.challenge:
            return self.reply(403, b"<html>Just a moment...</html>", {"cf-mitigated": "challenge"})
        if path.startswith(f"/{BOARD}/thumb/"):
            return self.send_bytes(JPEG, "image/jpeg")
        if path.startswith(f"/{BOARD}/src/"):
            found = full_file(path.rsplit("/", 1)[-1])
            if found:
                return self.send_bytes(*found)
        if path == f"/{BOARD}/catalog.json":
            return self.reply(200, FIXTURES["catalog.json"])
        match = re.fullmatch(rf"/api/mobile/v2/after/{BOARD}/{THREAD}/(\d+)", path)
        if match:
            return self.reply(200, after(int(match.group(1))))
        if path == "/api/captcha/emoji/id":
            Handler.clicks = 0
            return self.reply(200, FIXTURES["captcha_id.json"])
        if path == "/api/captcha/emoji/show":
            return self.reply(200, FIXTURES["captcha_show.json"])
        return self.reply(404, b"<html>Not found</html>")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        path = self.path.split("?")[0]
        if path == "/api/captcha/emoji/click":
            request = json.loads(body or b"{}")
            if request.get("captchaTokenID") != CAPTCHA_ID:
                return self.reply(200, {"result": 0})
            Handler.clicks += 1
            return self.reply(200, FIXTURES["captcha_next.json"] if Handler.clicks < 2
                              else FIXTURES["captcha_success.json"])
        if path == "/user/posting":
            fields = dict(re.findall(rb'name="([^"]+)"\r\n\r\n(.*?)\r\n--', body, re.S))
            problems = []
            if fields.get(b"emoji_captcha_id") != SUCCESS_TOKEN.encode():
                problems.append("captcha")
            if fields.get(b"2ch_challenge") != str(POW_ANSWER).encode():
                problems.append("challenge")
            if not fields.get(b"comment", b"").strip():
                problems.append("comment")
            if problems:
                return self.reply(200, {"result": 0, "error": {"code": -5, "message": "Bad " + ", ".join(problems)}})
            print("posted:", fields.get(b"comment", b"").decode("utf-8", "replace"))
            return self.reply(200, FIXTURES["posting_ok.json"])
        return self.reply(404, b"<html>Not found</html>")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    write_parser = commands.add_parser("write")
    write_parser.add_argument("directory")
    serve_parser = commands.add_parser("serve")
    serve_parser.add_argument("--port", type=int, default=8080)
    serve_parser.add_argument("--challenge", action="store_true")
    args = parser.parse_args()
    if args.command == "write":
        write(args.directory)
        return 0
    Handler.challenge = args.challenge
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"mock 2ch on port {args.port}; Ctrl-C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
