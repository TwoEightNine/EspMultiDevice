import asyncio
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import yaml

from core.handle.textHandler.conversationMessageHandler import ConversationMessageHandler
from core.providers.memory.mem_local_short import esp_media_memory_overlay as memory_overlay


class DummyProvider:
    def __init__(self, path, role, initial=""):
        self.memory_path = path
        self.role_id = role
        self.short_memory = initial
        self.save_to_file = True
        self.llm = object()
        self.init_memory(role, self.llm)

    def init_memory(self, role_id, llm, summary_memory=None,
                    save_to_file=True, **kwargs):
        del kwargs
        self.role_id = role_id
        self.llm = llm
        self.save_to_file = save_to_file
        self.load_memory(summary_memory)

    def load_memory(self, summary_memory):
        self.short_memory = summary_memory or ""

    def save_memory_to_file(self):
        raise AssertionError("overlay did not replace non-atomic save")

    async def save_memory(self, msgs, session_id=None):
        del msgs, session_id
        self.short_memory = "stale-summary"
        self.save_memory_to_file()
        return self.short_memory

    async def query_memory(self, query):
        del query
        return self.short_memory


memory_overlay.install(DummyProvider)


class MemoryOverlayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = str(Path(self.temporary.name) / ".memory.yaml")
        with open(self.path, "w", encoding="utf-8") as stream:
            yaml.safe_dump({"device-a": "alpha", "device-b": "bravo"}, stream)

    def tearDown(self):
        self.temporary.cleanup()

    def test_targeted_atomic_idempotent_reset(self):
        provider = DummyProvider(self.path, "device-a")
        with mock.patch.object(memory_overlay.os, "replace",
                               wraps=os.replace) as replaced:
            self.assertTrue(provider.reset_memory())
            self.assertTrue(replaced.called)
        with open(self.path, "r", encoding="utf-8") as stream:
            stored = yaml.safe_load(stream)
        self.assertNotIn("device-a", stored)
        self.assertEqual(stored["device-b"], "bravo")
        self.assertEqual(provider.short_memory, "")
        self.assertTrue(provider.reset_memory())

    async def test_close_reopen_generation_cannot_resurrect_reset(self):
        closing = DummyProvider(self.path, "device-a")
        resetting = DummyProvider(self.path, "device-a")
        resetting.reset_memory()
        await closing.save_memory([object(), object()], "old-session")
        reopened = DummyProvider(self.path, "device-a")
        self.assertEqual(reopened.short_memory, "")
        with open(self.path, "r", encoding="utf-8") as stream:
            self.assertNotIn("device-a", yaml.safe_load(stream))


class FakeLogger:
    def info(self, *_args, **_kwargs):
        pass

    def error(self, *_args, **_kwargs):
        pass

    def bind(self, **_kwargs):
        return self


class FakeWebSocket:
    def __init__(self):
        self.messages = []

    async def send(self, value):
        self.messages.append(json.loads(value))


class FakeMemory:
    def __init__(self, fail=False):
        self.fail = fail
        self.resets = 0

    def reset_memory(self):
        self.resets += 1
        if self.fail:
            raise OSError("test failure")


class FakeConnection:
    def __init__(self, memory):
        self.memory = memory
        self.websocket = FakeWebSocket()
        self.logger = FakeLogger()
        self.session_id = "private-session"
        self.prompt = "system prompt"
        self.dialogue = object()
        self.close_after_chat = False
        self.client_abort = False

    def clear_queues(self):
        pass

    def clearSpeakStatus(self):
        pass

    def change_system_prompt(self, prompt):
        self.prompt = prompt
        self.dialogue.update_system_message(prompt)


class ConversationHandlerTests(unittest.IsolatedAsyncioTestCase):
    async def test_live_dialogue_and_duplicate_reset(self):
        connection = FakeConnection(FakeMemory())
        handler = ConversationMessageHandler()
        request = {"action": "reset", "request_id": 7}
        await handler.handle(connection, request)
        await handler.handle(connection, request)
        acknowledgements = [message for message in connection.websocket.messages
                            if message.get("type") == "conversation"]
        self.assertEqual(connection.memory.resets, 2)
        self.assertEqual([message["result"] for message in acknowledgements],
                         ["ok", "ok"])
        self.assertEqual(connection.dialogue.get_llm_dialogue()[0]["content"],
                         "system prompt")

    async def test_malformed_request_does_not_reset(self):
        connection = FakeConnection(FakeMemory())
        await ConversationMessageHandler().handle(
            connection, {"action": "reset", "request_id": True}
        )
        self.assertEqual(connection.memory.resets, 0)
        self.assertEqual(connection.websocket.messages[-1]["result"], "error")
        self.assertEqual(connection.websocket.messages[-1]["category"],
                         "invalid_request")

    async def test_reset_failure_never_sends_false_success(self):
        connection = FakeConnection(FakeMemory(fail=True))
        await ConversationMessageHandler().handle(
            connection, {"action": "reset", "request_id": 9}
        )
        acknowledgements = [message for message in connection.websocket.messages
                            if message.get("type") == "conversation"]
        self.assertEqual(len(acknowledgements), 1)
        self.assertEqual(acknowledgements[0]["result"], "error")
        self.assertEqual(acknowledgements[0]["category"], "reset_failed")


if __name__ == "__main__":
    unittest.main()
