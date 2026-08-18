// BLE bond storage inside the shared `apps_nvs` partition.
//
// NimBLE's stock store opens the default `nvs` partition by its hardcoded name.
// M5Apps does not create one, and adding it would be undone the next time the
// user reinstalls M5Apps (the installer calls makeDefaultPartitions()). So the
// companion keeps its bonds in the partition M5Apps already guarantees, under
// the one namespace this app owns.
//
// Records live in RAM and the whole array is rewritten on change. With at most
// eight bonds that is a few hundred bytes per commit, and pairing is rare.

#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "nvs.h"

namespace {

constexpr char kTag[]       = "ccp-bond";
constexpr char kPartition[] = "apps_nvs";
constexpr char kNamespace[] = "codex_ccp2";
constexpr char kKeyOurSec[]  = "ble_our";
constexpr char kKeyPeerSec[] = "ble_peer";
constexpr char kKeyCccd[]    = "ble_cccd";
constexpr char kSchemaKey[]  = "schema";
constexpr uint8_t kSchemaVersion = 3;

constexpr int kMaxBonds = 8;
constexpr int kMaxCccds = 8;

ble_store_value_sec  our_secs[kMaxBonds];
ble_store_value_sec  peer_secs[kMaxBonds];
ble_store_value_cccd cccds[kMaxCccds];
int our_sec_count  = 0;
int peer_sec_count = 0;
int cccd_count     = 0;

void persist(const char* key, const void* data, size_t size, int count)
{
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    char count_key[16];
    std::snprintf(count_key, sizeof(count_key), "%s_n", key);
    nvs_set_u8(handle, count_key, static_cast<uint8_t>(count));
    if (count > 0) nvs_set_blob(handle, key, data, size * count);
    else           nvs_erase_key(handle, key);
    nvs_commit(handle);
    nvs_close(handle);
}

void restore(const char* key, void* data, size_t size, int max_count, int* count)
{
    *count = 0;
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    char count_key[16];
    std::snprintf(count_key, sizeof(count_key), "%s_n", key);
    uint8_t stored = 0;
    if (nvs_get_u8(handle, count_key, &stored) == ESP_OK && stored > 0) {
        if (stored > max_count) stored = static_cast<uint8_t>(max_count);
        size_t length = size * stored;
        if (nvs_get_blob(handle, key, data, &length) == ESP_OK && length == size * stored) {
            *count = stored;
        }
    }
    nvs_close(handle);
}

// --- lookup, matching the semantics NimBLE's own store implements ----------
// An all-zero peer address is the documented "do not key off peer" wildcard.
bool keys_off_peer(const ble_addr_t& address)
{
    static const ble_addr_t none = {};
    return std::memcmp(&address, &none, sizeof(ble_addr_t)) != 0;
}

int find_sec(const ble_store_key_sec* key, const ble_store_value_sec* values, int count)
{
    int skipped = 0;
    for (int i = 0; i < count; ++i) {
        if (keys_off_peer(key->peer_addr)) {
            if (ble_addr_cmp(&values[i].peer_addr, &key->peer_addr) != 0) continue;
        }
        if (key->idx > skipped) { ++skipped; continue; }
        return i;
    }
    return -1;
}

int find_cccd(const ble_store_key_cccd* key)
{
    int skipped = 0;
    for (int i = 0; i < cccd_count; ++i) {
        if (keys_off_peer(key->peer_addr)) {
            if (ble_addr_cmp(&cccds[i].peer_addr, &key->peer_addr) != 0) continue;
        }
        if (key->chr_val_handle != 0) {
            if (cccds[i].chr_val_handle != key->chr_val_handle) continue;
        }
        if (key->idx > skipped) { ++skipped; continue; }
        return i;
    }
    return -1;
}

void save_sec(bool peer)
{
    persist(peer ? kKeyPeerSec : kKeyOurSec,
            peer ? peer_secs : our_secs,
            sizeof(ble_store_value_sec),
            peer ? peer_sec_count : our_sec_count);
}

int write_sec(bool peer, const ble_store_value_sec* value)
{
    ble_store_value_sec* values = peer ? peer_secs : our_secs;
    int* count = peer ? &peer_sec_count : &our_sec_count;

    // A re-pair with a known peer replaces its record rather than adding one.
    ble_store_key_sec key = {};
    ble_store_key_from_value_sec(&key, value);
    int index = find_sec(&key, values, *count);
    if (index < 0) {
        if (*count >= kMaxBonds) {
            ESP_LOGW(kTag, "bond table full");
            return BLE_HS_ESTORE_CAP;
        }
        index = (*count)++;
    }
    values[index] = *value;
    save_sec(peer);
    return 0;
}

int delete_sec(bool peer, const ble_store_key_sec* key)
{
    ble_store_value_sec* values = peer ? peer_secs : our_secs;
    int* count = peer ? &peer_sec_count : &our_sec_count;
    const int index = find_sec(key, values, *count);
    if (index < 0) return BLE_HS_ENOENT;
    std::memmove(&values[index], &values[index + 1],
                 sizeof(ble_store_value_sec) * (*count - index - 1));
    --(*count);
    save_sec(peer);
    return 0;
}

int store_read(int type, const ble_store_key* key, ble_store_value* value)
{
    switch (type) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:
        case BLE_STORE_OBJ_TYPE_PEER_SEC: {
            const bool peer = (type == BLE_STORE_OBJ_TYPE_PEER_SEC);
            const int index = find_sec(&key->sec, peer ? peer_secs : our_secs,
                                       peer ? peer_sec_count : our_sec_count);
            if (index < 0) return BLE_HS_ENOENT;
            value->sec = peer ? peer_secs[index] : our_secs[index];
            return 0;
        }
        case BLE_STORE_OBJ_TYPE_CCCD: {
            const int index = find_cccd(&key->cccd);
            if (index < 0) return BLE_HS_ENOENT;
            value->cccd = cccds[index];
            return 0;
        }
        default:
            return BLE_HS_ENOTSUP;
    }
}

int store_write(int type, const ble_store_value* value)
{
    switch (type) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:  return write_sec(false, &value->sec);
        case BLE_STORE_OBJ_TYPE_PEER_SEC: return write_sec(true,  &value->sec);
        case BLE_STORE_OBJ_TYPE_CCCD: {
            ble_store_key_cccd key = {};
            ble_store_key_from_value_cccd(&key, &value->cccd);
            int index = find_cccd(&key);
            if (index < 0) {
                if (cccd_count >= kMaxCccds) return BLE_HS_ESTORE_CAP;
                index = cccd_count++;
            }
            cccds[index] = value->cccd;
            persist(kKeyCccd, cccds, sizeof(ble_store_value_cccd), cccd_count);
            return 0;
        }
        default:
            return BLE_HS_ENOTSUP;
    }
}

int store_delete(int type, const ble_store_key* key)
{
    switch (type) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:  return delete_sec(false, &key->sec);
        case BLE_STORE_OBJ_TYPE_PEER_SEC: return delete_sec(true,  &key->sec);
        case BLE_STORE_OBJ_TYPE_CCCD: {
            const int index = find_cccd(&key->cccd);
            if (index < 0) return BLE_HS_ENOENT;
            std::memmove(&cccds[index], &cccds[index + 1],
                         sizeof(ble_store_value_cccd) * (cccd_count - index - 1));
            --cccd_count;
            persist(kKeyCccd, cccds, sizeof(ble_store_value_cccd), cccd_count);
            return 0;
        }
        default:
            return BLE_HS_ENOTSUP;
    }
}

}  // namespace

extern "C" void companion_ble_store_init(void)
{
    nvs_handle_t migration = 0;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &migration) == ESP_OK) {
        uint8_t schema = 0;
        if (nvs_get_u8(migration, kSchemaKey, &schema) != ESP_OK || schema != kSchemaVersion) {
            nvs_erase_all(migration);
            nvs_set_u8(migration, kSchemaKey, kSchemaVersion);
            nvs_commit(migration);
            ESP_LOGI(kTag, "cleared legacy BLE bonds");
        }
        nvs_close(migration);
    }
    restore(kKeyOurSec,  our_secs,  sizeof(ble_store_value_sec),  kMaxBonds, &our_sec_count);
    restore(kKeyPeerSec, peer_secs, sizeof(ble_store_value_sec),  kMaxBonds, &peer_sec_count);
    restore(kKeyCccd,    cccds,     sizeof(ble_store_value_cccd), kMaxCccds, &cccd_count);
    ESP_LOGI(kTag, "restored %d bonds, %d cccds", peer_sec_count, cccd_count);

    ble_hs_cfg.store_read_cb   = store_read;
    ble_hs_cfg.store_write_cb  = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
}

extern "C" int companion_ble_store_bond_count(void) { return peer_sec_count; }
