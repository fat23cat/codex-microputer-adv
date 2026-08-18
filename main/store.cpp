#include "store.h"

#include <cstdio>

#include "esp_err.h"
#include "esp_log.h"
#include "model.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace store {
namespace {

constexpr char kPartition[] = "apps_nvs";
constexpr char kNamespace[] = "codex_ccp2";

bool     ready         = false;
bool     settings_dirty = false;
uint32_t dirty_since_ms = 0;

nvs_handle_t open(nvs_open_mode_t mode)
{
    nvs_handle_t handle = 0;
    if (!ready) return 0;
    if (nvs_open_from_partition(kPartition, kNamespace, mode, &handle) != ESP_OK) return 0;
    return handle;
}

}  // namespace

void init()
{
    // init_partition is idempotent and, unlike nvs_flash_erase, never destroys
    // the keys M5Apps keeps in the same partition.
    const esp_err_t err = nvs_flash_init_partition(kPartition);
    ready = (err == ESP_OK);
    if (!ready) return;
    load_settings();
}

void load_settings()
{
    nvs_handle_t handle = open(NVS_READONLY);
    if (!handle) return;
    auto& s = model::state;
    uint8_t value = 0;
    if (nvs_get_u8(handle, "volume_v4", &value) == ESP_OK && value <= 100) {
        s.sound_volume = value;
        if (value > 0) s.unmuted_volume = value;
    }
    if (nvs_get_u8(handle, "startup", &value) == ESP_OK) s.startup_sound_on = value != 0;
    if (nvs_get_u8(handle, "ble_slot", &value) == ESP_OK && value < 3) s.ble_profile = value;
    if (nvs_get_u8(handle, "usb_hid", &value) == ESP_OK) s.usb_hid_enabled = value != 0;
    if (nvs_get_u8(handle, "chime_d3", &value) == ESP_OK && value < 10) s.startup_chime = value;
    uint16_t debounce = 0;
    if (nvs_get_u16(handle, "status_d2", &debounce) == ESP_OK
        && debounce >= 100 && debounce <= 500)
        s.status_debounce_ms = debounce;
    int16_t audio_offset = 200;
    if (nvs_get_i16(handle, "audio_of3", &audio_offset) == ESP_OK
        && audio_offset >= -300 && audio_offset <= 300)
        s.status_audio_offset_ms = audio_offset;
    nvs_close(handle);
}

void save_settings()
{
    settings_dirty = true;
    dirty_since_ms = static_cast<uint32_t>(esp_log_timestamp());
}

void service()
{
    if (!settings_dirty) return;
    // Debounce: holding an arrow key through a wheel should cost one write, not
    // one per step. Flash endurance on a shared partition is worth protecting.
    if (static_cast<uint32_t>(esp_log_timestamp()) - dirty_since_ms < 1500) return;
    settings_dirty = false;
    nvs_handle_t handle = open(NVS_READWRITE);
    if (!handle) return;
    const auto& s = model::state;
    nvs_set_u8(handle, "volume_v4", s.sound_volume);
    nvs_set_u8(handle, "startup", s.startup_sound_on ? 1 : 0);
    nvs_set_u8(handle, "ble_slot", s.ble_profile);
    nvs_set_u8(handle, "usb_hid", s.usb_hid_enabled ? 1 : 0);
    nvs_set_u8(handle, "chime_d3", s.startup_chime);
    nvs_set_u16(handle, "status_d2", s.status_debounce_ms);
    nvs_set_i16(handle, "audio_of3", s.status_audio_offset_ms);
    nvs_commit(handle);
    nvs_close(handle);
}

void flush()
{
    settings_dirty = false;
    nvs_handle_t handle = open(NVS_READWRITE);
    if (!handle) return;
    const auto& s = model::state;
    nvs_set_u8(handle, "volume_v4", s.sound_volume);
    nvs_set_u8(handle, "startup", s.startup_sound_on ? 1 : 0);
    nvs_set_u8(handle, "ble_slot", s.ble_profile);
    nvs_set_u8(handle, "usb_hid", s.usb_hid_enabled ? 1 : 0);
    nvs_set_u8(handle, "chime_d3", s.startup_chime);
    nvs_set_u16(handle, "status_d2", s.status_debounce_ms);
    nvs_set_i16(handle, "audio_of3", s.status_audio_offset_ms);
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace store
