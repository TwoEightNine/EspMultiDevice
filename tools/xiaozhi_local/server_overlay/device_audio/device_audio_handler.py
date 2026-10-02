"""Exact, command-specific ingress validation for private device results."""

from typing import Any, Dict
import re

from .device_audio_executor import QUERY_TOOLS, TERMINAL_TOOLS, _capability_version


ERROR_CODES = {
    "invalid_arguments", "storage_unavailable", "busy", "selection_unavailable",
    "protocol", "unavailable", "settings_failed",
}
COLORS = {"Red", "Green", "Blue", "Yellow", "Cyan", "Magenta"}
WIFI_STATES = {"disabled", "disconnected", "scanning", "connecting", "connected", "failed"}
LOCAL_TIME = re.compile(r"\d{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12]\d|3[01])T"
                        r"(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d\+04:00\Z")


def _integer(value: Any, minimum: int, maximum: int) -> bool:
    return type(value) is int and minimum <= value <= maximum


def _string(value: Any, maximum: int, allow_empty: bool = False) -> bool:
    return isinstance(value, str) and (allow_empty or bool(value)) and len(value.encode("utf-8")) <= maximum


def _matches(command: str, result: Dict[str, Any]) -> bool:
    if set(result) != {"matches"} or not isinstance(result["matches"], list) or len(result["matches"]) > 5:
        return False
    seen = set()
    for match in result["matches"]:
        keys = {"selection_id", "name", "kind"} if command == "search_files" else {"selection_id", "name"}
        if not isinstance(match, dict) or set(match) != keys or \
                not _integer(match["selection_id"], 1, 5) or \
                not _string(match["name"], 96) or match["selection_id"] in seen:
            return False
        if command == "search_files" and match["kind"] not in ("file", "folder"):
            return False
        seen.add(match["selection_id"])
    return True


def _valid_result(command: str, result: Any) -> bool:
    if not isinstance(result, dict):
        return False
    if command in QUERY_TOOLS - {"search_web"}:
        return _matches(command, result)
    if command in TERMINAL_TOOLS:
        return set(result) == {"accepted"} and result["accepted"] is True
    if command == "get_time":
        if set(result) != {"available", "local_iso8601"} or type(result["available"]) is not bool:
            return False
        value = result["local_iso8601"]
        return (isinstance(value, str) and bool(LOCAL_TIME.fullmatch(value))) \
            if result["available"] else value is None
    if command == "get_wifi_status":
        return set(result) == {"state"} and isinstance(result["state"], str) and result["state"] in WIFI_STATES
    if command == "get_wifi_signal":
        return set(result) == {"connected", "rssi_dbm"} and \
            type(result["connected"]) is bool and \
            (_integer(result["rssi_dbm"], -127, 0) if result["connected"] else result["rssi_dbm"] is None)
    if command == "get_sd_status":
        return set(result) == {"present", "mounted", "owner"} and \
            type(result["present"]) is bool and type(result["mounted"]) is bool and \
            isinstance(result["owner"], str) and result["owner"] in ("none", "device", "usb_host")
    if command == "get_mic_status":
        return set(result) == {"ready", "powered", "power_sense_present"} and \
            all(type(value) is bool for value in result.values())
    if command == "get_battery_status":
        return set(result) == {"voltage_mv"} and \
            _integer(result["voltage_mv"], 0, 6000)
    if command in ("get_volume", "set_volume"):
        return set(result) == {"level"} and _integer(result["level"], 0, 10)
    if command in ("get_led_status", "set_led_enabled", "set_led_color"):
        return set(result) == {"available", "enabled", "color"} and \
            type(result["available"]) is bool and type(result["enabled"]) is bool and \
            isinstance(result["color"], str) and result["color"] in COLORS
    if command == "show_file_info":
        return set(result) == {"name", "type", "size_bytes", "modified_unix"} and \
            _string(result["name"], 96) and _string(result["type"], 19) and \
            _integer(result["size_bytes"], 0, 0xFFFFFFFF) and \
            (result["modified_unix"] is None or _integer(result["modified_unix"], 1, 0xFFFFFFFF))
    return False


def _valid_v1_result(result: Any) -> bool:
    if not isinstance(result, dict) or set(result) - {"matches", "accepted"}:
        return False
    matches = result.get("matches", [])
    if not isinstance(matches, list) or len(matches) > 5 or \
            type(result.get("accepted", False)) is not bool:
        return False
    return _matches("search_audio", {"matches": matches})


async def handle_device_result(conn, message: Dict[str, Any]) -> None:
    """Resolve only the active request; never log arguments or result payloads."""
    version = _capability_version(conn)
    if not version or not isinstance(message, dict) or \
            message.get("type") != "device_result" or \
            message.get("session_id") != conn.session_id or \
            not _integer(message.get("request_id"), 1, 0xFFFFFFFF):
        return
    rpc = getattr(conn, "esp_media_device_audio_rpc", None)
    command = rpc.pending_command if rpc else None
    if command is None or type(message.get("ok")) is not bool:
        return
    if message["ok"]:
        valid = set(message) == {"type", "session_id", "request_id", "ok", "result"} and \
            (_valid_v1_result(message.get("result")) if version == 1
             else _valid_result(command, message.get("result")))
    else:
        valid = set(message) == {"type", "session_id", "request_id", "ok", "error"} and \
            isinstance(message.get("error"), dict) and set(message["error"]) == {"code"} and \
            message["error"]["code"] in ERROR_CODES
    if valid:
        rpc.resolve(message["request_id"], message)
