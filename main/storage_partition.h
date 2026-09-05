// Runtime discovery for the M5Apps-owned NVS partition. M5Apps releases use
// `apps_nvs`, while other compatible loaders may retain the conventional
// `nvs` label. Partition type/subtype is the stable contract.
#pragma once

#include "esp_partition.h"

namespace storage_partition {

inline const esp_partition_t* nvs()
{
    static const esp_partition_t* partition = [] {
        const esp_partition_t* preferred = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "apps_nvs");
        return preferred ? preferred : esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    }();
    return partition;
}

inline const char* nvs_label()
{
    const esp_partition_t* partition = nvs();
    return partition ? partition->label : nullptr;
}

} // namespace storage_partition
