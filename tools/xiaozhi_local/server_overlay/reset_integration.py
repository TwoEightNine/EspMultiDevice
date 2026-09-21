#!/usr/bin/env python3
"""Exercise authenticated reset without printing bootstrap credentials."""

import asyncio
import json
from pathlib import Path
import sys

import websockets
import yaml


async def run(bootstrap_path, device_id, client_id):
    with open(bootstrap_path, "r", encoding="utf-8") as stream:
        bootstrap = json.load(stream)
    websocket = bootstrap["websocket"]
    token = websocket["token"]
    authorization = token if " " in token else f"Bearer {token}"
    headers = {
        "Authorization": authorization,
        "Protocol-Version": str(websocket.get("version", 1)),
        "Device-Id": device_id,
        "Client-Id": client_id,
    }
    connect_kwargs = {"ping_interval": None}
    # websockets renamed this keyword in version 14.
    if int(websockets.__version__.split(".")[0]) >= 14:
        connect_kwargs["additional_headers"] = headers
    else:
        connect_kwargs["extra_headers"] = headers
    async with websockets.connect(
        "ws://127.0.0.1:8000/xiaozhi/v1/", **connect_kwargs
    ) as connection:
        await connection.send(json.dumps({
            "type": "hello",
            "version": websocket.get("version", 1),
            "transport": "websocket",
            "audio_params": {
                "format": "opus", "sample_rate": 16000,
                "channels": 1, "frame_duration": 60,
            },
            "features": {"mcp": False},
        }))
        hello = json.loads(await asyncio.wait_for(connection.recv(), 10))
        if hello.get("type") != "hello":
            raise RuntimeError("server hello missing")
        await connection.send('{"type":"ping"}')
        pong = json.loads(await asyncio.wait_for(connection.recv(), 10))
        if pong.get("type") != "pong":
            raise RuntimeError("server pong missing")
        request_id = 184467
        request = {
            "type": "conversation", "action": "reset",
            "request_id": request_id,
        }
        await connection.send(json.dumps(request, separators=(",", ":")))
        while True:
            reply = json.loads(await asyncio.wait_for(connection.recv(), 10))
            if reply.get("type") == "conversation":
                break
        if (reply.get("action") != "reset" or
                reply.get("request_id") != request_id or
                reply.get("result") != "ok"):
            raise RuntimeError("reset was not acknowledged")

    memory_path = Path("data/.memory.yaml")
    if memory_path.exists():
        with memory_path.open("r", encoding="utf-8") as stream:
            all_memory = yaml.safe_load(stream) or {}
        if device_id in all_memory:
            raise RuntimeError("isolated test memory was not deleted")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: reset_integration.py BOOTSTRAP DEVICE_ID CLIENT_ID")
    asyncio.run(run(sys.argv[1], sys.argv[2], sys.argv[3]))
    print("PASS authenticated heartbeat and conversation reset")
