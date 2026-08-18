#!/usr/bin/env bash
# Source this to get the pinned toolchain for Codex Microputer ADV.
# Resolve under both bash and zsh, whether sourced or executed.
_SELF="${BASH_SOURCE[0]:-${(%):-%x}}"
PROJECT="$(cd "$(dirname "$_SELF")/.." && pwd)"
DEPS_ROOT="${MICROPUTER_DEPS_ROOT:-$PROJECT/.deps}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$DEPS_ROOT/idf-tools}"
export IDF_PATH="${IDF_PATH:-$DEPS_ROOT/esp-idf}"
export M5CARDPUTER_DEMO_PATH="${M5CARDPUTER_DEMO_PATH:-$DEPS_ROOT/M5Cardputer-UserDemo}"

if [[ ! -f "$IDF_PATH/export.sh" || ! -d "$M5CARDPUTER_DEMO_PATH/components/M5Unified" ]]; then
    printf 'Dependencies are missing. Run ./tools/setup.sh first.\n' >&2
    return 1 2>/dev/null || exit 1
fi
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1
