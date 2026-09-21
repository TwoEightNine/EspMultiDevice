#!/usr/bin/env python3
"""Apply the ESP Media reset extension only to the pinned upstream layout."""

from hashlib import sha256
from pathlib import Path
import shutil
import sys


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
                        '    PING = "ping"\n    CONVERSATION = "conversation"\n',
                        relative)
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
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/handle/textMessageHandlerRegistry.py"
    text = checked_text(root, relative)
    text = replace_once(
        text,
        "from core.handle.textHandler.pingMessageHandler import PingMessageHandler\n",
        "from core.handle.textHandler.pingMessageHandler import PingMessageHandler\n"
        "from core.handle.textHandler.conversationMessageHandler import "
        "ConversationMessageHandler\n",
        relative,
    )
    text = replace_once(text, "            PingMessageHandler(),\n",
                        "            PingMessageHandler(),\n"
                        "            ConversationMessageHandler(),\n", relative)
    (root / relative).write_text(text, encoding="utf-8")

    relative = "core/handle/textMessageProcessor.py"
    text = checked_text(root, relative)
    text = replace_once(
        text,
        '                conn.logger.bind(tag=TAG).info(f"收到{message_type}消息：{message}")\n',
        '                if message_type == "conversation":\n'
        '                    conn.logger.bind(tag=TAG).info("收到conversation控制消息")\n'
        '                else:\n'
        '                    conn.logger.bind(tag=TAG).info(f"收到{message_type}消息：{message}")\n',
        relative,
    )
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


if __name__ == "__main__":
    main()
