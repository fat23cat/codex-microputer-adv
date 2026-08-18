#pragma once

#include <cstddef>
#include <cstdint>

// Native Work Louder / Codex Micro RPC carried by HID report 6.
namespace codex_micro {

using SendReport = bool (*)(const uint8_t* body, size_t length);

void init(SendReport sender);
void receive_report(const uint8_t* body, size_t length);
// Start a fresh control-plane baseline after a real session loss. Lighting
// snapshots may update the deck during this phase but can never become events.
void begin_session_sync();
// Temporary lighting previews can use the same breath effect as host selection.
// Guard selection inference before an interaction that opens such a preview.
void suppress_host_selection(uint32_t duration_ms);
bool send_key(const char* key, int action, int agent = -1);
bool send_joystick(float angle, float distance);

}  // namespace codex_micro
