#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/.." && pwd)";tmp="$(mktemp -d)";trap 'rm -rf "$tmp"' EXIT
compiler="${CXX:-c++}";flags=(-std=c++17 -Wall -Wextra -Werror -pedantic -I"$repo_dir/main")
if [[ "${SANITIZE:-0}" == "1" ]];then flags+=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined);fi
for source in "$repo_dir"/tests/test_*.cpp;do name="$(basename "$source" .cpp)";"$compiler" "${flags[@]}" "$source" -o "$tmp/$name";"$tmp/$name";done
export PYTHONDONTWRITEBYTECODE=1;python3 -m py_compile "$repo_dir"/tools/*.py "$repo_dir"/tests/*.py
for source in "$repo_dir"/tests/test_*.py;do python3 "$source";done
echo "PASS all host tests"
