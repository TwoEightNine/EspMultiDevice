#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
arduino_cli="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
build_dir="${ESP_MEDIA_ASSISTANT_CONTRACT_BUILD_DIR:-/private/tmp/esp-media-assistant-contract-build}"
fqbn='espressif:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CPUFreq=240,USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default'
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
  --build-property 'compiler.cpp.extra_flags=-DESP_MEDIA_ASSISTANT_CONTRACT=1' \
  --build-path "$build_dir" \
  --warnings default \
  .

firmware_bin="$build_dir/esp_media.ino.bin"
firmware_size="$(wc -c < "$firmware_bin" | tr -d ' ')"
if (( firmware_size > application_limit )); then
  echo "assistant contract is $firmware_size bytes; slot limit is $application_limit bytes" >&2
  exit 4
fi
echo "verified assistant contract: $firmware_size / $application_limit bytes"
