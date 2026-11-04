#!/usr/bin/env bash
# Builds and runs the AppDisabledList (Settings → Apps persistence) contract
# test on the host.
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/esp-media-app-disabled-list"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$project_dir/src/services/apps" \
  "$project_dir/tests/app_disabled_list/app_disabled_list.cpp" \
  -o "$build_dir/app_disabled_list"
"$build_dir/app_disabled_list"
