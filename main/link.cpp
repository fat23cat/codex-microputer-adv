#include "link.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "ble_transport.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "model.h"

namespace hostlink {
namespace {

char   rx[512]     = {};
size_t rx_length   = 0;
uint32_t last_rx_ms = 0;
uint32_t last_usb_rx_ms = 0;
bool     saw_usb_host = false;

// A host that has stopped talking for this long is treated as gone, so the
// panel stops claiming a USB link that is really just a charger.
constexpr uint32_t kUsbIdleMs = 4000;

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_log_timestamp());
}

}  // namespace

void init()
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.tx_buffer_size = 2048;
    config.rx_buffer_size = 2048;
    usb_serial_jtag_driver_install(&config);
    // Start offline. A USB cable that is only supplying power must not read as
    // a live diagnostic host link until the serial peer actually says something.
    last_rx_ms = last_usb_rx_ms = 0;
}

void poll()
{
    char incoming[128];
    const int count = usb_serial_jtag_read_bytes(incoming, sizeof(incoming), 0);
    if (count <= 0) return;
    saw_usb_host = true;
    last_usb_rx_ms = last_rx_ms = now_ms();
    for (int i = 0; i < count; ++i) {
        const char ch = incoming[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            rx[rx_length] = 0;
            if (rx_length > 0) handle_line(rx);
            rx_length = 0;
        } else if (rx_length + 1 < sizeof(rx)) {
            rx[rx_length++] = ch;
        } else {
            rx_length = 0;
        }
    }
}

void sendf(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length <= 0) return;
    const size_t size = static_cast<size_t>(length) < sizeof(line) - 1
                      ? static_cast<size_t>(length) : sizeof(line) - 1;

    std::fwrite(line, 1, size, stdout);
    std::fflush(stdout);
    companion_ble_send(line, size);
}

void send_usb(const char* data, size_t size)
{
    if (!data || size == 0) return;
    std::fwrite(data, 1, size, stdout);
    std::fflush(stdout);
}

void begin_usb_bulk()
{
    // stdout is shared with ESP-IDF diagnostics and callbacks on the other
    // core. Hold its recursive FILE lock for the complete framed transfer.
    flockfile(stdout);
}

void end_usb_bulk()
{
    std::fflush(stdout);
    funlockfile(stdout);
}

void emit(const char* test, bool ok, const char* detail)
{
    sendf("CCP_PROBE {\"test\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}\n",
          test, ok ? "true" : "false", detail ? detail : "");
}

bool usb_active() { return saw_usb_host && now_ms() - last_usb_rx_ms < kUsbIdleMs; }

bool ble_active() { return companion_ble_connected(); }

void note_host_activity() { last_rx_ms = now_ms(); }

uint32_t silence_ms()
{
    // Before the first valid host message, elapsed uptime is not host silence.
    // Returning the maximum keeps a merely mounted transport on the splash.
    return last_rx_ms == 0 ? UINT32_MAX : now_ms() - last_rx_ms;
}

}  // namespace hostlink
