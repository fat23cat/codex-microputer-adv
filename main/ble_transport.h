// BLE HID transport for the native Codex Micro protocol: keyboard plus a
// vendor RPC report collection. There is no newline-delimited text channel
// over BLE; diagnostic text lives only on the USB serial path (link.h).
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void companion_ble_start(void);
bool companion_ble_connected(void);
bool companion_ble_pairing_active(void);
uint32_t companion_ble_pairing_passkey(void);

// Returns false when the link is down or the notify queue rejected the payload,
// so callers can fall back to queueing instead of assuming delivery.
bool companion_ble_send(const char* data, size_t length);

// Number of hosts this device has bonded with.
int companion_ble_bond_count(void);
bool companion_ble_select_profile(uint8_t profile);
bool companion_codex_key(const char* key, int action, int agent = -1);
bool companion_codex_joystick(float angle, float distance);
void companion_ble_service(void);

// Implemented in main.cpp.
void companion_ble_link_changed(bool connected);

#ifdef __cplusplus
}
#endif
