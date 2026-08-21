# Changelog

## 0.4.7 — 2026-08-21

- Made the firmware version single-sourced from the build: `PROJECT_VER` in
  the root CMakeLists now feeds `CCP_HELLO`, `CCP_PONG`, and the OTA app
  descriptor, which had drifted apart.
- Discarded any pending diagnostic task batch when a new host session opens
  (`HOST|`), so a stale partial batch can no longer merge into the next deck,
  and made mid-batch slot overwrites observable without dropping the batch.
- Flushed pending settings before returning to M5Apps and verified the
  rollback flush when disabling USB HID fails, so neither path can lose the
  last change or resurrect a disabled mode after reboot.
- Stopped rewriting settings NVS on every host `CFG|`/`OPT|` exchange; those
  values are host-owned and were never persisted anyway.
- Replaced scattered host lamp colour literals with the byte-exact named
  constants in `main/lamp.h`.
- Removed dead BLE text-channel entry points that never had callers.

## 0.4.6 — 2026-08-21

- Treat the first Codex lighting snapshot as restored state, so reconnecting or
  rebooting cannot turn every previously completed task unread green.
- Preserve green unread feedback for real active-to-completed transitions.

## 0.4.5 — 2026-08-21

- Moved long status scores and short interface cues onto separate hardware
  mixer channels so navigation cannot cut off notifications.
- Retained and retried an armed status buffer when the speaker temporarily
  cannot accept it instead of silently advancing the animation.

## 0.4.4 — 2026-08-21

- Made selected-completion read state self-healing on every native status
  snapshot and added an animation-independent settlement fail-safe.

## 0.4.3 — 2026-08-21

- Fixed selected tasks remaining unread green when they complete during the
  short local-selection guard window.

## 0.4.2 — 2026-08-21

- Hardened native USB/BLE session ownership so one gesture cannot cross hosts
  or survive a transport reset.
- Moved HID input processing out of transport callbacks and protected release
  edges from queue pressure.
- Made settings and BLE bond persistence transactional without erasing the
  shared M5Apps NVS partition.
- Added reliable M5Apps partition discovery, retrying flash writes, read-back
  verification, and OTA selection to the development installer.
- Corrected completed-task read state: an active completion plays green and
  settles to viewed grey, while a background completion remains green until
  selected.
- Added sanitizer regressions, installer tests, handshake tests, public-tree
  auditing, and a clean ESP-IDF firmware build to CI.
- Added project formatting rules and restored readable release sources.

The release remains an independent community implementation of the native
Codex Micro protocol. No bridge, daemon, API key, or Wi-Fi connection is
required.

## 0.1.0 — 2026-08-18

- Initial public release.
