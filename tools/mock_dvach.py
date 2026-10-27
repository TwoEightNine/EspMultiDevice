#!/usr/bin/env python3
"""Synthetic 2ch API for testing examples/apps/dvach without the real site.

  mock_dvach.py write DIR        write the fixture files the host harness uses
  mock_dvach.py serve [--port N] [--challenge]
                                 serve them over HTTP for a device on the LAN

Every response mirrors the shape of the real endpoints probed on 2026-10-26:
board catalogs, the posts-after API, the emoji captcha (id, show, click), and
multipart posting. The content is generated; no real posts are included. For a
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
            "files": [{"name": "a.jpg", "thumbnail": "/pr/thumb/a.jpg", "width": 800}],
            "name": "Аноним", "views": 100 + i,
        })
    return {"advert_top_image": "/banners/x.png",
            "board": {"id": BOARD, "name": "Программирование", "max_pages": 10},
            "threads": threads}


def posts(start, count):
    out = []
    for n in range(count):
        num = start + n
        comment = (f"Пост номер {num}.<br>Вторая строка с &quot;кавычками&quot; и "
                   f"<span class=\"unkfunc\">&gt;гринтекстом</span>.<br>" + "слово " * (n * 7 % 60))
        out.append({"num": num, "number": num - THREAD + 1, "date": "15/10/26 Чтв 18:48:46",
                    "name": "Аноним", "comment": comment, "op": 1 if num == THREAD else 0,
                    "files": [], "parent": THREAD, "trip": ""})
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
    print(f"wrote {len(FIXTURES)} fixtures to {target}")


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

    def do_GET(self):
        path = self.path.split("?")[0]
        if Handler.challenge:
            return self.reply(403, b"<html>Just a moment...</html>", {"cf-mitigated": "challenge"})
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
