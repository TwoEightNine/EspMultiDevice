#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/arduino_build_common.sh"

# The probe uses the same two-port profile as production and prints its
# hardware gates through UART0 on the board's USB-to-UART connector.
build_dir="${ESP_MEDIA_PROBE_BUILD_DIR:-$(default_build_dir assistant-probe)}"
compile_sketch "$build_dir" \
  --build-property 'compiler.cpp.extra_flags=-DESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE=1'

firmware_size="$(binary_size "$build_dir")"
if (( firmware_size > application_limit )); then
  echo "diagnostic is $firmware_size bytes; slot limit is $application_limit bytes" >&2
  exit 4
fi
echo "verified assistant diagnostic: $firmware_size / $application_limit bytes"
