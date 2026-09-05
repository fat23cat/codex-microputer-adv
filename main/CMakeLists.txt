idf_component_register(
    SRCS
        "main.cpp"
        "ui.cpp"
        "audio.cpp"
        "store.cpp"
        "link.cpp"
        "keys.cpp"
        "ble_transport.cpp"
        "usb_transport.cpp"
        "codex_micro_protocol.cpp"
        "ble_store_apps_nvs.cpp"
        "puzzle_unit.cpp"
        "$ENV{M5CARDPUTER_DEMO_PATH}/main/hal/utils/adafruit_tca8418/Adafruit_TCA8418.cpp"
    INCLUDE_DIRS
        "."
        "$ENV{M5CARDPUTER_DEMO_PATH}/main/hal/utils/adafruit_tca8418"
    REQUIRES M5Unified nvs_flash app_update esp_partition fatfs sdmmc driver bt esp_hid json esp_tinyusb led_strip esp_timer
)

set(HID_EXAMPLE_DIR "$ENV{IDF_PATH}/examples/bluetooth/esp_hid_device/main")
target_sources(${COMPONENT_LIB} PRIVATE "${HID_EXAMPLE_DIR}/esp_hid_gap.c")
target_include_directories(${COMPONENT_LIB} PRIVATE "${HID_EXAMPLE_DIR}")
