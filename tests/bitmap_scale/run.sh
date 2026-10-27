#!/usr/bin/env bash
# Builds and runs the SDK 6 bitmap scaler contract test on the host.
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/esp-media-bitmap-scale"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  "$project_dir/tests/bitmap_scale/bitmap_scale.cpp" \
  "$project_dir/src/features/apps/BitmapScale.cpp" \
  -o "$build_dir/bitmap_scale"
"$build_dir/bitmap_scale"
