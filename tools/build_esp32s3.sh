#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/arduino_build_common.sh"

build_dir="${ESP_MEDIA_BUILD_DIR:-$(default_build_dir firmware)}"
compile_sketch "$build_dir"

firmware_size="$(binary_size "$build_dir")"
if (( firmware_size > application_limit )); then
  echo "firmware is $firmware_size bytes; slot limit is $application_limit bytes" >&2
  exit 4
fi
echo "verified firmware binary: $firmware_size / $application_limit bytes"
