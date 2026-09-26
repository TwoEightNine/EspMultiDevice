import asyncio
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import yaml

# Tool executors initialize the upstream logger at import time. The production
# container mounts a private data/.config.yaml; image-build tests deliberately
# use the tracked non-secret baseline only to satisfy that import contract.
_test_config = Path("data/.config.yaml")
if not _test_config.exists():
    _test_config.parent.mkdir(parents=True, exist_ok=True)
    _test_config.write_text(Path("config.yaml").read_text(encoding="utf-8"), encoding="utf-8")

from core.handle.textHandler.conversationMessageHandler import ConversationMessageHandler
from core.providers.memory.mem_local_short import esp_media_memory_overlay as memory_overlay
from core.providers.tools.device_audio import DeviceAudioExecutor, handle_device_result
from core.providers.tools.base import ToolType
from core.providers.tools.unified_tool_handler import UnifiedToolHandler
from core.providers.tools.unified_tool_manager import ToolManager
from plugins_func.register import Action, ActionResponse


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

    def debug(self, *_args, **_kwargs):
        pass

    def warning(self, *_args, **_kwargs):
        pass

    def bind(self, **_kwargs):
        return self


class FakeWebSocket:
    def __init__(self):
        self.messages = []

    async def send(self, value):
        self.messages.append(json.loads(value))


class FakeDeviceConnection:
    def __init__(self, enabled=True):
        self.features = {"device_tools": enabled}
        self.session_id = "private-session"
        self.websocket = FakeWebSocket()
        self.esp_media_terminal_device_action = False


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


class DeviceAudioRpcTests(unittest.IsolatedAsyncioTestCase):
    async def test_any_song_query_uses_bounded_library_pick(self):
        conn = FakeDeviceConnection()
        task = asyncio.create_task(DeviceAudioExecutor(conn).execute(
            conn, "search_audio", {"query": "any song"}
        ))
        await asyncio.sleep(0)
        self.assertEqual(conn.websocket.messages[-1]["arguments"], {"query": "*"})
        await handle_device_result(conn, {
            "type": "device_result", "session_id": "private-session", "request_id": 1,
            "ok": True, "result": {"matches": [{"selection_id": 1, "name": "First Song"}]},
        })
        self.assertEqual((await task).action, Action.REQLLM)

    async def test_search_forwards_strict_command_and_returns_model_result(self):
        conn = FakeDeviceConnection()
        executor = DeviceAudioExecutor(conn)
        self.assertEqual(set(executor.get_tools()), {"search_audio", "play_audio"})
        search_schema = executor.get_tools()["search_audio"].description["function"]
        self.assertIn("query '*'", search_schema["description"])
        task = asyncio.create_task(executor.execute(conn, "search_audio", {"query": "Night Drive"}))
        await asyncio.sleep(0)
        self.assertEqual(conn.websocket.messages[-1], {
            "type": "device_command", "session_id": "private-session", "request_id": 1,
            "command": "search_audio", "arguments": {"query": "Night Drive"},
        })
        await handle_device_result(conn, {
            "type": "device_result", "session_id": "private-session", "request_id": 1,
            "ok": True, "result": {"matches": [{"selection_id": 1, "name": "Night Drive"}]},
        })
        response = await task
        self.assertEqual(response.action, Action.REQLLM)
        self.assertIn("Night Drive", response.result)

    async def test_malformed_or_mismatched_result_cannot_resolve_command(self):
        conn = FakeDeviceConnection()
        executor = DeviceAudioExecutor(conn)
        task = asyncio.create_task(executor.execute(conn, "search_audio", {"query": "song"}))
        await asyncio.sleep(0)
        await handle_device_result(conn, {
            "type": "device_result", "session_id": "other", "request_id": 1,
            "ok": True, "result": {"matches": []},
        })
        await handle_device_result(conn, {
            "type": "device_result", "session_id": "private-session", "request_id": 1,
            "ok": True, "result": {"matches": [], "path": "/private.mp3"},
        })
        self.assertFalse(task.done())
        await handle_device_result(conn, {
            "type": "device_result", "session_id": "private-session", "request_id": 1,
            "ok": False, "error": {"code": "selection_unavailable"},
        })
        response = await task
        self.assertEqual(response.action, Action.REQLLM)
        self.assertIn("selection_unavailable", response.result)

    async def test_playback_is_terminal_and_unavailable_without_capability(self):
        conn = FakeDeviceConnection()
        executor = DeviceAudioExecutor(conn)
        task = asyncio.create_task(executor.execute(conn, "play_audio", {"selection_id": 2}))
        await asyncio.sleep(0)
        await handle_device_result(conn, {
            "type": "device_result", "session_id": "private-session", "request_id": 1,
            "ok": True, "result": {"matches": [], "accepted": True},
        })
        self.assertEqual((await task).action, Action.REQLLM)
        self.assertTrue(conn.esp_media_terminal_device_action)
        disabled = FakeDeviceConnection(enabled=False)
        self.assertEqual(DeviceAudioExecutor(disabled).get_tools(), {})


class DeviceAudioToolIsolationTests(unittest.TestCase):
    class StaticExecutor:
        def __init__(self, tools):
            self.tools = tools

        def get_tools(self):
            return self.tools

    def test_device_session_excludes_legacy_play_music(self):
        manager = ToolManager(FakeDeviceConnection(enabled=True))
        manager.register_executor(ToolType.SERVER_PLUGIN, self.StaticExecutor({
            "play_music": object(), "get_weather": object(),
        }))
        self.assertEqual(set(manager.get_all_tools()), {"get_weather"})

    def test_non_device_session_retains_legacy_play_music(self):
        manager = ToolManager(FakeDeviceConnection(enabled=False))
        manager.register_executor(ToolType.SERVER_PLUGIN, self.StaticExecutor({
            "play_music": object(), "get_weather": object(),
        }))
        self.assertEqual(set(manager.get_all_tools()), {"play_music", "get_weather"})


class DeviceAudioDisplayTests(unittest.IsolatedAsyncioTestCase):
    async def test_audio_tool_does_not_send_fake_stt_caption(self):
        class StubManager:
            async def execute_tool(self, *_args):
                return ActionResponse(action=Action.REQLLM, result="{}")

        handler = UnifiedToolHandler.__new__(UnifiedToolHandler)
        handler.conn = FakeDeviceConnection()
        handler.logger = FakeLogger()
        handler.tool_manager = StubManager()
        with mock.patch("core.providers.tools.unified_tool_handler.send_display_message",
                        new_callable=mock.AsyncMock) as display:
            result = await handler.handle_llm_function_call(handler.conn, {
                "name": "search_audio", "arguments": '{"query":"song"}',
            })
        self.assertEqual(result.action, Action.REQLLM)
        display.assert_not_awaited()


if __name__ == "__main__":
    unittest.main()
