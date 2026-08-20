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
constexpr uint32_t kWriteDebounceMs = 1500;
bool ready = false;
bool settings_dirty = false;
uint32_t dirty_since_ms = 0;
uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_log_timestamp());
}
nvs_handle_t open(nvs_open_mode_t mode)
{
    nvs_handle_t handle = 0;
    if (!ready)
        return 0;
    return nvs_open_from_partition(kPartition, kNamespace, mode, &handle) == ESP_OK ? handle : 0;
}
esp_err_t write_settings()
{
    nvs_handle_t h = open(NVS_READWRITE);
    if (!h)
        return ESP_FAIL;
    const auto& s = model::state;
    esp_err_t e = nvs_set_u8(h, "volume_v4", s.sound_volume);
    if (e == ESP_OK)
        e = nvs_set_u8(h, "startup", s.startup_sound_on ? 1 : 0);
    if (e == ESP_OK)
        e = nvs_set_u8(h, "ble_slot", s.ble_profile);
    if (e == ESP_OK)
        e = nvs_set_u8(h, "usb_hid", s.usb_hid_enabled ? 1 : 0);
    if (e == ESP_OK)
        e = nvs_set_u8(h, "chime_d3", s.startup_chime);
    if (e == ESP_OK)
        e = nvs_set_u16(h, "status_d2", s.status_debounce_ms);
    if (e == ESP_OK)
        e = nvs_set_i16(h, "audio_of3", s.status_audio_offset_ms);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e;
}
bool commit_dirty()
{
    const esp_err_t e = write_settings();
    if (e == ESP_OK) {
        settings_dirty = false;
        return true;
    }
    settings_dirty = true;
    dirty_since_ms = now_ms();
    std::printf("CCP_STORE|settings|write_failed|err=%s\n", esp_err_to_name(e));
    return false;
}
} // namespace
void init()
{
    const esp_err_t e = nvs_flash_init_partition(kPartition);
    ready = e == ESP_OK;
    if (!ready) {
        std::printf("CCP_STORE|init|failed|err=%s\n", esp_err_to_name(e));
        return;
    }
    load_settings();
}
void load_settings()
{
    nvs_handle_t h = open(NVS_READONLY);
    if (!h)
        return;
    auto& s = model::state;
    uint8_t v = 0;
    if (nvs_get_u8(h, "volume_v4", &v) == ESP_OK && v <= 100) {
        s.sound_volume = v;
        if (v > 0)
            s.unmuted_volume = v;
    }
    if (nvs_get_u8(h, "startup", &v) == ESP_OK)
        s.startup_sound_on = v != 0;
    if (nvs_get_u8(h, "ble_slot", &v) == ESP_OK && v < 3)
        s.ble_profile = v;
    if (nvs_get_u8(h, "usb_hid", &v) == ESP_OK)
        s.usb_hid_enabled = v != 0;
    if (nvs_get_u8(h, "chime_d3", &v) == ESP_OK && v < 10)
        s.startup_chime = v;
    uint16_t d = 0;
    if (nvs_get_u16(h, "status_d2", &d) == ESP_OK && d >= 100 && d <= 500)
        s.status_debounce_ms = d;
    int16_t o = 200;
    if (nvs_get_i16(h, "audio_of3", &o) == ESP_OK && o >= -300 && o <= 300)
        s.status_audio_offset_ms = o;
    nvs_close(h);
}
void save_settings()
{
    settings_dirty = true;
    dirty_since_ms = now_ms();
}
void service()
{
    if (settings_dirty && now_ms() - dirty_since_ms >= kWriteDebounceMs)
        commit_dirty();
}
bool flush()
{
    settings_dirty = true;
    return commit_dirty();
}
} // namespace store
