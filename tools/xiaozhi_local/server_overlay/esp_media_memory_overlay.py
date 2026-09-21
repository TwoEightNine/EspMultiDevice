"""Serialized, atomic local-memory operations for the ESP Media server overlay."""

import asyncio
import functools
import os
from pathlib import Path
import tempfile
import threading
import yaml


class _RoleState:
    def __init__(self):
        self.lock = threading.RLock()
        self.epoch = 0


_states_lock = threading.Lock()
_states = {}


def _state_for(provider):
    key = (os.path.abspath(provider.memory_path), provider.role_id)
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
        return result

    def load_memory(self, summary_memory):
        state = _state_for(self)
        with state.lock:
            self._esp_media_memory_epoch = state.epoch
            if summary_memory or not self.save_to_file:
                self.short_memory = summary_memory or ""
                return
            all_memory = _read_all(self.memory_path)
            self.short_memory = all_memory.get(self.role_id, "")

    def save_memory_to_file(self):
        state = _state_for(self)
        with state.lock:
            if getattr(self, "_esp_media_memory_epoch", state.epoch) != state.epoch:
                return False
            all_memory = _read_all(self.memory_path)
            all_memory[self.role_id] = self.short_memory
            _atomic_write(self.memory_path, all_memory)
            return True

    @functools.wraps(original_save_memory)
    async def save_memory(self, msgs, session_id=None):
        state = _state_for(self)
        with state.lock:
            if getattr(self, "_esp_media_memory_epoch", state.epoch) != state.epoch:
                return None
            return await original_save_memory(self, msgs, session_id)

    def reset_memory(self):
        state = _state_for(self)
        with state.lock:
            all_memory = _read_all(self.memory_path)
            all_memory.pop(self.role_id, None)
            _atomic_write(self.memory_path, all_memory)
            state.epoch += 1
            self._esp_media_memory_epoch = state.epoch
            self.short_memory = ""
            return True

    async def query_memory(self, query):
        del query
        state = _state_for(self)
        with state.lock:
            return self.short_memory

    provider_class.init_memory = init_memory
    provider_class.load_memory = load_memory
    provider_class.save_memory_to_file = save_memory_to_file
    provider_class.save_memory = save_memory
    provider_class.reset_memory = reset_memory
    provider_class.query_memory = query_memory

