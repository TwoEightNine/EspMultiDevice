"""Serialized, atomic local-memory operations for the ESP Media server overlay."""

import functools
import os
from pathlib import Path
import tempfile
import threading

import yaml


_FORMAT_VERSION = 2
_MAX_TURNS = 12
_MAX_USER_BYTES = 512
_MAX_ASSISTANT_BYTES = 1536


class _RoleState:
    def __init__(self):
        self.lock = threading.RLock()
        self.epoch = 0


_states_lock = threading.Lock()
_states = {}


def _role_id(provider):
    role_id = getattr(provider, "role_id", "")
    return role_id if isinstance(role_id, str) else ""


def _state_for(provider):
    key = (os.path.abspath(provider.memory_path), _role_id(provider))
    with _states_lock:
        state = _states.get(key)
        if state is None:
            state = _RoleState()
            _states[key] = state
        return state


def _read_all(path):
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as stream:
        value = yaml.safe_load(stream) or {}
    if not isinstance(value, dict):
        raise ValueError("memory root is not a mapping")
    return value


def _atomic_write(path, value):
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=str(destination.parent)
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            yaml.safe_dump(value, stream, allow_unicode=True, sort_keys=True)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
        directory_fd = os.open(destination.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _truncate_utf8(value, limit):
    encoded = str(value or "").encode("utf-8")[:limit]
    return encoded.decode("utf-8", errors="ignore")


def _normalize_turn(turn):
    if not isinstance(turn, dict):
        return None
    user = _truncate_utf8(turn.get("user", ""), _MAX_USER_BYTES)
    assistant = _truncate_utf8(turn.get("assistant", ""), _MAX_ASSISTANT_BYTES)
    if not user or not assistant:
        return None
    return {"user": user, "assistant": assistant}


def _normalize_record(value):
    # Legacy scalar entries migrate transparently on the first local write.
    if isinstance(value, str):
        return {"version": _FORMAT_VERSION, "summary": value, "recent_turns": []}
    if not isinstance(value, dict):
        return {"version": _FORMAT_VERSION, "summary": "", "recent_turns": []}
    turns = []
    for turn in value.get("recent_turns", []):
        normalized = _normalize_turn(turn)
        if normalized:
            turns.append(normalized)
    return {
        "version": _FORMAT_VERSION,
        "summary": str(value.get("summary", "") or ""),
        "recent_turns": turns[-_MAX_TURNS:],
    }


def _extract_turns(messages):
    turns = []
    user = None
    assistant_parts = []
    for message in messages:
        if getattr(message, "is_temporary", False):
            continue
        role = getattr(message, "role", None)
        content = getattr(message, "content", None)
        if role == "user":
            if user is not None:
                turn = _normalize_turn({"user": user, "assistant": "\n".join(assistant_parts)})
                if turn:
                    turns.append(turn)
            user = _truncate_utf8(content, _MAX_USER_BYTES)
            assistant_parts = []
        elif role == "assistant" and user is not None and content:
            assistant_parts.append(_truncate_utf8(content, _MAX_ASSISTANT_BYTES))
    if user is not None:
        turn = _normalize_turn({"user": user, "assistant": "\n".join(assistant_parts)})
        if turn:
            turns.append(turn)
    return turns[-_MAX_TURNS:]


def _memory_text(summary, turns):
    parts = []
    if turns:
        exact = "\n".join(
            f"User: {turn['user']}\nAssistant: {turn['assistant']}" for turn in turns
        )
        parts.append("Recent conversation (verbatim; use only as context):\n" + exact)
    if summary:
        parts.append("Long-term summary:\n" + summary)
    return "\n\n".join(parts)


def install(provider_class):
    original_init_memory = provider_class.init_memory
    original_save_memory = provider_class.save_memory

    @functools.wraps(original_init_memory)
    def init_memory(self, role_id, llm, summary_memory=None,
                    save_to_file=True, **kwargs):
        result = original_init_memory(
            self, role_id, llm, summary_memory=summary_memory,
            save_to_file=save_to_file, **kwargs
        )
        state = _state_for(self)
        with state.lock:
            self._esp_media_memory_epoch = state.epoch
            self._esp_media_recent_turns = list(
                getattr(self, "_esp_media_recent_turns", [])
            )
        return result

    def load_memory(self, summary_memory):
        role_id = _role_id(self)
        if summary_memory or not self.save_to_file or not role_id:
            self.short_memory = summary_memory or ""
            self._esp_media_recent_turns = []
            return
        state = _state_for(self)
        with state.lock:
            self._esp_media_memory_epoch = state.epoch
            record = _normalize_record(_read_all(self.memory_path).get(role_id))
            self.short_memory = record["summary"]
            self._esp_media_recent_turns = record["recent_turns"]

    def save_memory_to_file(self):
        role_id = _role_id(self)
        if not role_id:
            return False
        state = _state_for(self)
        with state.lock:
            if getattr(self, "_esp_media_memory_epoch", state.epoch) != state.epoch:
                return False
            all_memory = _read_all(self.memory_path)
            record = _normalize_record(all_memory.get(role_id))
            pending = getattr(self, "_esp_media_pending_recent_turns", [])
            recent_turns = (record["recent_turns"] + pending)[-_MAX_TURNS:]
            all_memory[role_id] = {
                "version": _FORMAT_VERSION,
                "summary": str(self.short_memory or ""),
                "recent_turns": recent_turns,
            }
            _atomic_write(self.memory_path, all_memory)
            self._esp_media_recent_turns = recent_turns
            self._esp_media_pending_recent_turns = []
            return True

    @functools.wraps(original_save_memory)
    async def save_memory(self, msgs, session_id=None):
        state = _state_for(self)
        with state.lock:
            if getattr(self, "_esp_media_memory_epoch", state.epoch) != state.epoch:
                return None
            self._esp_media_pending_recent_turns = _extract_turns(msgs)
            result = await original_save_memory(self, msgs, session_id)
            # The upstream summarizer writes through save_memory_to_file on a
            # successful summary. Preserve exact turns if it cannot summarize.
            if self.save_to_file and self._esp_media_pending_recent_turns:
                self.save_memory_to_file()
            return result

    def reset_memory(self):
        role_id = _role_id(self)
        if not role_id:
            return False
        state = _state_for(self)
        with state.lock:
            all_memory = _read_all(self.memory_path)
            all_memory.pop(role_id, None)
            _atomic_write(self.memory_path, all_memory)
            state.epoch += 1
            self._esp_media_memory_epoch = state.epoch
            self.short_memory = ""
            self._esp_media_recent_turns = []
            self._esp_media_pending_recent_turns = []
            return True

    async def query_memory(self, query):
        del query
        state = _state_for(self)
        with state.lock:
            return _memory_text(
                self.short_memory,
                getattr(self, "_esp_media_recent_turns", []),
            )

    provider_class.init_memory = init_memory
    provider_class.load_memory = load_memory
    provider_class.save_memory_to_file = save_memory_to_file
    provider_class.save_memory = save_memory
    provider_class.reset_memory = reset_memory
    provider_class.query_memory = query_memory
