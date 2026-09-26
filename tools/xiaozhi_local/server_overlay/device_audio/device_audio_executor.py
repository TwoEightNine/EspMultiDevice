"""Authenticated custom RPC tools for ESP Media's normal SD audio library."""

import asyncio
import json
from typing import Any, Dict

from plugins_func.register import Action, ActionResponse
from ..base import ToolDefinition, ToolExecutor, ToolType


class DeviceAudioRpc:
    """One correlated device command at a time for one WSS connection."""

    TIMEOUT_SECONDS = 15

    def __init__(self, conn):
        self.conn = conn
        self._next_request_id = 1
        self._pending_id = None
        self._pending_future = None
        self._lock = asyncio.Lock()

    async def call(self, command: str, arguments: Dict[str, Any]) -> Dict[str, Any]:
        if not isinstance(getattr(self.conn, "features", None), dict) or not self.conn.features.get("device_tools"):
            raise RuntimeError("device_tools_unavailable")
        async with self._lock:
            if self._pending_future is not None:
                raise RuntimeError("device_tools_busy")
            request_id = self._next_request_id
            self._next_request_id = 1 if request_id == 0xFFFFFFFF else request_id + 1
            loop = asyncio.get_running_loop()
            future = loop.create_future()
            self._pending_id = request_id
            self._pending_future = future
            payload = {
                "type": "device_command",
                "session_id": self.conn.session_id,
                "request_id": request_id,
                "command": command,
                "arguments": arguments,
            }
            try:
                await self.conn.websocket.send(json.dumps(payload, separators=(",", ":")))
                return await asyncio.wait_for(future, timeout=self.TIMEOUT_SECONDS)
            finally:
                self._pending_id = None
                self._pending_future = None

    def resolve(self, request_id: int, result: Dict[str, Any]) -> bool:
        if request_id != self._pending_id or self._pending_future is None or self._pending_future.done():
            return False
        self._pending_future.set_result(result)
        return True


class DeviceAudioExecutor(ToolExecutor):
    SEARCH_NAME = "search_audio"
    PLAY_NAME = "play_audio"

    def __init__(self, conn):
        self.conn = conn

    def _rpc(self, conn) -> DeviceAudioRpc:
        rpc = getattr(conn, "esp_media_device_audio_rpc", None)
        if rpc is None:
            rpc = DeviceAudioRpc(conn)
            conn.esp_media_device_audio_rpc = rpc
        return rpc

    async def execute(self, conn, tool_name: str, arguments: Dict[str, Any]) -> ActionResponse:
        if not self.has_tool(tool_name):
            return ActionResponse(action=Action.NOTFOUND, response="device audio tool unavailable")
        if not isinstance(arguments, dict):
            return ActionResponse(action=Action.ERROR, response="invalid device audio arguments")
        try:
            if tool_name == self.SEARCH_NAME:
                if set(arguments) != {"query"} or not isinstance(arguments["query"], str):
                    raise ValueError("invalid_arguments")
                query = arguments["query"].strip()
                if not query or len(query.encode("utf-8")) > 96:
                    raise ValueError("invalid_arguments")
                if query.casefold() in {"any", "any song", "any music", "a song", "random song", "random music"}:
                    query = "*"
                result = await self._rpc(conn).call(tool_name, {"query": query})
            else:
                selection_id = arguments.get("selection_id")
                if set(arguments) != {"selection_id"} or isinstance(selection_id, bool) or \
                        not isinstance(selection_id, int) or not 1 <= selection_id <= 5:
                    raise ValueError("invalid_arguments")
                result = await self._rpc(conn).call(tool_name, {"selection_id": selection_id})
                # The device begins safe session teardown immediately after
                # acknowledging the terminal action. Let the model see its
                # result but suppress a competing TTS response.
                conn.esp_media_terminal_device_action = True
            return ActionResponse(action=Action.REQLLM,
                                  result=json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        except ValueError as error:
            return ActionResponse(action=Action.ERROR, response=str(error))
        except asyncio.TimeoutError:
            return ActionResponse(action=Action.ERROR, response="device_tools_timeout")
        except RuntimeError as error:
            return ActionResponse(action=Action.ERROR, response=str(error))
        except Exception:
            return ActionResponse(action=Action.ERROR, response="device_tools_failed")

    def get_tools(self) -> Dict[str, ToolDefinition]:
        if not isinstance(getattr(self.conn, "features", None), dict) or not self.conn.features.get("device_tools"):
            return {}
        return {
            self.SEARCH_NAME: ToolDefinition(
                name=self.SEARCH_NAME,
                tool_type=ToolType.DEVICE_AUDIO,
                description={
                    "type": "function",
                    "function": {
                        "name": self.SEARCH_NAME,
                        "description": "Search normal SD-card WAV and MP3 filenames. For 'play any song', use query '*' and then play the first returned selection without clarification. For a named song, ask the user to clarify when several results are plausible.",
                        "parameters": {
                            "type": "object",
                            "properties": {"query": {"type": "string", "description": "Requested song title or filename words, or '*' to choose any local song."}},
                            "required": ["query"],
                            "additionalProperties": False,
                        },
                    },
                },
            ),
            self.PLAY_NAME: ToolDefinition(
                name=self.PLAY_NAME,
                tool_type=ToolType.DEVICE_AUDIO,
                description={
                    "type": "function",
                    "function": {
                        "name": self.PLAY_NAME,
                        "description": "Immediately play one opaque selection returned by search_audio. This is terminal: do not produce a spoken confirmation after calling it.",
                        "parameters": {
                            "type": "object",
                            "properties": {"selection_id": {"type": "integer", "minimum": 1, "maximum": 5}},
                            "required": ["selection_id"],
                            "additionalProperties": False,
                        },
                    },
                },
            ),
        }

    def has_tool(self, tool_name: str) -> bool:
        return tool_name in self.get_tools()
