import asyncio
import json
from typing import Any, Dict

from core.handle.abortHandle import handleAbortMessage
from core.handle.textMessageHandler import TextMessageHandler
from core.handle.textMessageType import TextMessageType
from core.utils.dialogue import Dialogue


class ConversationMessageHandler(TextMessageHandler):
    """Authenticated, connection-scoped conversation control."""

    @property
    def message_type(self) -> TextMessageType:
        return TextMessageType.CONVERSATION

    async def _reply(self, conn, request_id: Any, result: str,
                     category: str = None) -> None:
        reply = {
            "type": "conversation",
            "action": "reset",
            "request_id": request_id if isinstance(request_id, int) else 0,
            "result": result,
        }
        if category:
            reply["category"] = category
        await conn.websocket.send(json.dumps(reply, separators=(",", ":")))

    async def handle(self, conn, msg_json: Dict[str, Any]) -> None:
        request_id = msg_json.get("request_id")
        if (msg_json.get("action") != "reset" or
                isinstance(request_id, bool) or
                not isinstance(request_id, int) or
                request_id < 1 or request_id > 0xFFFFFFFF):
            await self._reply(conn, request_id, "error", "invalid_request")
            return

        memory = getattr(conn, "memory", None)
        if memory is None or not hasattr(memory, "reset_memory"):
            await self._reply(conn, request_id, "error", "unsupported_memory")
            return

        try:
            await handleAbortMessage(conn)
            await asyncio.to_thread(memory.reset_memory)
            conn.dialogue = Dialogue()
            if getattr(conn, "prompt", None):
                conn.change_system_prompt(conn.prompt)
            await self._reply(conn, request_id, "ok")
            conn.logger.info("Conversation memory reset completed")
        except Exception:
            conn.logger.error("Conversation memory reset failed")
            await self._reply(conn, request_id, "error", "reset_failed")

