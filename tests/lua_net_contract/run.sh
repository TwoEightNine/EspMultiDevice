#!/usr/bin/env bash
# Builds and runs the SDK 6 JSON/text contract test on the host.
# Usage: tests/lua_net_contract/run.sh [path/to/catalog.json]
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/esp-media-lua-net-contract"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  "$project_dir/tests/lua_net_contract/lua_net_contract.cpp" \
  "$project_dir/src/features/apps/JsonPick.cpp" \
  "$project_dir/src/features/apps/AppText.cpp" \
  -o "$build_dir/lua_net_contract"
"$build_dir/lua_net_contract" "$@"
