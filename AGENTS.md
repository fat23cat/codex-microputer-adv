# AGENTS.md

## Verification contract (mandatory before claiming any firmware change works)

1. `./tools/test.sh` — host regression suite.
2. `python3 tools/audit_public_tree.py` — public-tree audit.
3. `./tools/build.sh` — full ESP-IDF build. Never skip: host tests do NOT
   compile `main/` firmware sources (`main.cpp`, `ui.cpp`, transports), so
   compile errors there are invisible without this step.
4. If a device is attached (`/dev/cu.usbmodem*`): flash and verify on hardware
   (`tools/install.py`, `tools/devlink.py --demo`, `tools/screenshot.py`),
   exercising the changed code paths over USB serial.

## Build/install environment gotchas

- Source `tools/env.sh` first; it sets `IDF_PATH`, `IDF_TOOLS_PATH`,
  `M5CARDPUTER_DEMO_PATH`. Without it `tools/install.py` flashes the image but
  fails the final OTA switch with "IDF_PATH is not set", leaving the old app
  running.
- First-time setup: `./tools/setup.sh`, then
  `python3 $IDF_PATH/tools/idf_tools.py install cmake ninja` if cmake/ninja
  are not on PATH.
- Run repo Python tools that need `esptool`/`pyserial` with the pinned IDF
  interpreter `.deps/idf-tools/python_env/*/bin/python`, not the system one.
- Firmware version lives only in root `CMakeLists.txt` (`PROJECT_VER`) and is
  read back via `esp_app_get_description()->version`; bump together with
  `CHANGELOG.md`.

## Repo conventions

- `tests/test_source_contracts.py` greps sources with regexes; when moving or
  renaming pinned code, update the contract in the same change.
- Testable logic lives in headers under `main/` and is covered by host tests;
  protocol wire values (e.g. host lamp colours in `main/lamp.h`) must stay
  byte-exact.
