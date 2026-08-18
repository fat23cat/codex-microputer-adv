#include "keys.h"

#include <M5Unified.hpp>
#include <Adafruit_TCA8418.h>

#include <cstring>

#include "driver/gpio.h"
#include "key_layout.h"

namespace keys {
namespace {

Backend active = Backend::None;
Adafruit_TCA8418 tca;
bool tca_option_down = false;

// ------------------------------------------------------- original Cardputer
// Three address lines drive a 74HC138 that pulls one of eight columns low;
// seven inputs read the rows with pull-ups, so a pressed key reads 0.
constexpr gpio_num_t kColumnSelect[3] = {GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11};
constexpr gpio_num_t kRowInput[7] = {
    GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
};

uint16_t held[4]      = {};   // bitmask of columns currently down, per logical row
uint32_t repeat_at_ms = 0;
int      repeat_row   = -1;
int      repeat_col   = -1;
uint32_t last_scan_ms = 0;

// Small ring of decoded presses, so one scan can surface several keys.
Press    queue[12];
int      queue_head = 0;
int      queue_tail = 0;

void push(Press press)
{
    const int next_tail = (queue_tail + 1) % 12;
    if (next_tail == queue_head) return;   // full: drop rather than block
    queue[queue_tail] = press;
    queue_tail = next_tail;
}

// The physical layout is identical on both generations:
//   row 0: ` 1 2 3 4 5 6 7 8 9 0 - = del
//   row 1: tab q w e r t y u i o p [ ] backslash
//   row 2: fn shift a s d f g h j k l ; ' enter
//   row 3: ctrl opt alt z x c v b n m , . / space
//
// Arrows are bound to their unshifted keys rather than behind Fn, because this
// is a controller and the letters underneath are never typed.
bool repeatable(Key key)
{
    return key == Key::Up || key == Key::Down || key == Key::Left || key == Key::Right
        || key == Key::EncoderLeft || key == Key::EncoderRight;
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
    // 15 ms between sweeps debounces the switches and keeps the scan off the
    // critical path of the render loop.
    if (now - last_scan_ms < 15) return;
    last_scan_ms = now;

    uint16_t fresh[4] = {};
    for (int column = 0; column < 8; ++column) {
        for (int bit = 0; bit < 3; ++bit) {
            gpio_set_level(kColumnSelect[bit], (column >> bit) & 1);
        }
        // Let the decoder output and the row pull-ups settle.
        esp_rom_delay_us(20);
        for (int input = 0; input < 7; ++input) {
            if (gpio_get_level(kRowInput[input]) != 0) continue;   // active low
            const int col = (column > 3) ? input + 7 : input;
            const int row = (column > 3) ? column - 4 : column;
            if (row >= 0 && row < 4 && col >= 0 && col < 14) fresh[row] |= (1u << col);
        }
    }

    // A real hand cannot hold this many keys at once. Seeing it means the scan
    // is reading a bus that is not the keyboard, so discard the sweep instead of
    // flooding the app with phantom presses.
    int down = 0;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 14; ++col) if (fresh[row] & (1u << col)) ++down;
    }
    if (down > 6) {
        std::memset(held, 0, sizeof(held));
        repeat_row = repeat_col = -1;
        return;
    }

    for (int row = 0; row < 4; ++row) {
        const uint16_t rising = static_cast<uint16_t>(fresh[row] & ~held[row]);
        const uint16_t falling = static_cast<uint16_t>(held[row] & ~fresh[row]);
        for (int col = 0; col < 14; ++col) {
            if (!(rising & (1u << col))) continue;
            const Press press = (row == 1 && col == 0 && (fresh[3] & (1u << 1)))
                ? Press{Key::DebugSettings, 0} : map_layout(row, col);
            if (press.key == Key::None) continue;
            push(press);
            if (repeatable(press.key)) {
                repeat_row = row; repeat_col = col;
                repeat_at_ms = now + 420;   // initial delay before auto-repeat
            }
        }
        for (int col = 0; col < 14; ++col) {
            if (!(falling & (1u << col))) continue;
            Press release = map_layout(row, col);
            if (needs_release(release.key)) {
                release.down = false;
                push(release);
            }
        }
        held[row] = fresh[row];
    }

    // Auto-repeat only while the originating key is still down.
    if (repeat_row >= 0) {
        const bool still_down = (held[repeat_row] & (1u << repeat_col)) != 0;
        if (!still_down) {
            repeat_row = repeat_col = -1;
        } else if (now >= repeat_at_ms) {
            push(map_layout(repeat_row, repeat_col));
            repeat_at_ms = now + 120;
        }
    }
}

// -------------------------------------------------------------- Cardputer ADV
void tca_scan()
{
    while (tca.available()) {
        const uint8_t event = tca.getEvent();
        const bool pressed = (event & 0x80) != 0;
        const int index = (event & 0x7F) - 1;
        if (index < 0) continue;
        // The controller reports a packed index; the panel is wired transposed.
        const int row = (index % 10) % 4;
        const int col = (index / 10) * 2 + ((index % 10) > 3 ? 1 : 0);
        if (row < 0 || row > 3 || col < 0 || col > 13) continue;
        if (row == 3 && col == 1) {
            tca_option_down = pressed;
            if (pressed) push({Key::Other, 0});
            continue;
        }
        const Press press = (row == 1 && col == 0 && tca_option_down)
            ? Press{Key::DebugSettings, 0} : map_layout(row, col);
        if (press.key == Key::None) continue;
        if (pressed) {
            push(press);
        } else if (needs_release(press.key)) {
            Press release = press;
            release.down = false;
            push(release);
        }
    }
}

}  // namespace

Backend init()
{
    // board_M5CardputerADV is 24 in the M5GFX enum; anything else on this panel
    // is the original Cardputer with the GPIO matrix.
    const bool is_adv = static_cast<int>(M5.getBoard()) == 24;
    if (is_adv) {
        bool devices[128] = {};
        M5.In_I2C.scanID(devices);
        if (devices[0x34] && tca.begin() && tca.matrix(7, 8)) {
            tca.flush();
            active = Backend::Tca8418;
            return active;
        }
    }
    matrix_init();
    active = Backend::Matrix;
    return active;
}

Backend backend() { return active; }

const char* backend_name()
{
    switch (active) {
        case Backend::Matrix:  return "gpio_matrix_74hc138";
        case Backend::Tca8418: return "tca8418_i2c";
        default:               return "none";
    }
}

const char* name(Key key)
{
    switch (key) {
        case Key::Up:        return "UP";
        case Key::Down:      return "DOWN";
        case Key::Left:      return "LEFT";
        case Key::Right:     return "RIGHT";
        case Key::Enter:     return "ENTER";
        case Key::Back:      return "BACK";
        case Key::Settings:  return "SETTINGS";
        case Key::DebugSettings: return "DEBUG_SETTINGS";
        case Key::Digit:     return "DIGIT";
        case Key::Record:    return "RECORD";
        case Key::Interrupt: return "INTERRUPT";
        case Key::Mute:      return "MUTE";
        case Key::EncoderLeft:  return "ENCODER_LEFT";
        case Key::EncoderRight: return "ENCODER_RIGHT";
        case Key::EncoderPress: return "ENCODER_PRESS";
        case Key::NativeAction: return "NATIVE_ACTION";
        case Key::Help:      return "HELP";
        case Key::Other:     return "OTHER";
        default:             return "NONE";
    }
}

Press next()
{
    if (active == Backend::Matrix)  matrix_scan();
    else if (active == Backend::Tca8418) tca_scan();
    else return {};

    if (queue_head == queue_tail) return {};
    const Press press = queue[queue_head];
    queue_head = (queue_head + 1) % 12;
    return press;
}

}  // namespace keys
