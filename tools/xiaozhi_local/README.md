# Local Xiaozhi production backend

This pinned arm64 Compose stack is the production target for ESP Media.
It builds a small checksum-guarded reset overlay on the digest-pinned Xiaozhi
image, then runs authenticated bootstrap and voice WebSocket service behind
Caddy-managed HTTPS/WSS. The language-matching pipeline is `SileroVAD`, OpenAI
`gpt-transcribe`, GPT-5.6 Luna through the Responses API, Azure multilingual
TTS, `function_call`, and `mem_local_short`. Every chat, tool follow-up, and
summary-memory request uses Luna at medium reasoning. Azure routes English,
Simplified Chinese, and Russian segments to Ada HD, Xiaoxiao HD Flash, and
Dariya respectively. These configured HD voices require an Azure Speech
resource in `eastus`, `westeurope`, or `southeastasia`. The backend resamples
synthesized audio and sends 16 kHz Opus
downlink frames, matching the firmware's native 60 ms (960-sample) playback
frames. MQTT and public Xiaozhi fallback remain disabled.
WebSocket ping handling is enabled for the firmware's 60-second heartbeat.
The backend also uses OpenAI function calling for the firmware's custom,
authenticated `device_tools` RPC. MCP stays disabled: the only v1 tools search
normal SD WAV/MP3 filenames and play a returned opaque selection. The backend
receives neither a source path nor secret-storage content from a v1 client.
Version-2 firmware additionally exposes eight device-status commands, three
persisted setting commands, Battery Details, image/video/file search and
selection, and DuckDuckGo Lite Browser search. The backend never sends a raw
file path or URL to firmware. Version-3 firmware returns user-facing
SD-relative paths from file search and adds paged visible-directory listing;
paths remain result data and are never accepted back as filesystem authority.

Runtime state, the Caddy private CA, model data, endpoint settings, and provider
secrets live below ignored `.runtime/` paths. The OpenAI API key must appear only
in `.runtime/data/.config.yaml`, separately in the ASR and LLM sections. Azure
Speech's resource key and region also belong only in that ignored YAML. Never put
any provider secret in Compose, firmware configuration, tracked examples, logs,
or commands.

## Configure and start

1. Start Colima with at least four CPUs and 6 GiB RAM:

   ```sh
   colima start --arch aarch64 --vm-type vz --cpus 4 --memory 6 --disk 30
   ```

2. Copy `endpoint.example.env` to `.runtime/endpoint.env` and
   `config.example.yaml` to `.runtime/data/.config.yaml`. Replace the example
   LAN host with this Mac's current `sslip.io` DNS hostname in both files. Keep
   the advertised endpoint on WSS, enter the OpenAI key in the ASR and LLM
   `api_key` fields, and enter a valid Azure Speech key plus region in the TTS
   block. Preserve the TTS `language` directive from the example: the upstream
   prompt enhancer otherwise defaults it to Chinese and overrides the
   multilingual system prompt. Existing installations must also change
   `selected_module.Memory` from `nomem` to `mem_local_short`, select
   `Intent: function_call` with the matching `Intent.function_call` block, add the matching
   `Memory.mem_local_short` block, and enable the root-level
   `enable_websocket_ping` setting.
3. Put the pinned Silero VAD ONNX file at
   `.runtime/models/snakers4_silero-vad/src/silero_vad/data/silero_vad.onnx`.
   The validated file is 2,327,524 bytes with SHA-256
   `1a153a22f4509e292a94e67d6f9b85e8deb25b4988682b7e174c65279d8788e3`.
4. Validate the private configuration, then start the pinned services:

   ```sh
   ruby check_config.rb config .runtime/data/.config.yaml
   docker-compose --env-file .runtime/endpoint.env build xiaozhi
   docker-compose --env-file .runtime/endpoint.env up -d
   ./check_backend.sh
   ```

5. Caddy creates its local root at
   `.runtime/caddy-data/caddy/pki/authorities/local/root.crt`. Copy the complete
   PEM to the ignored
   `../../src/services/assistant/LocalAssistantBackendConfig.h`, together with
   this backend's HTTPS bootstrap/activation URLs. The compatibility diagnostic
   keeps its separate ignored
   `../../tests/xiaozhi_compatibility_probe/LocalBackendConfig.h`.

`check_backend.sh` verifies the selected providers, exact pinned models,
placeholder-free private keys, DNS/WSS advertisement, disabled MQTT, container
health, the Caddy trust chain, authenticated bootstrap, and an authenticated
idempotent reset for an isolated test device without printing credentials. Use `docker-compose --env-file .runtime/endpoint.env ps`
for status. Avoid routine backend log output because transcripts and provider
errors can contain user data.

The overlay registers only audio tools for version-1 `device_tools` clients,
the existing expanded catalog for version 2, and `list_directory` plus
path-bearing file search for clients advertising `device_tools_version: 3`.
Each tool call uses a session-bound nonzero request ID and a
single in-flight `device_command`/`device_result` exchange. Invalid, delayed,
duplicate, or mismatched results are discarded. Media, folder, Battery Details,
and Browser launches are terminal: the backend gives the result to the LLM but
suppresses competing TTS while the firmware changes screens. Status and
setting results remain speakable. All device-tool arguments and results are
redacted from backend logs; Browser logs redact HTTP(S) URLs.

To add another device capability, follow
[the repository command guide](../../docs/agentic-commands.md). Keep all
backend changes in `server_overlay`, update the checksum-guarded patch and its
tests, and do not expose tool inputs or result payloads in logs.

## Trust-anchor lifecycle

`docker-compose down` preserves `.runtime/` and therefore preserves the private
CA. Deleting `.runtime/caddy-data`, or starting Caddy with a new data volume,
rotates that CA. After a rotation, copy the new root into the ignored firmware
configuration and rebuild before the device can connect. Never disable
certificate verification or replace the DNS hostname with a bare LAN IP.

## Conversation memory and reset overlay

Exact dialogue remains inside one WSS connection, while the backend persists
the most recent 12 completed user/assistant exchanges plus a summarized
per-device memory in `.runtime/data/.memory.yaml`. This local backend state is
not encrypted at rest. New conversation sends
`{"type":"conversation","action":"reset","request_id":N}` over the already
authenticated WSS. The server aborts outstanding dialogue work, atomically
deletes only that device's versioned YAML entry, clears its summary, exact
turns, and live Dialogue, restores the system prompt, and returns an `ok` or
bounded error category. Duplicate requests are safe. Per-device epochs and
locks prevent a late closer from resurrecting reset memory. Legacy scalar
summary entries migrate on their next local write.

`server_overlay/Dockerfile` retains upstream commit
`6afc54a17def47578a4b3efc4680873689d3168b` and base manifest
`sha256:4673fac2cb7125d6370a3e8cd6cd9985195c9bfc6203d3ced41e86a1b9775c30`.
`apply_overlay.py` verifies every modified upstream file checksum before
patching, so layout drift fails the image build. Xiaozhi's MIT notice is copied
into the image and retained in `server_overlay/XIAOZHI_MIT_LICENSE.txt`.
