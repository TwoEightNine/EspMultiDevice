#!/usr/bin/env bash
# Builds and runs the AudioTags contract test on the host.
# Regenerate fixtures with tools/generate_audio_tag_fixtures.py.
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/esp-media-audio-tags"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  "$project_dir/tests/audio_tags/audio_tags.cpp" \
  "$project_dir/src/media/AudioTags.cpp" \
  -o "$build_dir/audio_tags"
"$build_dir/audio_tags" "$project_dir/tests/audio_tags/fixtures"
