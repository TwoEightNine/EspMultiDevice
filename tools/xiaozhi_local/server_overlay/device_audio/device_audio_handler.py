"""Strict ingress validation for ESP Media custom device-tool results."""

from typing import Any, Dict


_ERROR_CODES = {
    "invalid_arguments", "storage_unavailable", "busy", "selection_unavailable", "protocol",
}


def _positive_uint(value: Any, maximum: int = 0xFFFFFFFF) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= maximum


def _valid_result(message: Dict[str, Any]) -> bool:
    if set(message) != {"type", "session_id", "request_id", "ok", "result"}:
        return False
    result = message["result"]
    if not isinstance(result, dict) or set(result) - {"matches", "accepted"}:
        return False
    matches = result.get("matches", [])
    if not isinstance(matches, list) or len(matches) > 5:
        return False
    for match in matches:
        if not isinstance(match, dict) or set(match) != {"selection_id", "name"}:
            return False
        if not _positive_uint(match["selection_id"], 5) or not isinstance(match["name"], str):
            return False
        if not match["name"] or len(match["name"].encode("utf-8")) > 96:
            return False
    return isinstance(result.get("accepted", False), bool)


def _valid_error(message: Dict[str, Any]) -> bool:
    return set(message) == {"type", "session_id", "request_id", "ok", "error"} and \
        isinstance(message["error"], dict) and set(message["error"]) == {"code"} and \
        message["error"].get("code") in _ERROR_CODES


async def handle_device_result(conn, message: Dict[str, Any]) -> None:
    """Resolve only the current authenticated command; never log filenames."""
    if not isinstance(getattr(conn, "features", None), dict) or not conn.features.get("device_tools"):
        return
    if not isinstance(message, dict) or message.get("type") != "device_result" or \
            message.get("session_id") != conn.session_id or not _positive_uint(message.get("request_id")):
        return
    ok = message.get("ok")
    if not isinstance(ok, bool) or (ok and not _valid_result(message)) or (not ok and not _valid_error(message)):
        return
    rpc = getattr(conn, "esp_media_device_audio_rpc", None)
    if rpc is not None:
        rpc.resolve(message["request_id"], message)
