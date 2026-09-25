#!/usr/bin/env bash
# Build the companion and stage the loader-installable raw app image in dist/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"

# shellcheck disable=SC1091
source "$HERE/env.sh"
"$HERE/prepare-idf.sh"

cd "$PROJECT"
idf.py build

mkdir -p dist
# The stable filename is part of the SD update contract.
cp build/codex_microputer_adv.bin dist/Codex.bin

printf '\ndist/Codex.bin  %s bytes\n' "$(wc -c < dist/Codex.bin | tr -d ' ')"
printf 'Stage it with Cardputer Firmware Manager, then run upcodex in crub (flash /firmware/Codex.bin extra)\n'
