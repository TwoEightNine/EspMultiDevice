# Adding Assistant device commands

This guide is the implementation contract for a new action that the local
assistant can perform on ESP Media. V1 audio search/play is the reference
implementation. Read [architecture.md](architecture.md),
[services.md](services.md), and this guide before changing the command set.

## Command design

Start with one small, explicit capability. Write down its inputs, bounded
output, device-side authority, and failure cases before adding code. A command
must never accept an arbitrary path, URL, shell expression, or configuration
key from the LLM. Use opaque IDs for resources selected on the device, then
revalidate the selected resource immediately before performing the action.

Capability version 2 retains `search_audio`/`play_audio` and adds read-only
device status, persisted volume/LED settings, normal-SD image/video/file
search and selection, Battery Details navigation, and fixed-provider web
search. The client hello advertises `features.device_tools_version: 2` beside
`device_tools: true`; a backend serving a version-1 client exposes only the
two original audio commands. The version-2 backend validates a distinct
`device_result.result` shape for every command, including typed file/folder
matches, nullable unavailable telemetry, and `accepted: true` for terminal
launches. A command result must never contain a source path.

All local searches share one latest selection set: at most five matches,
120-second expiry, an eight-second scan deadline, and the audio scanner's
normal-storage privacy filter. A new search clears selections from every
search type. `search_files` may select an ordinary file for `show_file_info`
or a folder for `open_folder`; unsupported ordinary file types are included.
`search_web` accepts only a 96-byte query. Browser constructs the fixed
DuckDuckGo Lite URL on the UI side and excludes it from recents and logs.
The built-in recorder's `/voice_last.wav` remains discoverable through
`search_audio`; no recording-specific command exists.

Keep the command set deliberately narrow. If an action reads files, exposes
data, changes settings, sends network traffic, or starts playback/recording,
document the privacy boundary and require a user-visible confirmation when the
action cannot be safely inferred from the request.

## Firmware protocol

Add the command to `XiaozhiProtocol` first.

1. Add a bounded command kind and typed arguments in
   `src/services/assistant/XiaozhiProtocol.h`.
2. Extend `parseMessage()` in `XiaozhiProtocol.cpp`. Require the exact
   top-level keys, exact argument keys, UTF-8-safe bounded strings, a nonzero
   integer request ID, and the active session ID. Reject duplicate, unknown,
   malformed, oversized, and out-of-state input without executing it.
3. Extend the typed `device_result` builder with only the fields that the
   backend needs. Do not return raw file paths, credentials, secret contents,
   or internal error text.
4. Handle the command in `XiaozhiSession::handleDeviceCommand()`. Maintain one
   request per command ID and send one correlated result before publishing any
   UI event. Clear selections and request state on session close or reconnect.

Commands execute only while the turn is `Thinking`. The upstream backend emits
`tts start` before it finishes tool selection, so that signal only marks speech
as pending. Start the speaker only on the first binary audio packet. Ignore
late STT after the turn leaves `Listening`; do not send another `listen stop`
after the backend's auto VAD finalizes speech.

## Device implementation and UI handoff

Place device work in an assistant-owned service, as
`AssistantDeviceTools` does for audio. It runs on the session worker and must
not depend on `Navigator`, a screen, or drawing code.

- Use `SpiBusLock` for each SD transaction, release it between incremental
  work, and yield so touch polling remains responsive.
- Use the subsystem registry such as `FileFormats`; do not reimplement media
  extension or privacy rules in the command handler.
- Bound scans, queues, result counts, strings, and retained state. Return a
  typed safe error such as `busy` or `storage_unavailable` when a bound is
  reached.
- Revalidate removable storage, ownership, source type, and opaque selections
  at action time.

If a command changes screens or starts another audio owner, publish a
generation-tagged `XiaozhiSessionEvent`. `AssistantController` consumes that
event and `AssistantScreen` performs the safe deactivation and Navigator push.
No worker, backend message handler, or service may navigate directly.
Status and persisted-setting requests use a separate generation-tagged UI
request and correlated reply, so NVS, LED hardware, and UI-owned telemetry
are read or changed on the UI task before a device result is sent.

## Backend overlay

The pinned backend is extended only through
`tools/xiaozhi_local/server_overlay/` and its checksum-guarded
`apply_overlay.py` script.

1. Add a `ToolDefinition` and executor under `server_overlay/device_audio/`,
   or a similarly scoped command package. Use OpenAI function schemas with
   required fields and `additionalProperties: false`.
2. Register the executor and its inbound result handler in `apply_overlay.py`.
   Tool availability must depend on the device's authenticated capability in
   hello; keep `features.mcp` disabled.
3. Send exactly one `device_command` at a time per connection. Bind it to the
   active `session_id` and a nonzero request ID, enforce a timeout, and ignore
   delayed or mismatched results.
4. Validate the full incoming `device_result` shape before resuming the LLM.
   Do not trust a result merely because it names the correct command.
5. Do not log tool arguments, result payloads, filenames, selections, paths,
   or secret data. Log only the command name, outcome class, and safe metrics.
6. Remove or hide legacy tools that can compete with the new capability for
   eligible device sessions. Audio commands remove the backend's generic
   `play_music` tool.

If an accepted command takes over device playback or navigation, mark it as a
terminal device action. Give the result to the LLM so the tool call completes,
but suppress follow-up TTS that would compete for I2S or narrate an action
already visible on screen.

## Tests and release checks

Update both layers with a regression test for each new command.

- `tests/assistant_contract/assistant_contract.ino`: capability negotiation,
  exact command/result JSON, malformed and oversized input, session/request
  mismatch, duplicate IDs, and any generation-safe launch event.
- `tools/xiaozhi_local/server_overlay/tests/`: function schema, command
  forwarding, result validation, timeout, capability absence, terminal TTS
  behavior, and logging redaction.
- Exercise success, invalid input, unavailable source, cancellation,
  disconnect, and stale-result paths on hardware.

Before hardware testing, run:

```sh
tools/build_assistant_contract.sh
tools/build_esp32s3.sh
cd tools/xiaozhi_local
docker-compose --env-file .runtime/endpoint.env build xiaozhi
docker-compose --env-file .runtime/endpoint.env up -d xiaozhi
./check_backend.sh
```

Update [services.md](services.md) with user-visible behavior and privacy
limits, [architecture.md](architecture.md) with state/ownership changes, and
the local-backend README/config prompt when the LLM should use the command.
