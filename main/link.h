// Host link: the same newline protocol over USB Serial/JTAG or BLE.
//
// USB wins whenever a host is actually talking on it, because that is the cable
// the user just plugged in. BLE takes over when USB goes quiet.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hostlink {

void init();

// Drains both transports and dispatches complete lines to handle_line().
void poll();

// printf-style. Writes to whichever transport is live; USB always gets a copy
// when a host is attached so the serial console stays useful for diagnostics.
void sendf(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Diagnostic bulk output must stay on USB: mirroring a screenshot over BLE
// would congest the native Codex HID session.
void send_usb(const char* data, size_t size);
void begin_usb_bulk();
void end_usb_bulk();

// Structured diagnostic record, mirrored to both transports.
void emit(const char* test, bool ok, const char* detail);

bool usb_active();
bool ble_active();

// Records inbound traffic that did not arrive through poll() (BLE writes).
void note_host_activity();

// Milliseconds since the host last sent us anything.
uint32_t silence_ms();

// Implemented by main.cpp.
void handle_line(char* line);

}  // namespace hostlink
