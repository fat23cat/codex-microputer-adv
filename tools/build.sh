#!/usr/bin/env bash
# Build the companion and stage the M5Apps-installable artifact in dist/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"

# shellcheck disable=SC1091
source "$HERE/env.sh"
"$HERE/prepare-idf.sh"

cd "$PROJECT"
idf.py build

mkdir -p dist
# The filename becomes the partition label and the launcher entry, so it is
# part of the deliverable, not an incidental detail.
cp build/codex_microputer_adv.bin dist/Codex.bin

printf '\ndist/Codex.bin  %s bytes\n' "$(wc -c < dist/Codex.bin | tr -d ' ')"
printf 'Copy it to the SD card and install with M5Apps: Installer -> SD card -> Codex.bin\n'
