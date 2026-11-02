#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/arduino_build_common.sh"

build_dir="${ESP_MEDIA_ASSISTANT_CONTRACT_BUILD_DIR:-$(default_build_dir assistant-contract)}"
compile_sketch "$build_dir" \
  --build-property 'compiler.cpp.extra_flags=-DESP_MEDIA_ASSISTANT_CONTRACT=1'

firmware_size="$(binary_size "$build_dir")"
if (( firmware_size > application_limit )); then
  echo "assistant contract is $firmware_size bytes; slot limit is $application_limit bytes" >&2
  exit 4
fi
echo "verified assistant contract: $firmware_size / $application_limit bytes"
