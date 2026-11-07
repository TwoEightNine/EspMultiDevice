#!/usr/bin/env bash
# Renders every benchmark scene on the host, checks that band order does not
# change a frame, and writes PPM previews to the build directory.
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/esp-media-benchmark-render"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I"$project_dir/tests/benchmark_render/stubs" \
  "$project_dir/tests/benchmark_render/benchmark_render.cpp" \
  "$project_dir/src/features/settings/benchmark/BenchmarkRaster.cpp" \
  "$project_dir/src/features/settings/benchmark/BenchmarkScenes.cpp" \
  -o "$build_dir/benchmark_render"
"$build_dir/benchmark_render" "$build_dir"
