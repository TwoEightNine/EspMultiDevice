#!/usr/bin/env python3
"""Apply ESP Media reset and authenticated device-tool extensions."""

from hashlib import sha256
from pathlib import Path
import shutil
import sys

DEVICE_TOOL_NAMES = (
    "search_audio", "play_audio", "get_time", "get_wifi_status", "get_wifi_signal",
    "get_sd_status", "get_mic_status", "get_battery_status", "get_volume",
    "get_led_status", "open_battery_details", "set_volume", "set_led_enabled",
    "set_led_color", "search_images", "open_image", "search_videos", "play_video",
    "search_files", "show_file_info", "open_folder", "search_web", "list_directory",
)
DEVICE_TOOL_NAMES_LITERAL = repr(DEVICE_TOOL_NAMES)
# Upstream plugins hidden from authenticated device sessions: play_music
# competes with device playback, and the search/weather/news plugins need
# China-only providers. Hosted web_search in the Responses adapter replaces them.
DEVICE_SESSION_EXCLUDED_PLUGINS = (
    "play_music", "web_search", "get_weather",
    "get_news_from_newsnow", "get_news_from_chinanews",
)
DEVICE_SESSION_EXCLUDED_PLUGINS_LITERAL = repr(DEVICE_SESSION_EXCLUDED_PLUGINS)


EXPECTED = {
    "core/handle/textMessageType.py":
        "9a62efb730723fe47588cd12d4d38af091dea062f4e7a1b0cd8de691be40522e",
    "core/handle/textMessageHandlerRegistry.py":
        "9fd213d714384be4fc5e4b87ae725ca78789bb58fa9c7786cab0f873651e43c3",
    "core/handle/textMessageProcessor.py":
        "61a3b88f1528410f29122d0ca3933e159474222bf333e93c0aa1739d5f2e434d",
    "core/providers/memory/mem_local_short/mem_local_short.py":
        "98013810ac5316a0d669d9d274e636066ff1efe184e5d2335bb88be05465fd17",
    "core/connection.py":
        "e065c994bf3f926b81c69da045e09fad8c46d6fd1fc3e3a6403295aeab64e0fa",
    "core/providers/tools/base/tool_types.py":
        "5cac336143c085ff3ecc724f9d98bdd41aa4fd6ce93c5c3f8b169957a4977a19",
    "core/providers/tools/unified_tool_handler.py":
        "a4d3d492c2a0ed1c5c4cf1af19f145e73fd24ae32daccd88f4ffa0e94953078d",
    "core/handle/helloHandle.py":
        "77be3faf5c8201450f1b98c51140a8960155a9bf95f67dfc288768ba14857d51",
    "core/providers/tools/unified_tool_manager.py":
        "e6ce2d08b1d4932735170e6a6440506b2461311f9f2141cf65913447a57fc41b",
}


def checked_text(root: Path, relative: str) -> str:
    path = root / relative
    data = path.read_bytes()
    actual = sha256(data).hexdigest()
    if actual != EXPECTED[relative]:
        raise SystemExit(
            f"upstream layout mismatch for {relative}: expected "
            f"{EXPECTED[relative]}, got {actual}"
        )
    return data.decode("utf-8")


def replace_once(text: str, old: str, new: str, relative: str) -> str:
    if text.count(old) != 1:
        raise SystemExit(f"overlay anchor mismatch for {relative}")
    return text.replace(old, new, 1)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: apply_overlay.py SERVER_ROOT")
    root = Path(sys.argv[1]).resolve()
    overlay = Path(__file__).resolve().parent

    relative = "core/handle/textMessageType.py"
    text = checked_text(root, relative)
    text = replace_once(text, '    PING = "ping"\n',
                        '    PING = "ping"\n    CONVERSATION = "conversation"\n'
                        '    DEVICE_RESULT = "device_result"\n',
                        relative)
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/handle/helloHandle.py"
    text = checked_text(root, relative)
    text = replace_once(text, "        conn.features = features\n",
                        "        conn.features = features\n"
                        "        # Features arrive independently of background component setup.\n"
                        "        # Refresh cached tool descriptions once capability negotiation completes.\n"
                        "        if getattr(conn, \"func_handler\", None):\n"
                        "            conn.func_handler.tool_manager.refresh_tools()\n", relative)
    (root / relative).write_text(text, encoding="utf-8")

    # A closing connection must enter the provider's per-device lock before
    # the next connection loads memory. Awaiting the provider directly enters
    # its process-wide role lock before this coroutine can yield, closing the
    # scheduling gap left by the upstream fire-and-forget daemon thread.
    relative = "core/connection.py"
    text = checked_text(root, relative)
    text = replace_once(
        text,
        "                # 启动线程保存记忆，不等待完成\n"
        "                threading.Thread(target=save_memory_task, daemon=True).start()\n",
        "                # Serialize completion with the next load/reset for this device.\n"
        "                await self.memory.save_memory(\n"
        "                    self.dialogue.dialogue, self.session_id\n"
        "                )\n",
        relative,
    )
    text = replace_once(text, "        self.dialogue = Dialogue()\n",
                        "        self.dialogue = Dialogue()\n"
                        "        self.esp_media_terminal_device_action = False\n", relative)
    text = replace_once(text, "    def chat(self, query, depth=0):\n",
                        "    def chat(self, query, depth=0, suppress_tts=False):\n", relative)
    text = replace_once(text,
                        "                            if remaining:\n"
                        "                                remaining = self._clean_response_garbage(remaining)\n",
                        "                            if remaining and not suppress_tts:\n"
                        "                                remaining = self._clean_response_garbage(remaining)\n", relative)
    text = replace_once(text,
                        "                if content is not None and len(content) > 0:\n"
                        "                    if not tool_call_flag:\n",
                        "                if content is not None and len(content) > 0:\n"
                        "                    if not tool_call_flag and not suppress_tts:\n", relative)
    text = replace_once(text, "        if need_llm_tools:\n",
                        "        terminal_device_action = bool(getattr(self, \"esp_media_terminal_device_action\", False))\n"
                        "        self.esp_media_terminal_device_action = False\n\n"
                        "        if need_llm_tools:\n", relative)
    text = replace_once(text, "            self.chat(None, depth=depth + 1)\n",
                        "            self.chat(None, depth=depth + 1, suppress_tts=terminal_device_action)\n", relative)
    text = replace_once(text,
                        "                    self.logger.bind(tag=TAG).debug(\n"
                        "                        f\"function_name={tool_call_data['name']}, function_id={tool_call_data['id']}, function_arguments={tool_call_data['arguments']}\"\n"
                        "                    )\n",
                        f"                    if tool_call_data['name'] in {DEVICE_TOOL_NAMES_LITERAL}:\n"
                        "                        self.logger.bind(tag=TAG).debug(\n"
                        "                            f\"device_audio function_name={tool_call_data['name']}, function_id={tool_call_data['id']}\"\n"
                        "                        )\n"
                        "                    else:\n"
                        "                        self.logger.bind(tag=TAG).debug(\n"
                        "                            f\"function_name={tool_call_data['name']}, function_id={tool_call_data['id']}, function_arguments={tool_call_data['arguments']}\"\n"
                        "                        )\n", relative)
    text = replace_once(text,
                        "                    enqueue_tool_report(self, tool_call_data['name'], tool_input)\n",
                        f"                    if tool_call_data['name'] in {DEVICE_TOOL_NAMES_LITERAL}:\n"
                        "                        tool_input = {}\n"
                        "                    enqueue_tool_report(self, tool_call_data['name'], tool_input)\n", relative)
    text = replace_once(text,
                        "                        enqueue_tool_report(self, tool_call_data['name'], tool_input, str(result.result) if result.result else None, report_tool_call=False)\n",
                        f"                        reported_result = None if tool_call_data['name'] in {DEVICE_TOOL_NAMES_LITERAL} else (str(result.result) if result.result else None)\n"
                        "                        enqueue_tool_report(self, tool_call_data['name'], tool_input, reported_result, report_tool_call=False)\n", relative)
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/handle/textMessageHandlerRegistry.py"
    text = checked_text(root, relative)
    text = replace_once(
        text,
        "from core.handle.textHandler.pingMessageHandler import PingMessageHandler\n",
        "from core.handle.textHandler.pingMessageHandler import PingMessageHandler\n"
                        "from core.handle.textHandler.conversationMessageHandler import "
        "ConversationMessageHandler\n"
        "from core.handle.textHandler.deviceAudioMessageHandler import DeviceAudioMessageHandler\n",
        relative,
    )
    text = replace_once(text, "            PingMessageHandler(),\n",
                        "            PingMessageHandler(),\n"
                        "            ConversationMessageHandler(),\n"
                        "            DeviceAudioMessageHandler(),\n", relative)
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/handle/textMessageProcessor.py"
    text = checked_text(root, relative)
    text = replace_once(
        text,
        '                conn.logger.bind(tag=TAG).info(f"收到{message_type}消息：{message}")\n',
        '                if message_type in ("conversation", "device_result"):\n'
        '                    conn.logger.bind(tag=TAG).info("收到私有控制消息")\n'
        '                else:\n'
        '                    conn.logger.bind(tag=TAG).info(f"收到{message_type}消息：{message}")\n',
        relative,
    )
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/providers/tools/base/tool_types.py"
    text = checked_text(root, relative)
    text = replace_once(text, "    MCP_ENDPOINT = \"mcp_endpoint\"  # MCP接入点\n",
                        "    MCP_ENDPOINT = \"mcp_endpoint\"  # MCP接入点\n"
                        "    DEVICE_AUDIO = \"device_audio\"  # ESP Media custom RPC\n", relative)
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/providers/tools/unified_tool_handler.py"
    text = checked_text(root, relative)
    text = replace_once(text, "from .mcp_endpoint import MCPEndpointExecutor\n",
                        "from .mcp_endpoint import MCPEndpointExecutor\n"
                        "from .device_audio import DeviceAudioExecutor\n", relative)
    text = replace_once(text, "        self.mcp_endpoint_executor = MCPEndpointExecutor(conn)\n",
                        "        self.mcp_endpoint_executor = MCPEndpointExecutor(conn)\n"
                        "        self.device_audio_executor = DeviceAudioExecutor(conn)\n", relative)
    text = replace_once(text, "        self.tool_manager.register_executor(\n            ToolType.MCP_ENDPOINT, self.mcp_endpoint_executor\n        )\n",
                        "        self.tool_manager.register_executor(\n            ToolType.MCP_ENDPOINT, self.mcp_endpoint_executor\n        )\n"
                        "        self.tool_manager.register_executor(\n            ToolType.DEVICE_AUDIO, self.device_audio_executor\n        )\n", relative)
    text = replace_once(text, "            self.logger.debug(f\"调用函数: {function_name}, 参数: {arguments}\")\n",
                        f"            if function_name in {DEVICE_TOOL_NAMES_LITERAL}:\n"
                        "                self.logger.debug(f\"调用设备音频函数: {function_name}\")\n"
                        "            else:\n"
                        "                self.logger.debug(f\"调用函数: {function_name}, 参数: {arguments}\")\n", relative)
    text = replace_once(text,
                        "                await send_display_message(self.conn, f\"% {function_name}\")\n",
                        f"                if function_name not in {DEVICE_TOOL_NAMES_LITERAL}:\n"
                        "                    await send_display_message(self.conn, f\"% {function_name}\")\n",
                        relative)
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/providers/tools/unified_tool_manager.py"
    text = checked_text(root, relative)
    text = replace_once(text, "            self.logger.info(f\"执行工具: {tool_name}，参数: {arguments}\")\n"
                        "            result = await executor.execute(self.conn, tool_name, arguments)\n"
                        "            self.logger.debug(f\"工具执行结果: {result}\")\n",
                        "            if tool_type == ToolType.DEVICE_AUDIO:\n"
                        "                self.logger.info(f\"执行设备音频工具: {tool_name}\")\n"
                        "            else:\n"
                        "                self.logger.info(f\"执行工具: {tool_name}，参数: {arguments}\")\n"
                        "            result = await executor.execute(self.conn, tool_name, arguments)\n"
                        "            if tool_type == ToolType.DEVICE_AUDIO:\n"
                        "                self.logger.debug(f\"设备音频工具完成: {tool_name}, action={result.action.name}\")\n"
                        "            else:\n"
                        "                self.logger.debug(f\"工具执行结果: {result}\")\n", relative)
    text = replace_once(text, "        self._cached_tools = all_tools\n",
                        "        # ESP Media owns normal-SD playback. Its older generic\n"
                        "        # play_music plugin can only synthesize a spoken reply and\n"
                        "        # must not compete with the authenticated device RPC.\n"
                        "        # The upstream search, weather, and news plugins depend on\n"
                        "        # China-only providers and demo keys; the Responses adapter's\n"
                        "        # hosted web_search replaces them.\n"
                        "        if isinstance(getattr(self.conn, \"features\", None), dict) and \\\n"
                        "                self.conn.features.get(\"device_tools\"):\n"
                        f"            for _name in {DEVICE_SESSION_EXCLUDED_PLUGINS_LITERAL}:\n"
                        "                all_tools.pop(_name, None)\n"
                        "        self._cached_tools = all_tools\n", relative)
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/providers/memory/mem_local_short/mem_local_short.py"
    text = checked_text(root, relative)
    text += ("\n\n# ESP Media overlay: process-wide per-device serialization and atomic reset.\n"
             "from .esp_media_memory_overlay import install as _install_esp_media_memory\n"
             "_install_esp_media_memory(MemoryProvider)\n")
    (root / relative).write_text(text, encoding="utf-8")
    shutil.copyfile(
        overlay / "esp_media_memory_overlay.py",
        root / "core/providers/memory/mem_local_short/esp_media_memory_overlay.py",
    )
    shutil.copyfile(
        overlay / "conversationMessageHandler.py",
        root / "core/handle/textHandler/conversationMessageHandler.py",
    )
    shutil.copytree(overlay / "device_audio", root / "core/providers/tools/device_audio")
    shutil.copyfile(
        overlay / "deviceAudioMessageHandler.py",
        root / "core/handle/textHandler/deviceAudioMessageHandler.py",
    )
    responses_provider = root / "core/providers/llm/esp_media_openai_responses"
    responses_provider.mkdir(exist_ok=True)
    shutil.copyfile(
        overlay / "esp_media_openai_responses.py",
        responses_provider / "esp_media_openai_responses.py",
    )
    shutil.copyfile(
        overlay / "esp_media_azure_multilingual.py",
        root / "core/providers/tts/esp_media_azure_multilingual.py",
    )


if __name__ == "__main__":
    main()
