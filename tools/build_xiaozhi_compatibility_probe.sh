#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
arduino_cli="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
build_dir="${ESP_MEDIA_XIAOZHI_PROBE_BUILD_DIR:-/private/tmp/esp-media-xiaozhi-probe-build}"
fqbn='espressif:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CPUFreq=240,USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default'
application_limit=6553600
diagnostic_limit=4718592

if [[ ! -x "$arduino_cli" ]]; then
  echo "arduino-cli not found; set ARDUINO_CLI to its absolute path" >&2
  exit 2
fi

mkdir -p "$build_dir"
cd "$project_dir"
"$arduino_cli" compile \
  --fqbn "$fqbn" \
  --build-property "upload.maximum_size=$application_limit" \
  --build-property 'compiler.cpp.extra_flags=-DESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE=1' \
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
if (( firmware_size > diagnostic_limit )); then
  echo "diagnostic is $firmware_size bytes; Stage 1 gate is $diagnostic_limit bytes" >&2
  exit 5
fi
echo "verified Xiaozhi diagnostic: $firmware_size / $diagnostic_limit bytes (slot $application_limit)"
