#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
test_tmp="$(mktemp -d "${TMPDIR:-/tmp}/codex-companion-tests.XXXXXX")"
trap 'rm -rf "$test_tmp"' EXIT

compiler="${CXX:-c++}"
for source in "$repo_dir"/tests/test_*.cpp; do
  name="$(basename "$source" .cpp)"
  "$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$repo_dir/main" "$source" -o "$test_tmp/$name"
  "$test_tmp/$name"
done

python3 "$repo_dir/tests/test_source_contracts.py"
python3 "$repo_dir/tests/test_screenshot_tool.py"
echo "PASS all host tests"
