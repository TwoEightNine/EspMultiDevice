#!/usr/bin/env python3
"""Minimal TLS WebSocket echo fixture for the ESP Media Stage 0 probe."""

import argparse
import base64
import hashlib
import socket
import ssl
import struct


WEBSOCKET_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_exact(stream, count):
    chunks = bytearray()
    while len(chunks) < count:
        chunk = stream.recv(count - len(chunks))
        if not chunk:
            raise ConnectionError("peer closed")
        chunks.extend(chunk)
    return bytes(chunks)


def read_request(stream):
    data = bytearray()
    while b"\r\n\r\n" not in data:
        if len(data) >= 8192:
            raise ValueError("HTTP request too large")
        data.extend(read_exact(stream, 1))
    head = data.decode("iso-8859-1").split("\r\n")
    method, path, _ = head[0].split(" ", 2)
    headers = {}
    for line in head[1:]:
        if not line:
            continue
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    return method, path, headers


def send_frame(stream, opcode, payload=b""):
    first = 0x80 | opcode
    length = len(payload)
    if length < 126:
        header = bytes((first, length))
    elif length <= 0xFFFF:
        header = bytes((first, 126)) + struct.pack("!H", length)
    else:
        header = bytes((first, 127)) + struct.pack("!Q", length)
    stream.sendall(header + payload)


def read_frame(stream):
    first, second = read_exact(stream, 2)
    if not first & 0x80:
        raise ValueError(f"fragmented frames are not supported by this fixture (first=0x{first:02x})")
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", read_exact(stream, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", read_exact(stream, 8))[0]
    if length > 4096:
        raise ValueError("frame exceeds fixture limit")
    if not masked:
        raise ValueError("client frame is not masked")
    mask = read_exact(stream, 4)
    payload = bytearray(read_exact(stream, length))
    for index in range(length):
        payload[index] ^= mask[index % 4]
    return opcode, bytes(payload)


def handle_client(stream):
    method, path, headers = read_request(stream)
    print(f"request method={method} path={path} header={headers.get('x-esp-media-probe')}", flush=True)
    if method != "GET" or path != "/echo":
        stream.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        return
    if headers.get("x-esp-media-probe") != "stage0":
        stream.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
        return
    key = headers.get("sec-websocket-key")
    if not key:
        stream.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        return
    accept = base64.b64encode(hashlib.sha1(key.encode("ascii") + WEBSOCKET_GUID).digest())
    response = [
        b"HTTP/1.1 101 Switching Protocols",
        b"Upgrade: websocket",
        b"Connection: Upgrade",
        b"Sec-WebSocket-Accept: " + accept,
    ]
    protocol = headers.get("sec-websocket-protocol")
    if protocol:
        response.append(b"Sec-WebSocket-Protocol: " + protocol.encode("ascii"))
    stream.sendall(b"\r\n".join(response) + b"\r\n\r\n")
    send_frame(stream, 0x1, b"header-ok")
    print("upgrade accepted; sent header-ok", flush=True)

    while True:
        opcode, payload = read_frame(stream)
        print(f"frame opcode={opcode} bytes={len(payload)}", flush=True)
        if opcode == 0x8:
            send_frame(stream, 0x8, payload[:125])
            return
        if opcode == 0x9:
            send_frame(stream, 0xA, payload)
        elif opcode in (0x1, 0x2):
            send_frame(stream, opcode, payload)
        else:
            raise ValueError(f"unsupported opcode {opcode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    with socket.create_server((args.host, args.port), reuse_port=False) as listener:
        listener.settimeout(1.0)
        print(f"WSS echo fixture listening on {args.host}:{args.port}", flush=True)
        while True:
            try:
                raw, address = listener.accept()
            except socket.timeout:
                continue
            try:
                with context.wrap_socket(raw, server_side=True) as stream:
                    stream.settimeout(20)
                    handle_client(stream)
            except (ConnectionError, OSError, ssl.SSLError, ValueError) as error:
                print(f"connection {address} ended: {error}", flush=True)
            finally:
                raw.close()


if __name__ == "__main__":
    main()
