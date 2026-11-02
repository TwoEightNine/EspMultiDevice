#!/usr/bin/env python3
"""Rewrite quoted includes in vendored source trees to file-relative paths.

Arduino adds only the sketch root, the core, and libraries to the header
search path, and copies build_opt.h without expanding {build.source.path}.
Vendored code that expects its own -I directories therefore cannot find its
headers. This tool resolves each quoted include the way the upstream build
would and replaces it with a path relative to the including file, so the
compiler finds it in the current file's directory without any -I flag.

An include is rewritten only when it does not already resolve next to the
including file and it names a file that exists either relative to one of the
tree's search directories or, for a bare file name, as a header with a unique
name inside the tree. Anything else, such as architecture-specific headers
that the configuration compiles out, is left untouched. Running the tool
again on a localized tree changes nothing.

Usage, from the repository root after importing or upgrading a subset:

    python3 tools/localize_vendor_includes.py           # rewrite
    python3 tools/localize_vendor_includes.py --check   # report only
"""

import argparse
import os
import re
import sys
from collections import defaultdict

# Vendored tree -> upstream include directories, relative to the tree root.
TREES = {
    "third_party/ffmpeg": ["."],
    "third_party/opencore-amr": [],  # bare names are resolved by unique basename
}

SOURCE_SUFFIXES = (".c", ".cpp", ".h")
INCLUDE = re.compile(r'^(\s*#\s*include\s*")([^"]+)(")', re.M)


def header_index(tree: str) -> dict[str, list[str]]:
    index: dict[str, list[str]] = defaultdict(list)
    for directory, _, files in os.walk(tree):
        for name in files:
            if name.endswith(".h"):
                index[name].append(os.path.join(directory, name))
    return index


def resolve(tree: str, search_dirs: list[str], index: dict[str, list[str]],
            source: str, name: str) -> str | None:
    if os.path.exists(os.path.join(os.path.dirname(source), name)):
        return None
    for search in search_dirs:
        candidate = os.path.normpath(os.path.join(tree, search, name))
        if os.path.isfile(candidate):
            return candidate
    if "/" not in name and len(index.get(name, [])) == 1:
        return index[name][0]
    return None


def localize(tree: str, search_dirs: list[str], write: bool) -> tuple[int, int]:
    index = header_index(tree)
    changed_files = 0
    changed_includes = 0
    for directory, _, files in os.walk(tree):
        for file_name in files:
            if not file_name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(directory, file_name)
            with open(path, encoding="latin-1", newline="") as handle:
                text = handle.read()
            count = 0

            def replace(match: re.Match[str]) -> str:
                nonlocal count
                target = resolve(tree, search_dirs, index, path, match.group(2))
                if target is None:
                    return match.group(0)
                count += 1
                relative = os.path.relpath(target, directory).replace(os.sep, "/")
                return match.group(1) + relative + match.group(3)

            updated = INCLUDE.sub(replace, text)
            if count:
                changed_files += 1
                changed_includes += count
                if write:
                    with open(path, "w", encoding="latin-1", newline="") as handle:
                        handle.write(updated)
    return changed_files, changed_includes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="report includes that still need localizing; exit 1 if any")
    args = parser.parse_args()
    pending = 0
    for tree, search_dirs in TREES.items():
        files, includes = localize(tree, search_dirs, write=not args.check)
        pending += includes
        verb = "need localizing" if args.check else "localized"
        print(f"{tree}: {includes} includes in {files} files {verb}")
    return 1 if args.check and pending else 0


if __name__ == "__main__":
    sys.exit(main())
