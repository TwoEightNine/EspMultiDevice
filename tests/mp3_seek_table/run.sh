#!/usr/bin/env bash
# Builds and runs the Mp3SeekTable contract test on the host.
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/esp-media-mp3-seek-table"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  "$project_dir/tests/mp3_seek_table/mp3_seek_table.cpp" \
  "$project_dir/src/media/Mp3SeekTable.cpp" \
  -o "$build_dir/mp3_seek_table"
"$build_dir/mp3_seek_table"
