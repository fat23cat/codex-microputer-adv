#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
DEPS_ROOT="${MICROPUTER_DEPS_ROOT:-$PROJECT/.deps}"
IDF_PATH="${IDF_PATH:-$DEPS_ROOT/esp-idf}"
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$DEPS_ROOT/idf-tools}"
M5CARDPUTER_DEMO_PATH="${M5CARDPUTER_DEMO_PATH:-$DEPS_ROOT/M5Cardputer-UserDemo}"

IDF_TAG="v5.5.3"
M5CARDPUTER_COMMIT="b549eac0a3c65bc108186c276b8fac0a214aaa4e"

mkdir -p "$DEPS_ROOT"

if [[ ! -d "$IDF_PATH/.git" ]]; then
    git clone --filter=blob:none --branch "$IDF_TAG" --recurse-submodules \
        https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi
git -C "$IDF_PATH" fetch --depth 1 origin tag "$IDF_TAG"
git -C "$IDF_PATH" checkout --detach "$IDF_TAG"
git -C "$IDF_PATH" submodule update --init --recursive

if [[ ! -d "$M5CARDPUTER_DEMO_PATH/.git" ]]; then
    git clone --filter=blob:none https://github.com/m5stack/M5Cardputer-UserDemo.git \
        "$M5CARDPUTER_DEMO_PATH"
fi
git -C "$M5CARDPUTER_DEMO_PATH" fetch --depth 1 origin "$M5CARDPUTER_COMMIT"
git -C "$M5CARDPUTER_DEMO_PATH" checkout --detach "$M5CARDPUTER_COMMIT"

export IDF_PATH IDF_TOOLS_PATH M5CARDPUTER_DEMO_PATH
"$IDF_PATH/install.sh" esp32s3
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1
"$HERE/prepare-idf.sh"

printf '\nSetup complete. Run ./tools/test.sh && ./tools/build.sh\n'
