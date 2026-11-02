# Shared arduino-cli setup for the tools/build_*.sh scripts; source it, do not run it.
#
# Environment:
#   ARDUINO_CLI         arduino-cli executable (default: PATH, then the macOS IDE bundle)
#   ESP_MEDIA_FQBN      board ID with options (default: the installed esp32 package)
#   ESP_MEDIA_PROFILE   sketch.yaml profile to build instead of the installed board
#                       package; --profile on the command line selects esp32s3

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
application_limit=6553600
board_options='FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CPUFreq=240,USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default'
profile="${ESP_MEDIA_PROFILE:-}"

for argument in "$@"; do
  case "$argument" in
    --profile) profile=esp32s3 ;;
    *) echo "unknown option: $argument (expected --profile)" >&2; exit 2 ;;
  esac
done

find_arduino_cli() {
  if [[ -n "${ARDUINO_CLI:-}" ]]; then
    echo "$ARDUINO_CLI"
  elif command -v arduino-cli > /dev/null 2>&1; then
    command -v arduino-cli
  else
    echo "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
  fi
}

arduino_cli="$(find_arduino_cli)"
if [[ ! -x "$arduino_cli" ]]; then
  echo "arduino-cli not found; install it or set ARDUINO_CLI to its absolute path" >&2
  exit 2
fi

# Boards Manager installs the core as esp32:esp32; a Git checkout under the
# sketchbook's hardware/espressif/ folder appears as espressif:esp32.
detect_fqbn() {
  local package
  package="$("$arduino_cli" core list | awk '$1 == "espressif:esp32" || $1 == "esp32:esp32" { print $1 }' | sort | head -n 1)"
  if [[ -z "$package" ]]; then
    echo "no esp32 board package installed; install esp32:esp32 or use --profile" >&2
    exit 2
  fi
  echo "$package:esp32s3:$board_options"
}

# Usage: compile_sketch <build dir> [extra arduino-cli arguments...]
compile_sketch() {
  local build_dir="$1"
  shift
  local target
  if [[ -n "$profile" ]]; then
    target=(--profile "$profile")
  else
    target=(--fqbn "${ESP_MEDIA_FQBN:-$(detect_fqbn)}")
  fi
  mkdir -p "$build_dir"
  cd "$project_dir"
  "$arduino_cli" compile \
    "${target[@]}" \
    --build-property "upload.maximum_size=$application_limit" \
    --build-path "$build_dir" \
    --warnings default \
    "$@" \
    .
}

# Usage: binary_size <build dir>; fails when the expected binary is missing.
binary_size() {
  local binary="$1/esp_media.ino.bin"
  if [[ ! -f "$binary" ]]; then
    echo "expected binary was not produced: $binary" >&2
    exit 3
  fi
  wc -c < "$binary" | tr -d ' '
}

default_build_dir() {
  echo "${TMPDIR:-/tmp}/esp-media-$1-build"
}
