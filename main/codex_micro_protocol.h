#pragma once

#include <cstddef>
#include <cstdint>

// Native Work Louder / Codex Micro RPC carried by HID report 6.
namespace codex_micro {

enum class Transport : uint8_t { None, Usb, Ble };

// Responses are always returned through the transport that supplied the RPC.
// User actions use active_transport(), which prefers a live USB Codex session
// and falls back to a live BLE session. A mounted HID interface alone is never
// enough to become authoritative.
using SendReport = bool (*)(Transport target, const uint8_t* body, size_t length);

constexpr uint32_t kSessionIdleMs = 90000;

void init(SendReport sender);

// Transport callbacks only enqueue immutable reports. service() owns parsing,
// model changes and UI changes and must be called from the main application
// task. This keeps TinyUSB and NimBLE callbacks outside presentation state.
bool enqueue_report(Transport source, const uint8_t* body, size_t length);
void service();

// Invalidate buffered fragments, queued reports and liveness for one transport.
// Safe from TinyUSB/NimBLE callback tasks.
void reset_transport(Transport source);

Transport active_transport();
bool session_alive(Transport source);
const char* transport_name(Transport source);

// Start a fresh control-plane baseline after a real session loss. Lighting
// snapshots may update the deck during this phase but can never become events.
void begin_session_sync();
// Temporary lighting previews can use the same breath effect as host selection.
// Guard selection inference before an interaction that opens such a preview.
void suppress_host_selection(uint32_t duration_ms);
bool send_key_to(Transport target, const char* key, int action, int agent = -1);
bool send_joystick_to(Transport target, float angle, float distance);
bool send_key(const char* key, int action, int agent = -1);
bool send_joystick(float angle, float distance);

}  // namespace codex_micro
