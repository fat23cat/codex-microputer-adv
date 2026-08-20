#include "keys.h"

#include <M5Unified.hpp>
#include <Adafruit_TCA8418.h>

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "input_event_queue.h"
#include "key_layout.h"

namespace keys {
namespace {

Backend active = Backend::None;
Adafruit_TCA8418 tca;
bool tca_option_down = false;
uint32_t tca_retry_at_ms = 0;

constexpr gpio_num_t kColumnSelect[3] = {GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11};
constexpr gpio_num_t kRowInput[7] = {
    GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
};
uint16_t held[4] = {};
uint32_t repeat_at_ms = 0;
int repeat_row = -1;
int repeat_col = -1;
uint32_t last_scan_ms = 0;

constexpr size_t kQueueCapacity = 24;
input_event_queue::Queue<Press, kQueueCapacity> queue;

bool repeatable(Key key)
{
    return key == Key::Up || key == Key::Down || key == Key::Left || key == Key::Right ||
           key == Key::EncoderLeft || key == Key::EncoderRight;
}

void push(Press press, bool repeat = false)
{
    const bool accepted = queue.push(
        press, repeat, [](const Press& event) { return !event.down; },
        [](const Press& event) {
            return event.down && (repeatable(event.key) || event.key == Key::Other);
        });
    if (!accepted && !repeat) {
        std::printf("CCP_KEY_QUEUE|drop|key=%s|edge=%s\n", name(press.key),
                    press.down ? "down" : "up");
    }
}

void release_held_matrix_keys()
{
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 14; ++col) {
            if (!(held[row] & (1u << col)))
                continue;
            Press release = map_layout(row, col);
            if (!needs_release(release.key))
                continue;
            release.down = false;
            push(release);
        }
    }
}

void matrix_init()
{
    for (gpio_num_t pin : kColumnSelect) {
        gpio_config_t config = {};
        config.pin_bit_mask = 1ULL << pin;
        config.mode = GPIO_MODE_OUTPUT;
        gpio_config(&config);
        gpio_set_level(pin, 0);
    }
    for (gpio_num_t pin : kRowInput) {
        gpio_config_t config = {};
        config.pin_bit_mask = 1ULL << pin;
        config.mode = GPIO_MODE_INPUT;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&config);
    }
}

void matrix_scan()
{
    const uint32_t now = lgfx::millis();
    if (now - last_scan_ms < 15)
        return;
    last_scan_ms = now;
    uint16_t fresh[4] = {};
    for (int column = 0; column < 8; ++column) {
        for (int bit = 0; bit < 3; ++bit)
            gpio_set_level(kColumnSelect[bit], (column >> bit) & 1);
        esp_rom_delay_us(20);
        for (int input = 0; input < 7; ++input) {
            if (gpio_get_level(kRowInput[input]) != 0)
                continue;
            const int col = (column > 3) ? input + 7 : input;
            const int row = (column > 3) ? column - 4 : column;
            if (row >= 0 && row < 4 && col >= 0 && col < 14)
                fresh[row] |= (1u << col);
        }
    }
    int down = 0;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 14; ++col)
            if (fresh[row] & (1u << col))
                ++down;
    if (down > 6) {
        release_held_matrix_keys();
        std::memset(held, 0, sizeof(held));
        repeat_row = repeat_col = -1;
        return;
    }
    for (int row = 0; row < 4; ++row) {
        const uint16_t rising = static_cast<uint16_t>(fresh[row] & ~held[row]);
        const uint16_t falling = static_cast<uint16_t>(held[row] & ~fresh[row]);
        for (int col = 0; col < 14; ++col) {
            if (!(rising & (1u << col)))
                continue;
            const Press press = (row == 1 && col == 0 && (fresh[3] & (1u << 1)))
                                    ? Press{Key::DebugSettings, 0}
                                    : map_layout(row, col);
            if (press.key == Key::None)
                continue;
            push(press);
            if (repeatable(press.key)) {
                repeat_row = row;
                repeat_col = col;
                repeat_at_ms = now + 420;
            }
        }
        for (int col = 0; col < 14; ++col) {
            if (!(falling & (1u << col)))
                continue;
            Press release = map_layout(row, col);
            if (needs_release(release.key)) {
                release.down = false;
                push(release);
            }
        }
        held[row] = fresh[row];
    }
    if (repeat_row >= 0) {
        const bool still_down = (held[repeat_row] & (1u << repeat_col)) != 0;
        if (!still_down) {
            repeat_row = repeat_col = -1;
        } else if (now >= repeat_at_ms) {
            push(map_layout(repeat_row, repeat_col), true);
            repeat_at_ms = now + 120;
        }
    }
}

bool try_tca_init()
{
    bool devices[128] = {};
    M5.In_I2C.scanID(devices);
    if (!devices[0x34] || !tca.begin() || !tca.matrix(7, 8))
        return false;
    tca.flush();
    tca_option_down = false;
    active = Backend::Tca8418;
    return true;
}

void tca_scan()
{
    while (tca.available()) {
        const uint8_t event = tca.getEvent();
        const bool pressed = (event & 0x80) != 0;
        const int index = (event & 0x7F) - 1;
        if (index < 0)
            continue;
        const int row = (index % 10) % 4;
        const int col = (index / 10) * 2 + ((index % 10) > 3 ? 1 : 0);
        if (row < 0 || row > 3 || col < 0 || col > 13)
            continue;
        if (row == 3 && col == 1) {
            tca_option_down = pressed;
            if (pressed)
                push({Key::Other, 0});
            continue;
        }
        const Press press = (row == 1 && col == 0 && tca_option_down) ? Press{Key::DebugSettings, 0}
                                                                      : map_layout(row, col);
        if (press.key == Key::None)
            continue;
        if (pressed)
            push(press);
        else if (needs_release(press.key)) {
            Press release = press;
            release.down = false;
            push(release);
        }
    }
}

} // namespace

Backend init()
{
    const bool is_adv = static_cast<int>(M5.getBoard()) == 24;
    if (is_adv) {
        if (!try_tca_init()) {
            active = Backend::None;
            tca_retry_at_ms = lgfx::millis() + 1000;
        }
        return active;
    }
    matrix_init();
    active = Backend::Matrix;
    return active;
}

Backend backend()
{
    return active;
}

const char* backend_name()
{
    switch (active) {
    case Backend::Matrix:
        return "gpio_matrix_74hc138";
    case Backend::Tca8418:
        return "tca8418_i2c";
    default:
        return "none";
    }
}

const char* name(Key key)
{
    switch (key) {
    case Key::Up:
        return "UP";
    case Key::Down:
        return "DOWN";
    case Key::Left:
        return "LEFT";
    case Key::Right:
        return "RIGHT";
    case Key::Enter:
        return "ENTER";
    case Key::Back:
        return "BACK";
    case Key::Settings:
        return "SETTINGS";
    case Key::DebugSettings:
        return "DEBUG_SETTINGS";
    case Key::Digit:
        return "DIGIT";
    case Key::Record:
        return "RECORD";
    case Key::Interrupt:
        return "INTERRUPT";
    case Key::Mute:
        return "MUTE";
    case Key::EncoderLeft:
        return "ENCODER_LEFT";
    case Key::EncoderRight:
        return "ENCODER_RIGHT";
    case Key::EncoderPress:
        return "ENCODER_PRESS";
    case Key::NativeAction:
        return "NATIVE_ACTION";
    case Key::Help:
        return "HELP";
    case Key::Other:
        return "OTHER";
    default:
        return "NONE";
    }
}

Press next()
{
    if (active == Backend::Matrix)
        matrix_scan();
    else if (active == Backend::Tca8418)
        tca_scan();
    else if (static_cast<int>(M5.getBoard()) == 24 &&
             static_cast<int32_t>(lgfx::millis() - tca_retry_at_ms) >= 0) {
        if (!try_tca_init())
            tca_retry_at_ms = lgfx::millis() + 1000;
    }
    Press press;
    return queue.pop(press) ? press : Press{};
}

} // namespace keys
