# Assistant setup (optional)

The voice Assistant is an optional, advanced feature. Everything else in ESP
Media works without it, and a firmware built without its configuration simply
shows a configuration error on the Assistant screen.

The Assistant never talks to a public service. It needs:

- A self-hosted backend: the pinned Xiaozhi server and Caddy stack in
  [`tools/xiaozhi_local/`](../tools/xiaozhi_local/README.md), which runs with
  Docker Compose on an arm64 host such as an Apple Silicon Mac, on the same
  network as the device.
- Your own provider accounts: an OpenAI API key, used for `gpt-transcribe`
  speech recognition and GPT-5.6 Luna for all language-model work, and an
  Azure Speech resource for neural voices in English, Simplified Chinese, and
  Russian. Usage is billed by those providers.
- Artwork on the SD card for the Assistant screen.

For how the Assistant behaves and how it is secured, see
[services.md](services.md#production-assistant). To add a device command, see
[agentic-commands.md](agentic-commands.md).

## 1. Start the backend

Follow [`tools/xiaozhi_local/README.md`](../tools/xiaozhi_local/README.md) to
configure the private OpenAI and Azure credentials, start the stack, and run
`tools/xiaozhi_local/check_backend.sh`. The credentials stay in the backend's
ignored runtime configuration; they never go into the firmware.

## 2. Configure the firmware

1. Copy `src/services/assistant/LocalAssistantBackendConfig.example.h` to
   `src/services/assistant/LocalAssistantBackendConfig.h`. Git ignores the copy.
2. Set the bootstrap and activation HTTPS URLs to your backend's host name.
3. Paste the Caddy root CA certificate that the backend generated. The firmware
   trusts only that root for the Assistant.
4. Build and upload the firmware as usual.

If you later delete the backend's Caddy data, Caddy creates a new root CA. Paste
the new certificate and rebuild before the device can connect again.

## 3. Copy the artwork to the SD card

| Path on the card | Content |
| --- | --- |
| `/.system/ai_avatars/background.png` | Non-interlaced 320×432 PNG drawn directly below the app bar. It contains the fixed chat-bubble artwork. A missing, corrupt, or wrong-sized file falls back to black. |
| `/.system/ai_avatars/default/` | The five 320×240 avatar PNGs `w.png`, `n.png`, `s.png`, `h.png`, and `t.png`. Keep the lower-right 72×72 corner (x ≥ 248, y ≥ 168 in the PNG) free: the voice control covers it. |
| `/.system/ai_avatars/<dir>/` | Optional additional avatar sets with the same five file names. |
| `/.system/ai_avatars/avatars.config` | One `Name:dir` line per additional set. |

## Using the Assistant

- The screen shows only the latest exchange: your text in two lines with an
  ellipsis, and the Assistant's spoken reply paged three lines at a time every
  two seconds.
- The round 56×56 control in the bottom-right corner is blue while ready, green while listening,
  and gray when unavailable. Tap it to start talking, and tap it during a turn
  to cancel that turn.
- Hold the control for 800 ms to turn on auto-listen for the session. A short
  cyan pulse confirms it, and the next tap turns it off.
- The overflow menu has **New conversation** and the **Avatar** submenu.
- The session stays connected while the Assistant opens other screens. Its
  captions disappear only when you close it with the app-bar X.
- The backend keeps a summary plus the last 12 completed exchanges per device
  until you confirm **New conversation**.
