#pragma once

#include <cstddef>
#include <cstdint>

void companion_usb_start();
bool companion_usb_set_enabled(bool enabled);
bool companion_usb_enabled();
void companion_usb_service();
bool companion_usb_connected();
bool companion_usb_send_rpc(const uint8_t* body, size_t length);
