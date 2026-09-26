from typing import Any, Dict, TYPE_CHECKING

if TYPE_CHECKING:
    from core.connection import ConnectionHandler
from core.handle.textMessageHandler import TextMessageHandler
from core.handle.textMessageType import TextMessageType
from core.providers.tools.device_audio import handle_device_result


class DeviceAudioMessageHandler(TextMessageHandler):
    @property
    def message_type(self) -> TextMessageType:
        return TextMessageType.DEVICE_RESULT

    async def handle(self, conn: "ConnectionHandler", msg_json: Dict[str, Any]) -> None:
        await handle_device_result(conn, msg_json)
