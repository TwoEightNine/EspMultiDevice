# Local Xiaozhi compatibility backend

This is the minimal self-hosted target for the ESP Media Stage 1 probe. It
runs Xiaozhi's WebSocket and OTA/bootstrap service behind Caddy-managed HTTPS
and WSS. Authentication is enabled, so bootstrap returns a signed, short-lived
credential instead of an empty or test token. It is intentionally not an
assistant deployment: the placeholder cloud ASR configuration is never used
by the bootstrap/hello probe, and no microphone audio is sent.

The deployment pins the upstream server and Caddy image manifests used for the
Apple-silicon host. Runtime state, the local CA, model data, and private config
are under ignored `.runtime/` paths.

## Start on this Mac

1. Start Colima with at least four CPUs and 6 GiB RAM:

   ```sh
   colima start --arch aarch64 --vm-type vz --cpus 4 --memory 6 --disk 30
   ```

2. Confirm the Mac's current LAN address and encode it as an `sslip.io` host
   (for example, `192-168-1-10.sslip.io`) in `.runtime/endpoint.env` and
   `.runtime/data/.config.yaml`, using the committed example files as the
   templates. This keeps the connection on the LAN while giving TLS a DNS name
   for SNI; it still requires working DNS.
3. Put the pinned Silero VAD ONNX file at
   `.runtime/models/snakers4_silero-vad/src/silero_vad/data/silero_vad.onnx`.
   The verified input is
   `https://raw.githubusercontent.com/snakers4/silero-vad/41f03a954b841327835dea1ddb7bb28ae23ddc2c/src/silero_vad/data/silero_vad.onnx`,
   2,327,524 bytes, SHA-256
   `1a153a22f4509e292a94e67d6f9b85e8deb25b4988682b7e174c65279d8788e3`.
4. Start the services from this directory:

   ```sh
   docker-compose --env-file .runtime/endpoint.env up -d
   ```

5. Caddy creates its local root at
   `.runtime/caddy-data/caddy/pki/authorities/local/root.crt`. Copy that PEM
   into the ignored probe `LocalBackendConfig.h` as shown by
   `LocalBackendConfig.example.h`, build, upload, and monitor the diagnostic.

Use `docker-compose --env-file .runtime/endpoint.env ps` for status,
`docker-compose --env-file .runtime/endpoint.env logs --tail 100` for logs, and
`docker-compose --env-file .runtime/endpoint.env down` to stop it. `down` does
not remove `.runtime/` state.

## Moving it later

The service can move to a LAN server, NAS, or VPS. Change the advertised
`server.websocket` URL and Caddy site name, install or regenerate the relevant
certificate, and rebuild the diagnostic with the matching bootstrap URL and CA.
Device identity in NVS is unaffected. A public domain with a publicly trusted
certificate is preferred for an off-LAN deployment; never fall back to plain
HTTP/WS or disabled certificate verification.
