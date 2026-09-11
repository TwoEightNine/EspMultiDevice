#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
arduino_cli="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
build_dir="${ESP_MEDIA_PROBE_BUILD_DIR:-/private/tmp/esp-media-assistant-probe-build}"
# The probe prints its hardware gates over the ESP32-S3's native USB CDC port.
# Keep this diagnostic-only; normal production firmware retains its profile.
fqbn='espressif:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CPUFreq=240,CDCOnBoot=cdc'
application_limit=6553600

if [[ ! -x "$arduino_cli" ]]; then
  echo "arduino-cli not found; set ARDUINO_CLI to its absolute path" >&2
  exit 2
fi

mkdir -p "$build_dir"
cd "$project_dir"
"$arduino_cli" compile \
  --fqbn "$fqbn" \
  --build-property "upload.maximum_size=$application_limit" \
  --build-property 'compiler.cpp.extra_flags=-DESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE=1' \
  --build-path "$build_dir" \
  --warnings default \
  .

firmware_bin="$build_dir/esp_media.ino.bin"
if [[ ! -f "$firmware_bin" ]]; then
  echo "expected diagnostic binary was not produced: $firmware_bin" >&2
  exit 3
fi

firmware_size="$(wc -c < "$firmware_bin" | tr -d ' ')"
if (( firmware_size > application_limit )); then
  echo "diagnostic is $firmware_size bytes; slot limit is $application_limit bytes" >&2
  exit 4
fi
echo "verified assistant diagnostic: $firmware_size / $application_limit bytes"
