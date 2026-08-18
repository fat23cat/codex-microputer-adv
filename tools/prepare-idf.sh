#!/usr/bin/env bash
# Apply the minimal ESP-IDF 5.5.3 HID fixes required by Codex Micro.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
PATCH="$PROJECT/patches/esp-idf-5.5.3-codex-micro.patch"

if [[ -z "${IDF_PATH:-}" ]]; then
  # shellcheck disable=SC1091
  source "$HERE/env.sh"
fi

if git -C "$IDF_PATH" apply --check --reverse "$PATCH" 2>/dev/null; then
  exit 0
fi

git -C "$IDF_PATH" apply --check "$PATCH"
git -C "$IDF_PATH" apply "$PATCH"
