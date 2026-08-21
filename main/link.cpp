#include "link.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "ble_transport.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
namespace hostlink {
namespace {
// Line reassembly buffer for the USB serial diagnostic protocol. Lines of 512
// or more bytes are dropped whole until the next newline (reason=oversize).
// Contract: a well-formed OPT| line with eight options (kLabelMax=10,
// kWireMax=40) peaks near 450 bytes, and TASK titles are truncated to
// kTitleMax by the parser, so conforming hosts always fit.
char rx[512] = {};
size_t rx_length = 0;
bool discard_until_newline = false;
uint32_t last_rx_ms = 0, last_usb_rx_ms = 0;
bool saw_usb_host = false, driver_ready = false;
constexpr uint32_t kUsbIdleMs = 4000;
uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_log_timestamp());
}
void append_json(char* out, size_t cap, size_t& used, const char* text)
{
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : "");
         *p && used + 1 < cap; ++p) {
        const char* e = nullptr;
        switch (*p) {
        case '"':
            e = "\\\"";
            break;
        case '\\':
            e = "\\\\";
            break;
        case '\n':
            e = "\\n";
            break;
        case '\r':
            e = "\\r";
            break;
        case '\t':
            e = "\\t";
            break;
        }
        if (e) {
            size_t n = std::strlen(e);
            if (used + n >= cap)
                break;
            std::memcpy(out + used, e, n);
            used += n;
        } else if (*p >= 0x20)
            out[used++] = static_cast<char>(*p);
    }
    out[used] = 0;
}
} // namespace
void init()
{
    usb_serial_jtag_driver_config_t c = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    c.tx_buffer_size = 2048;
    c.rx_buffer_size = 2048;
    esp_err_t e = usb_serial_jtag_driver_install(&c);
    driver_ready = e == ESP_OK || e == ESP_ERR_INVALID_STATE;
    std::printf("CCP_NATIVE|serial|init|result=%s\n", esp_err_to_name(e));
}
void poll()
{
    if (!driver_ready)
        return;
    char incoming[128];
    int count = usb_serial_jtag_read_bytes(incoming, sizeof(incoming), 0);
    if (count <= 0)
        return;
    saw_usb_host = true;
    last_usb_rx_ms = last_rx_ms = now_ms();
    for (int i = 0; i < count; ++i) {
        char ch = incoming[i];
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            if (discard_until_newline)
                std::printf("CCP_SERIAL|line_dropped|reason=oversize\n");
            else {
                rx[rx_length] = 0;
                if (rx_length > 0)
                    handle_line(rx);
            }
            rx_length = 0;
            discard_until_newline = false;
        } else if (discard_until_newline)
            continue;
        else if (rx_length + 1 < sizeof(rx))
            rx[rx_length++] = ch;
        else {
            rx_length = 0;
            discard_until_newline = true;
        }
    }
}
void sendf(const char* format, ...)
{
    if (!format)
        return;
    char line[512];
    va_list args;
    va_start(args, format);
    int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length <= 0)
        return;
    size_t size = std::min(static_cast<size_t>(length), sizeof(line) - 1);
    std::fwrite(line, 1, size, stdout);
    std::fflush(stdout);
    companion_ble_send(line, size);
}
void send_usb(const char* data, size_t size)
{
    if (!data || !size)
        return;
    std::fwrite(data, 1, size, stdout);
    std::fflush(stdout);
}
void begin_usb_bulk()
{
    flockfile(stdout);
}
void end_usb_bulk()
{
    std::fflush(stdout);
    funlockfile(stdout);
}
void emit(const char* test, bool ok, const char* detail)
{
    char a[96] = {}, b[224] = {};
    size_t au = 0, bu = 0;
    append_json(a, sizeof(a), au, test);
    append_json(b, sizeof(b), bu, detail);
    sendf("CCP_PROBE {\"test\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}\n", a, ok ? "true" : "false", b);
}
bool usb_active()
{
    return saw_usb_host && now_ms() - last_usb_rx_ms < kUsbIdleMs;
}
bool ble_active()
{
    return companion_ble_connected();
}
void note_host_activity()
{
    last_rx_ms = now_ms();
}
uint32_t silence_ms()
{
    return last_rx_ms == 0 ? UINT32_MAX : now_ms() - last_rx_ms;
}
} // namespace hostlink
