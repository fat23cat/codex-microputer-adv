# Changelog

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
