#include "puzzle_unit.h"

#include <array>
#include <cstdio>

#include "sdkconfig.h"

#if CONFIG_CODEX_PUZZLE_ENABLED
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#endif

#include "model.h"
#include "puzzle_renderer.h"
#include "ui.h"

namespace puzzle_unit {
namespace {

constexpr uint32_t kFrameIntervalMs = 33;  // 30 fps ceiling
constexpr uint32_t kSelectionTravelMs = 220;

#if CONFIG_CODEX_PUZZLE_ENABLED
led_strip_handle_t strip = nullptr;
bool live = false;
bool frame_valid = false;
bool refresh_error_reported = false;
puzzle_renderer::Frame previous_frame{};
uint32_t last_frame_ms = 0;
int observed_selection = -1;
int travel_from = -1;
uint32_t travel_started_ms = 0;
bool selection_observed = false;

constexpr puzzle_renderer::Rotation configured_rotation()
{
#if CONFIG_CODEX_PUZZLE_ROTATION_90
    return puzzle_renderer::Rotation::Deg90;
#elif CONFIG_CODEX_PUZZLE_ROTATION_180
    return puzzle_renderer::Rotation::Deg180;
#elif CONFIG_CODEX_PUZZLE_ROTATION_270
    return puzzle_renderer::Rotation::Deg270;
#else
    return puzzle_renderer::Rotation::Deg0;
#endif
}

void hold_data_low()
{
    gpio_config_t pin_config = {};
    pin_config.pin_bit_mask = 1ULL << CONFIG_CODEX_PUZZLE_GPIO;
    pin_config.mode = GPIO_MODE_OUTPUT;
    pin_config.pull_up_en = GPIO_PULLUP_DISABLE;
    pin_config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    pin_config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&pin_config);
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_CODEX_PUZZLE_GPIO), 0);
}

esp_err_t create_strip()
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_CODEX_PUZZLE_GPIO,
        .max_leds = puzzle_renderer::kPixelCount,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            // A single 64-pixel panel does not need DMA.  Keeping RMT in its
            // small interrupt-driven mode also avoids competing with the LCD,
            // audio, and USB/BLE DMA users during boot.
            .with_dma = false,
        },
    };
    return led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
}

void disable()
{
    live = false;
    frame_valid = false;
    if (strip) {
        led_strip_del(strip);
        strip = nullptr;
    }
    hold_data_low();
}

void publish(const puzzle_renderer::Frame& frame)
{
    const auto rotation = configured_rotation();
    for (int y = 0; y < puzzle_renderer::kHeight; ++y) {
        for (int x = 0; x < puzzle_renderer::kWidth; ++x) {
            const auto& pixel = frame[puzzle_renderer::logical_index(x, y)];
            const std::size_t index = puzzle_renderer::wire_index(x, y, rotation);
            const esp_err_t error = led_strip_set_pixel(
                strip, index, pixel.r, pixel.g, pixel.b);
            if (error != ESP_OK) {
                if (!refresh_error_reported) {
                    std::printf("CCP_PUZZLE|set_pixel_failed|index=%u|error=%s\n",
                                static_cast<unsigned>(index), esp_err_to_name(error));
                    refresh_error_reported = true;
                }
                disable();
                return;
            }
        }
    }
    const esp_err_t error = led_strip_refresh(strip);
    if (error != ESP_OK) {
        if (!refresh_error_reported) {
            std::printf("CCP_PUZZLE|refresh_failed|error=%s\n", esp_err_to_name(error));
            refresh_error_reported = true;
        }
        disable();
        return;
    }
    previous_frame = frame;
    frame_valid = true;
    refresh_error_reported = false;
}
#endif

}  // namespace

void init()
{
#if CONFIG_CODEX_PUZZLE_ENABLED
    // Establish a quiet data line before RMT takes ownership.  Some WS2812
    // panels otherwise interpret a power-up edge as a bright random frame.
    hold_data_low();
    esp_rom_delay_us(100);

    const esp_err_t error = create_strip();
    if (error != ESP_OK) {
        std::printf("CCP_PUZZLE|init_failed|gpio=%d|error=%s\n",
                    CONFIG_CODEX_PUZZLE_GPIO, esp_err_to_name(error));
        hold_data_low();
        return;
    }
    const esp_err_t clear_error = led_strip_clear(strip);
    if (clear_error != ESP_OK) {
        std::printf("CCP_PUZZLE|clear_failed|error=%s\n", esp_err_to_name(clear_error));
        disable();
        return;
    }
    live = true;
    std::printf("CCP_PUZZLE|ready|gpio=%d|brightness=%d|dma=0\n",
                CONFIG_CODEX_PUZZLE_GPIO,
                CONFIG_CODEX_PUZZLE_MAX_BRIGHTNESS_PERCENT);
#endif
}

void service()
{
#if CONFIG_CODEX_PUZZLE_ENABLED
    if (!live) return;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now - last_frame_ms < kFrameIntervalMs) return;
    last_frame_ms = now;

    puzzle_renderer::Input input;
    input.linked = model::state.link != model::Link::Offline;
    input.selected = model::state.selected;
    input.phase = now / 1000.f;
    input.brightness = ui::effective_light_level()
        * (CONFIG_CODEX_PUZZLE_MAX_BRIGHTNESS_PERCENT / 100.f);
    for (int i = 0; i < puzzle_renderer::kSlotCount; ++i) {
        if (i >= model::state.task_count) continue;
        const auto& task = model::state.tasks[i];
        input.slots[i].present = task.present;
        input.slots[i].status = task.status;
        input.slots[i].unseen_done = task.unseen_done;
    }
    if (!input.linked) {
        selection_observed = false;
        travel_from = -1;
    } else if (!selection_observed) {
        observed_selection = input.selected;
        selection_observed = true;
    } else if (input.selected != observed_selection) {
        travel_from = observed_selection;
        observed_selection = input.selected;
        travel_started_ms = now;
    }
    if (travel_from >= 0 && now - travel_started_ms < kSelectionTravelMs) {
        input.selection_travel.active = true;
        input.selection_travel.from = travel_from;
        input.selection_travel.to = input.selected;
        input.selection_travel.progress = static_cast<float>(now - travel_started_ms)
                                        / kSelectionTravelMs;
    } else {
        travel_from = -1;
    }
    const ui::StatusTakeoverSnapshot takeover = ui::status_takeover_snapshot();
    input.takeover.active = takeover.active;
    input.takeover.slot = takeover.slot;
    input.takeover.status = takeover.status;
    input.takeover.unseen = takeover.unseen;
    input.takeover.viewed_progress = takeover.viewed_progress;
    input.takeover.age = takeover.age;

    const puzzle_renderer::Frame frame = puzzle_renderer::render(input);
    if (!frame_valid || frame != previous_frame) publish(frame);
#endif
}

}  // namespace puzzle_unit
