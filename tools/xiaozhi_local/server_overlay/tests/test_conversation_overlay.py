import asyncio
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

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
from core.providers.llm.esp_media_openai_responses import LLMProvider as ResponsesProvider
from core.providers.tts.esp_media_azure_multilingual import TTSProvider as AzureTtsProvider
from core.providers.tools.device_audio import DeviceAudioExecutor, handle_device_result
from core.providers.tools.device_audio.device_audio_executor import DEVICE_TOOL_NAMES
from core.providers.tools.device_audio.device_audio_handler import _valid_result
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

    async def test_legacy_summary_migrates_and_retains_last_twelve_complete_turns(self):
        provider = DummyProvider(self.path, "device-a")
        messages = []
        for index in range(13):
            messages.extend([
                SimpleNamespace(role="user", content=f"user-{index}", is_temporary=False),
                SimpleNamespace(role="assistant", content=f"assistant-{index}", is_temporary=False),
            ])
        await provider.save_memory(messages, "session")
        with open(self.path, "r", encoding="utf-8") as stream:
            record = yaml.safe_load(stream)["device-a"]
        self.assertEqual(record["version"], 2)
        self.assertEqual(record["summary"], "stale-summary")
        self.assertEqual(len(record["recent_turns"]), 12)
        self.assertEqual(record["recent_turns"][0]["user"], "user-1")
        self.assertEqual(record["recent_turns"][-1]["assistant"], "assistant-12")
        self.assertIn("Recent conversation", await provider.query_memory("ignored"))

    async def test_recent_turns_are_utf8_safe_and_ignore_incomplete_turns(self):
        provider = DummyProvider(self.path, "device-a")
        messages = [
            SimpleNamespace(role="user", content="你" * 400, is_temporary=False),
            SimpleNamespace(role="assistant", content="ответ" * 500, is_temporary=False),
            SimpleNamespace(role="user", content="unfinished", is_temporary=False),
        ]
        await provider.save_memory(messages, "session")
        with open(self.path, "r", encoding="utf-8") as stream:
            turns = yaml.safe_load(stream)["device-a"]["recent_turns"]
        self.assertEqual(len(turns), 1)
        self.assertLessEqual(len(turns[0]["user"].encode("utf-8")), 512)
        self.assertLessEqual(len(turns[0]["assistant"].encode("utf-8")), 1536)


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
    def __init__(self, enabled=True, version=1):
        self.features = {"device_tools": enabled, "device_tools_version": version}
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
    def test_v2_validates_every_command_result_shape(self):
        examples = {
            "search_audio": {"matches": [{"selection_id": 1, "name": "Song"}]},
            "play_audio": {"accepted": True},
            "get_time": {"available": False, "local_iso8601": None},
            "get_wifi_status": {"state": "scanning"},
            "get_wifi_signal": {"connected": False, "rssi_dbm": None},
            "get_sd_status": {"present": True, "mounted": True, "owner": "device"},
            "get_mic_status": {"ready": True, "powered": True, "power_sense_present": False},
            "get_battery_status": {"voltage_mv": 3820, "percentage": 65},
            "get_volume": {"level": 5},
            "get_led_status": {"available": True, "enabled": False, "color": "Cyan"},
            "open_battery_details": {"accepted": True},
            "set_volume": {"level": 6},
            "set_led_enabled": {"available": True, "enabled": True, "color": "Cyan"},
            "set_led_color": {"available": True, "enabled": False, "color": "Blue"},
            "search_images": {"matches": []},
            "open_image": {"accepted": True},
            "search_videos": {"matches": []},
            "play_video": {"accepted": True},
            "search_files": {"matches": [{"selection_id": 1, "name": "Photos", "kind": "folder"}]},
            "show_file_info": {"name": "notes.txt", "type": "unknown", "size_bytes": 12,
                               "modified_unix": None},
            "open_folder": {"accepted": True},
            "search_web": {"accepted": True},
        }
        self.assertEqual(set(examples), DEVICE_TOOL_NAMES)
        for command, result in examples.items():
            with self.subTest(command=command):
                self.assertTrue(_valid_result(command, result))
                self.assertFalse(_valid_result(command, {**result, "path": "/private"}))

    async def test_v2_timeout_and_disconnect_do_not_leave_pending_request(self):
        conn = FakeDeviceConnection(version=2)
        executor = DeviceAudioExecutor(conn)
        from core.providers.tools.device_audio.device_audio_executor import DeviceAudioRpc
        with mock.patch.object(DeviceAudioRpc, "TIMEOUT_SECONDS", 0.001):
            response = await executor.execute(conn, "get_time", {})
        self.assertEqual(response.action, Action.ERROR)
        self.assertIsNone(conn.esp_media_device_audio_rpc.pending_command)

        class BrokenSocket:
            async def send(self, _message):
                raise ConnectionError("private transport detail")

        conn.websocket = BrokenSocket()
        response = await executor.execute(conn, "get_time", {})
        self.assertEqual(response.action, Action.ERROR)
        self.assertNotIn("private transport detail", response.response)
        self.assertIsNone(conn.esp_media_device_audio_rpc.pending_command)

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

    async def test_v2_advertises_exact_new_command_set(self):
        conn = FakeDeviceConnection(version=2)
        executor = DeviceAudioExecutor(conn)
        self.assertEqual(set(executor.get_tools()), DEVICE_TOOL_NAMES)
        for tool in executor.get_tools().values():
            self.assertFalse(tool.description["function"]["parameters"]["additionalProperties"])
        self.assertEqual(DeviceAudioExecutor(FakeDeviceConnection(version=False)).get_tools(), {})

    async def test_v2_status_result_rejects_extra_or_wrong_fields(self):
        conn = FakeDeviceConnection(version=2)
        task = asyncio.create_task(DeviceAudioExecutor(conn).execute(conn, "get_battery_status", {}))
        await asyncio.sleep(0)
        base = {"type": "device_result", "session_id": conn.session_id,
                "request_id": 1, "ok": True}
        await handle_device_result(conn, {**base, "result": {
            "voltage_mv": 3820, "percentage": 65, "path": "/private"}})
        await handle_device_result(conn, {**base, "result": {
            "voltage_mv": True, "percentage": 65}})
        self.assertFalse(task.done())
        await handle_device_result(conn, {**base, "result": {
            "voltage_mv": 3820, "percentage": 65}})
        self.assertIn('"voltage_mv":3820', (await task).result)

    async def test_v2_typed_selection_and_terminal_handoff(self):
        conn = FakeDeviceConnection(version=2)
        executor = DeviceAudioExecutor(conn)
        search = asyncio.create_task(executor.execute(conn, "search_files", {"query": "photo"}))
        await asyncio.sleep(0)
        await handle_device_result(conn, {
            "type": "device_result", "session_id": conn.session_id,
            "request_id": 1, "ok": True,
            "result": {"matches": [{"selection_id": 1, "name": "Photos", "kind": "folder"}]},
        })
        self.assertEqual((await search).action, Action.REQLLM)
        launch = asyncio.create_task(executor.execute(conn, "open_folder", {"selection_id": 1}))
        await asyncio.sleep(0)
        await handle_device_result(conn, {
            "type": "device_result", "session_id": conn.session_id,
            "request_id": 2, "ok": True, "result": {"accepted": True},
        })
        self.assertEqual((await launch).action, Action.REQLLM)
        self.assertTrue(conn.esp_media_terminal_device_action)

    async def test_v2_rejects_invalid_arguments_before_rpc(self):
        conn = FakeDeviceConnection(version=2)
        executor = DeviceAudioExecutor(conn)
        invalid = (("set_volume", {"level": True}),
                   ("set_led_color", {"color": "purple"}),
                   ("search_web", {"query": "x" * 97}),
                   ("open_image", {"selection_id": 0}))
        for name, args in invalid:
            self.assertEqual((await executor.execute(conn, name, args)).action, Action.ERROR)
        self.assertEqual(conn.websocket.messages, [])


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


class ResponsesProviderTests(unittest.TestCase):
    class FakeStream:
        def __init__(self, events):
            self.events = events
            self.closed = False

        def __iter__(self):
            return iter(self.events)

        def close(self):
            self.closed = True

    class FakeResponses:
        def __init__(self, stream):
            self.stream = stream
            self.request = None

        def create(self, **request):
            self.request = request
            return self.stream

    def _provider(self, events):
        provider = ResponsesProvider.__new__(ResponsesProvider)
        provider.model_name = "gpt-5.6-luna"
        provider.reasoning_effort = "medium"
        provider.client = SimpleNamespace(
            responses=self.FakeResponses(self.FakeStream(events))
        )
        return provider

    def test_streams_text_and_normalizes_function_argument_deltas(self):
        function = SimpleNamespace(
            type="function_call", id="item-1", call_id="call-1",
            name="direct_answer", arguments='{"response":"Hello"}',
        )
        events = [
            SimpleNamespace(type="response.output_text.delta", delta="Hi "),
            SimpleNamespace(type="response.output_item.added", item_id="item-1", output_index=0, item=function),
            SimpleNamespace(type="response.function_call_arguments.delta", item_id="item-1", output_index=0, delta='{"response":"Hel'),
            SimpleNamespace(type="response.output_item.done", item_id="item-1", output_index=0, item=function),
        ]
        provider = self._provider(events)
        result = list(provider.response_with_functions("session", [
            {"role": "system", "content": "policy"},
            {"role": "user", "content": "hello"},
        ], functions=[{"type": "function", "function": {
            "name": "direct_answer", "parameters": {"type": "object"},
        }}]))
        self.assertEqual(result[0][0], "Hi ")
        calls = [call for _, call_list in result if call_list for call in call_list]
        self.assertEqual(calls[0].function.name, "direct_answer")
        self.assertEqual("".join(call.function.arguments for call in calls), '{"response":"Hello"}')
        request = provider.client.responses.request
        self.assertEqual(request["reasoning"], {"effort": "medium"})
        self.assertEqual(request["instructions"], "policy")
        self.assertEqual(request["tools"][0]["name"], "direct_answer")

    def test_converts_stored_chat_tool_history_to_responses_items(self):
        instructions, items = ResponsesProvider._to_responses_input([
            {"role": "system", "content": "policy"},
            {"role": "assistant", "tool_calls": [{
                "id": "call-2", "function": {"name": "search_audio", "arguments": "{}"},
            }]},
            {"role": "tool", "tool_call_id": "call-2", "content": "matches"},
        ])
        self.assertEqual(instructions, "policy")
        self.assertEqual(items[0]["type"], "function_call")
        self.assertEqual(items[1], {
            "type": "function_call_output", "call_id": "call-2", "output": "matches",
        })


class AzureTtsProviderTests(unittest.IsolatedAsyncioTestCase):
    class FakeResponse:
        def __init__(self, status_code=200, content=b"mp3"):
            self.status_code = status_code
            self.content = content

    class FakeClient:
        def __init__(self, response, captured):
            self.response = response
            self.captured = captured

        async def __aenter__(self):
            return self

        async def __aexit__(self, *_args):
            return False

        async def post(self, endpoint, **kwargs):
            self.captured["endpoint"] = endpoint
            self.captured.update(kwargs)
            return self.response

    def _provider(self):
        return AzureTtsProvider({
            "api_key": "not-logged", "region": "westeurope", "output_dir": "tmp/",
            "fallback_locale": "en-US",
            "voices": {
                "en-US": "en-US-JennyMultilingualNeural",
                "zh-CN": "zh-CN-XiaoxiaoMultilingualNeural",
                "ru-RU": "ru-RU-SvetlanaNeural",
            },
        }, True)

    async def test_routes_scripts_and_escapes_ssml_without_exposing_key(self):
        provider = self._provider()
        self.assertEqual(provider._locale_for("hello"), "en-US")
        self.assertEqual(provider._locale_for("你好"), "zh-CN")
        self.assertEqual(provider._locale_for("Привет"), "ru-RU")
        self.assertEqual(provider._locale_for("123"), "ru-RU")
        captured = {}
        with mock.patch("core.providers.tts.esp_media_azure_multilingual.httpx.AsyncClient",
                        return_value=self.FakeClient(self.FakeResponse(), captured)):
            self.assertEqual(await provider.text_to_speak("a < b & c", None), b"mp3")
        self.assertIn(b"a &lt; b &amp; c", captured["content"])
        self.assertEqual(captured["headers"]["X-Microsoft-OutputFormat"],
                         "audio-24khz-48kbitrate-mono-mp3")
        self.assertNotIn("not-logged", str(provider._ssml("text", "en-US")))

    async def test_http_failure_has_no_credential_or_text(self):
        provider = self._provider()
        with mock.patch("core.providers.tts.esp_media_azure_multilingual.httpx.AsyncClient",
                        return_value=self.FakeClient(self.FakeResponse(401), {})):
            with self.assertRaisesRegex(RuntimeError, "HTTP 401") as error:
                await provider.text_to_speak("private words", None)
        self.assertNotIn("not-logged", str(error.exception))
        self.assertNotIn("private words", str(error.exception))


if __name__ == "__main__":
    unittest.main()
