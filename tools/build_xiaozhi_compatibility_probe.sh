#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/arduino_build_common.sh"

build_dir="${ESP_MEDIA_XIAOZHI_PROBE_BUILD_DIR:-$(default_build_dir xiaozhi-probe)}"
diagnostic_limit=4718592
compile_sketch "$build_dir" \
  --build-property 'compiler.cpp.extra_flags=-DESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE=1'

firmware_size="$(binary_size "$build_dir")"
if (( firmware_size > application_limit )); then
  echo "diagnostic is $firmware_size bytes; slot limit is $application_limit bytes" >&2
  exit 4
fi
if (( firmware_size > diagnostic_limit )); then
  echo "diagnostic is $firmware_size bytes; Stage 1 gate is $diagnostic_limit bytes" >&2
  exit 5
fi
echo "verified Xiaozhi diagnostic: $firmware_size / $diagnostic_limit bytes (slot $application_limit)"
