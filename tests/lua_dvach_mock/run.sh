#!/usr/bin/env bash
# Builds the host harness, writes the mock 2ch fixtures, and runs the dvach
# example app through a scripted session.
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/esp-media-dvach-mock"
mkdir -p "$build_dir/lua"
python3 -I "$project_dir/tools/mock_dvach.py" write "$build_dir/fixtures" > /dev/null
for source in "$project_dir"/third_party/lua/*.c; do
  case "$(basename "$source")" in lua.c|luac.c) continue ;; esac
  object="$build_dir/lua/$(basename "$source" .c).o"
  if [[ ! -f "$object" || "$source" -nt "$object" ]]; then
    "${CC:-cc}" -std=c99 -O1 -g -c "$source" -o "$object"
  fi
done
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  "$project_dir/tests/lua_dvach_mock/dvach_mock.cpp" \
  "$project_dir/src/features/apps/AppLuaJson.cpp" \
  "$project_dir/src/features/apps/JsonPick.cpp" \
  "$project_dir/src/features/apps/AppText.cpp" \
  "$build_dir"/lua/*.o -o "$build_dir/dvach_mock"
"$build_dir/dvach_mock" "$project_dir/examples/apps/dvach/main.lua" "$build_dir/fixtures"
