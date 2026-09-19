# Local Xiaozhi production backend

This pinned arm64 Compose stack is the Stage 2 production target for ESP Media.
It runs Xiaozhi's authenticated bootstrap and voice WebSocket service behind
Caddy-managed HTTPS/WSS. The language-matching pipeline is `SileroVAD`, `OpenaiASR`,
`OpenAILLM`, `EdgeTTS`, `nointent`, and `nomem`; it uses
`zh-CN-XiaoxiaoNeural`, the Edge TTS voice supported by the local service, so
Chinese input receives Simplified-Chinese speech. The prompt still makes replies
match other input languages, although the fixed Mandarin voice may give non-Chinese
speech a Mandarin accent. MQTT and public Xiaozhi fallback remain disabled.

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
   private `api_key` fields.
3. Put the pinned Silero VAD ONNX file at
   `.runtime/models/snakers4_silero-vad/src/silero_vad/data/silero_vad.onnx`.
   The validated file is 2,327,524 bytes with SHA-256
   `1a153a22f4509e292a94e67d6f9b85e8deb25b4988682b7e174c65279d8788e3`.
4. Validate the private configuration, then start the pinned services:

   ```sh
   ruby check_config.rb config .runtime/data/.config.yaml
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
health, the Caddy trust chain, and the authenticated bootstrap response without
printing credentials. Use `docker-compose --env-file .runtime/endpoint.env ps`
for status. Avoid routine backend log output because transcripts and provider
errors can contain user data.

## Trust-anchor lifecycle

`docker-compose down` preserves `.runtime/` and therefore preserves the private
CA. Deleting `.runtime/caddy-data`, or starting Caddy with a new data volume,
rotates that CA. After a rotation, copy the new root into the ignored firmware
configuration and rebuild before the device can connect. Never disable
certificate verification or replace the DNS hostname with a bare LAN IP.
