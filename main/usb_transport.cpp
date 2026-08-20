#include "usb_transport.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "class/hid/hid_device.h"
#include "codex_micro_protocol.h"
#include "tinyusb.h"
#include "tusb.h"
namespace {
constexpr uint16_t kVid = 0x303a, kPid = 0x8360;
constexpr uint8_t kRpcReport = 6;
constexpr size_t kBody = 63;
constexpr auto kReportMap = std::to_array<uint8_t>(
    {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
     0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
     0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0, 0x06,
     0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x06, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95,
     0x3F, 0x09, 0x01, 0x81, 0x02, 0x95, 0x3F, 0x09, 0x02, 0x91, 0x02, 0xC0});
const tusb_desc_device_t kDeviceDescriptor = {.bLength = sizeof(tusb_desc_device_t),
                                              .bDescriptorType = TUSB_DESC_DEVICE,
                                              .bcdUSB = 0x0200,
                                              .bDeviceClass = 0,
                                              .bDeviceSubClass = 0,
                                              .bDeviceProtocol = 0,
                                              .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
                                              .idVendor = kVid,
                                              .idProduct = kPid,
                                              .bcdDevice = 0x0102,
                                              .iManufacturer = 1,
                                              .iProduct = 2,
                                              .iSerialNumber = 3,
                                              .bNumConfigurations = 1};
const char* kStrings[] = {(const char[]){0x09, 0x04}, "Work Louder", "Codex Micro ADV",
                          "CARDPUTER-ADV", "Codex Micro HID"};
constexpr uint16_t kConfigLength = TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN;
const uint8_t kConfiguration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, kConfigLength, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, kReportMap.size(), 0x81, 64, 1)};
struct Packet {
    uint32_t generation = 0;
    uint8_t body[kBody] = {};
};
constexpr int kQueueCapacity = 24;
Packet queue[kQueueCapacity] = {};
int queue_head = 0, queue_count = 0;
std::atomic<bool> mounted{false}, enabled{false}, installed{false}, queue_reset_requested{false};
std::atomic<uint32_t> generation{1};
void request_session_reset()
{
    generation.fetch_add(1, std::memory_order_acq_rel);
    queue_reset_requested.store(true, std::memory_order_release);
    codex_micro::reset_transport(codex_micro::Transport::Usb);
}
void clear_queue()
{
    queue_head = queue_count = 0;
}
void service_queue_reset()
{
    if (queue_reset_requested.exchange(false, std::memory_order_acq_rel))
        clear_queue();
}
} // namespace
extern "C" uint8_t const* tud_hid_descriptor_report_cb(uint8_t)
{
    return kReportMap.data();
}
extern "C" uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t*, uint16_t)
{
    return 0;
}
extern "C" void tud_hid_set_report_cb(uint8_t, uint8_t report_id, hid_report_type_t,
                                      uint8_t const* buffer, uint16_t size)
{
    if (!buffer)
        return;
    const uint8_t* body = nullptr;
    size_t length = 0;
    if (report_id == kRpcReport && size == kBody) {
        body = buffer;
        length = size;
    } else if (report_id == 0 && size == kBody + 1 && buffer[0] == kRpcReport) {
        body = buffer + 1;
        length = kBody;
    }
    if (body && !codex_micro::enqueue_report(codex_micro::Transport::Usb, body, length))
        std::printf("CCP_NATIVE|usb|rx_drop\n");
}
extern "C" void tud_mount_cb()
{
    mounted.store(enabled.load(std::memory_order_acquire), std::memory_order_release);
    request_session_reset();
}
extern "C" void tud_umount_cb()
{
    mounted.store(false, std::memory_order_release);
    request_session_reset();
}
bool companion_usb_set_enabled(bool requested)
{
    if (requested == enabled.load() && requested == installed.load())
        return true;
    if (!requested) {
        enabled.store(false);
        mounted.store(false);
        request_session_reset();
        if (!installed.load())
            return true;
        esp_err_t e = tinyusb_driver_uninstall();
        if (e == ESP_OK)
            installed.store(false);
        else
            enabled.store(true);
        std::printf("CCP_NATIVE|usb|disable|result=%s\n", esp_err_to_name(e));
        return e == ESP_OK;
    }
    const tinyusb_config_t config = {.device_descriptor = &kDeviceDescriptor,
                                     .string_descriptor = kStrings,
                                     .string_descriptor_count =
                                         sizeof(kStrings) / sizeof(kStrings[0]),
                                     .external_phy = false,
#if TUD_OPT_HIGH_SPEED
                                     .fs_configuration_descriptor = kConfiguration,
                                     .hs_configuration_descriptor = kConfiguration,
                                     .qualifier_descriptor = nullptr,
#else
                                     .configuration_descriptor = kConfiguration,
#endif
                                     .self_powered = false,
                                     .vbus_monitor_io = -1};
    esp_err_t e = tinyusb_driver_install(&config);
    if (e == ESP_OK) {
        installed.store(true);
        enabled.store(true);
        request_session_reset();
    }
    std::printf("CCP_NATIVE|usb|enable|result=%s\n", esp_err_to_name(e));
    return e == ESP_OK;
}
void companion_usb_start()
{
    companion_usb_set_enabled(true);
}
bool companion_usb_enabled()
{
    return enabled.load();
}
bool companion_usb_connected()
{
    return companion_usb_enabled() && mounted.load() && tud_hid_ready();
}
bool companion_usb_send_rpc(const uint8_t* body, size_t length)
{
    service_queue_reset();
    if (!body || length != kBody || !companion_usb_connected() || queue_count >= kQueueCapacity)
        return false;
    int tail = (queue_head + queue_count) % kQueueCapacity;
    queue[tail].generation = generation.load();
    std::memcpy(queue[tail].body, body, kBody);
    ++queue_count;
    return true;
}
void companion_usb_service()
{
    service_queue_reset();
    if (!companion_usb_connected() || queue_count == 0)
        return;
    Packet packet = queue[queue_head];
    if (packet.generation != generation.load()) {
        clear_queue();
        return;
    }
    if (!tud_hid_report(kRpcReport, packet.body, kBody))
        return;
    if (packet.generation != generation.load()) {
        queue_reset_requested.store(true);
        return;
    }
    queue_head = (queue_head + 1) % kQueueCapacity;
    --queue_count;
}
