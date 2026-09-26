# Local Xiaozhi production backend

This pinned arm64 Compose stack is the production target for ESP Media.
It builds a small checksum-guarded reset overlay on the digest-pinned Xiaozhi
image, then runs authenticated bootstrap and voice WebSocket service behind
Caddy-managed HTTPS/WSS. The language-matching pipeline is `SileroVAD`, `OpenaiASR`,
`OpenAILLM`, `EdgeTTS`, `nointent`, and `mem_local_short` using `OpenAILLM`; it uses
`zh-CN-XiaoxiaoNeural`, the Edge TTS voice supported by the local service, so
Chinese input receives Simplified-Chinese speech. The prompt still makes replies
match other input languages, although the fixed Mandarin voice may give non-Chinese
speech a Mandarin accent. MQTT and public Xiaozhi fallback remain disabled.
WebSocket ping handling is enabled for the firmware's 60-second heartbeat.
The backend also uses OpenAI function calling for the firmware's custom,
authenticated `device_tools` RPC. MCP stays disabled: the only v1 tools search
normal SD WAV/MP3 filenames and play a returned opaque selection. The backend
never receives a source path or secret-storage content.

Runtime state, the Caddy private CA, model data, endpoint settings, and provider
secrets live below ignored `.runtime/` paths. The OpenAI API key must appear only
in `.runtime/data/.config.yaml`, separately in the ASR and LLM sections. Never
put it in Compose, firmware configuration, tracked examples, logs, or commands.

## Configure and start

1. Start Colima with at least four CPUs and 6 GiB RAM:

   ```sh
   colima start --arch aarch64 --vm-type vz --cpus 4 --memory 6 --disk 30
   ```

2. Copy `endpoint.example.env` to `.runtime/endpoint.env` and
   `config.example.yaml` to `.runtime/data/.config.yaml`. Replace the example
   LAN host with this Mac's current `sslip.io` DNS hostname in both files. Keep
   the advertised endpoint on WSS and enter the OpenAI key only in the two
   private `api_key` fields. Existing installations must also change
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

The overlay only registers the audio tools after the client hello advertises
`device_tools`. Each tool call uses a session-bound nonzero request ID and a
single in-flight `device_command`/`device_result` exchange. Invalid, delayed,
duplicate, or mismatched results are discarded. `play_audio` is terminal: the
backend gives the result to the LLM but suppresses a competing TTS reply while
the firmware transitions to its Audio Player.

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

Exact dialogue remains inside one WSS connection. On close, `mem_local_short`
stores a summarized per-device memory in `.runtime/data/.memory.yaml`; it is
personalization, not a transcript. New conversation sends
`{"type":"conversation","action":"reset","request_id":N}` over the already
authenticated WSS. The server aborts outstanding dialogue work, atomically
deletes only that device's YAML entry, clears the provider summary and live
Dialogue, restores the system prompt, and returns an `ok` or bounded error
category. Duplicate requests are safe. Per-device epochs and locks prevent a
late summary from a closing connection from resurrecting reset memory.

`server_overlay/Dockerfile` retains upstream commit
`6afc54a17def47578a4b3efc4680873689d3168b` and base manifest
`sha256:4673fac2cb7125d6370a3e8cd6cd9985195c9bfc6203d3ced41e86a1b9775c30`.
`apply_overlay.py` verifies every modified upstream file checksum before
patching, so layout drift fails the image build. Xiaozhi's MIT notice is copied
into the image and retained in `server_overlay/XIAOZHI_MIT_LICENSE.txt`.
