"""Authenticated ESP Media device tools, gated by negotiated capability version."""

import asyncio
import json
from typing import Any, Dict

from plugins_func.register import Action, ActionResponse
from ..base import ToolDefinition, ToolExecutor, ToolType


QUERY_TOOLS = {"search_audio", "search_images", "search_videos", "search_files", "search_web"}
SELECT_TOOLS = {"play_audio", "open_image", "play_video", "show_file_info", "open_folder"}
SETTING_TOOLS = {"set_volume", "set_led_enabled", "set_led_color"}
STATUS_TOOLS = {
    "get_time", "get_wifi_status", "get_wifi_signal", "get_sd_status",
    "get_mic_status", "get_battery_status", "get_volume", "get_led_status",
}
TERMINAL_TOOLS = {
    "play_audio", "open_battery_details", "open_image", "play_video",
    "open_folder", "search_web",
}
DEVICE_TOOL_NAMES = frozenset(QUERY_TOOLS | SELECT_TOOLS | SETTING_TOOLS |
                              STATUS_TOOLS | {"open_battery_details"})
V1_TOOLS = frozenset({"search_audio", "play_audio"})
COLORS = ("red", "green", "blue", "yellow", "cyan", "magenta")


def _capability_version(conn) -> int:
    features = getattr(conn, "features", None)
    if not isinstance(features, dict) or features.get("device_tools") is not True:
        return 0
    version = features.get("device_tools_version", 1)
    return version if type(version) is int and version >= 1 else 0


def _parameters(name: str) -> Dict[str, Any]:
    if name in QUERY_TOOLS:
        properties = {"query": {"type": "string", "minLength": 1, "maxLength": 96}}
        required = ["query"]
    elif name in SELECT_TOOLS:
        properties = {"selection_id": {"type": "integer", "minimum": 1, "maximum": 5}}
        required = ["selection_id"]
    elif name == "set_volume":
        properties = {"level": {"type": "integer", "minimum": 0, "maximum": 10}}
        required = ["level"]
    elif name == "set_led_enabled":
        properties = {"enabled": {"type": "boolean"}}
        required = ["enabled"]
    elif name == "set_led_color":
        properties = {"color": {"type": "string", "enum": list(COLORS)}}
        required = ["color"]
    else:
        properties = {}
        required = []
    return {"type": "object", "properties": properties,
            "required": required, "additionalProperties": False}


DESCRIPTIONS = {
    "search_audio": "Search normal SD WAV/MP3 filenames. For 'play any song', use query '*' and play the first result; clarify ambiguous named matches.",
    "play_audio": "Play an opaque selection from the latest search_audio. Terminal action; do not speak confirmation.",
    "get_time": "Read local device date and time, or unavailable.",
    "get_wifi_status": "Read the device Wi-Fi state without disclosing SSID.",
    "get_wifi_signal": "Read Wi-Fi signal strength in dBm when connected.",
    "get_sd_status": "Read SD-card presence, mount state, and ownership.",
    "get_mic_status": "Read microphone readiness and power-switch state.",
    "get_battery_status": "Read latest cell voltage in millivolts and voltage-derived percentage; do not infer charging.",
    "get_volume": "Read saved speaker volume from 0 to 10.",
    "get_led_status": "Read built-in LED availability, enabled state, and selected color.",
    "open_battery_details": "Open the Battery Details screen. Terminal action; do not speak confirmation.",
    "set_volume": "Set and persist speaker volume from 0 to 10.",
    "set_led_enabled": "Enable or disable the built-in LED and persist the choice.",
    "set_led_color": "Set one of six LED colors without changing whether it is enabled.",
    "search_images": "Search visible normal-SD image filenames; use '*' for a bounded list.",
    "open_image": "Open an opaque selection from the latest search_images. Terminal action.",
    "search_videos": "Search visible normal-SD video filenames; use '*' for a bounded list.",
    "play_video": "Play an opaque selection from the latest search_videos. Terminal action.",
    "search_files": "Search visible normal-SD files and folders, including unsupported file types; use '*' for a bounded list.",
    "show_file_info": "Show type, size, and modification time of a file selected by search_files; never reads its contents.",
    "open_folder": "Open a folder selected by search_files in Files. Terminal action.",
    "search_web": "Open DuckDuckGo Lite search results in the device Browser. Pass search words only, never a URL. Terminal action.",
}


class DeviceAudioRpc:
    """One correlated command per authenticated WSS connection."""

    TIMEOUT_SECONDS = 15

    def __init__(self, conn):
        self.conn = conn
        self._next_request_id = 1
        self._pending_id = None
        self._pending_command = None
        self._pending_future = None
        self._lock = asyncio.Lock()

    @property
    def pending_command(self):
        return self._pending_command

    async def call(self, command: str, arguments: Dict[str, Any]) -> Dict[str, Any]:
        if not _capability_version(self.conn):
            raise RuntimeError("device_tools_unavailable")
        async with self._lock:
            if self._next_request_id > 0xFFFFFFFF:
                raise RuntimeError("device_request_ids_exhausted")
            request_id = self._next_request_id
            self._next_request_id = request_id + 1
            future = asyncio.get_running_loop().create_future()
            self._pending_id = request_id
            self._pending_command = command
            self._pending_future = future
            payload = {"type": "device_command", "session_id": self.conn.session_id,
                       "request_id": request_id, "command": command, "arguments": arguments}
            try:
                await self.conn.websocket.send(json.dumps(payload, separators=(",", ":")))
                return await asyncio.wait_for(future, timeout=self.TIMEOUT_SECONDS)
            finally:
                self._pending_id = None
                self._pending_command = None
                self._pending_future = None

    def resolve(self, request_id: int, result: Dict[str, Any]) -> bool:
        if request_id != self._pending_id or self._pending_future is None or self._pending_future.done():
            return False
        self._pending_future.set_result(result)
        return True


class DeviceAudioExecutor(ToolExecutor):
    """Legacy class name retained for the checksum-guarded upstream overlay."""

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
            return ActionResponse(action=Action.NOTFOUND, response="device tool unavailable")
        if not isinstance(arguments, dict):
            return ActionResponse(action=Action.ERROR, response="invalid_arguments")
        try:
            if tool_name in QUERY_TOOLS:
                if set(arguments) != {"query"} or not isinstance(arguments["query"], str):
                    raise ValueError("invalid_arguments")
                query = arguments["query"].strip()
                if not query or len(query.encode("utf-8")) > 96:
                    raise ValueError("invalid_arguments")
                if tool_name == "search_audio" and query.casefold() in {
                    "any", "any song", "any music", "a song", "random song", "random music"
                }:
                    query = "*"
                arguments = {"query": query}
            elif tool_name in SELECT_TOOLS:
                value = arguments.get("selection_id")
                if set(arguments) != {"selection_id"} or type(value) is not int or not 1 <= value <= 5:
                    raise ValueError("invalid_arguments")
            elif tool_name == "set_volume":
                value = arguments.get("level")
                if set(arguments) != {"level"} or type(value) is not int or not 0 <= value <= 10:
                    raise ValueError("invalid_arguments")
            elif tool_name == "set_led_enabled":
                if set(arguments) != {"enabled"} or type(arguments["enabled"]) is not bool:
                    raise ValueError("invalid_arguments")
            elif tool_name == "set_led_color":
                if set(arguments) != {"color"} or arguments["color"] not in COLORS:
                    raise ValueError("invalid_arguments")
            elif arguments:
                raise ValueError("invalid_arguments")
            result = await self._rpc(conn).call(tool_name, arguments)
            if tool_name in TERMINAL_TOOLS and result.get("ok") is True and \
                    result.get("result", {}).get("accepted") is True:
                conn.esp_media_terminal_device_action = True
            return ActionResponse(action=Action.REQLLM,
                                  result=json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        except ValueError:
            return ActionResponse(action=Action.ERROR, response="invalid_arguments")
        except asyncio.TimeoutError:
            return ActionResponse(action=Action.ERROR, response="device_tools_timeout")
        except RuntimeError as error:
            return ActionResponse(action=Action.ERROR, response=str(error))
        except Exception:
            return ActionResponse(action=Action.ERROR, response="device_tools_failed")

    def get_tools(self) -> Dict[str, ToolDefinition]:
        version = _capability_version(self.conn)
        names = V1_TOOLS if version == 1 else DEVICE_TOOL_NAMES if version >= 2 else ()
        return {name: ToolDefinition(
            name=name, tool_type=ToolType.DEVICE_AUDIO,
            description={"type": "function", "function": {
                "name": name, "description": DESCRIPTIONS[name], "parameters": _parameters(name)
            }}) for name in sorted(names)}

    def has_tool(self, tool_name: str) -> bool:
        return tool_name in self.get_tools()
