#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "puzzle_renderer.h"

model::State model::state;

namespace {
int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL puzzle_renderer: " << name << '\n';
}

bool dark(puzzle_renderer::Rgb pixel)
{
    return pixel.r == 0 && pixel.g == 0 && pixel.b == 0;
}

bool frames_near(const puzzle_renderer::Frame& a, const puzzle_renderer::Frame& b)
{
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(a[i].r) - b[i].r) > 1
            || std::abs(static_cast<int>(a[i].g) - b[i].g) > 1
            || std::abs(static_cast<int>(a[i].b) - b[i].b) > 1) return false;
    }
    return true;
}

puzzle_renderer::Input deck_input()
{
    puzzle_renderer::Input input;
    input.linked = true;
    input.brightness = 0.10f;
    input.selected = 0;
    for (int i = 0; i < puzzle_renderer::kSlotCount; ++i) {
        input.slots[i].present = true;
        input.slots[i].status = model::Status::Running;
    }
    return input;
}

void physical_mapping_is_a_permutation()
{
    static constexpr puzzle_renderer::Rotation rotations[] = {
        puzzle_renderer::Rotation::Deg0, puzzle_renderer::Rotation::Deg90,
        puzzle_renderer::Rotation::Deg180, puzzle_renderer::Rotation::Deg270,
    };
    for (auto rotation : rotations) {
        std::array<bool, puzzle_renderer::kPixelCount> seen{};
        for (int y = 0; y < puzzle_renderer::kHeight; ++y) {
            for (int x = 0; x < puzzle_renderer::kWidth; ++x) {
                const std::size_t index = puzzle_renderer::wire_index(x, y, rotation);
                check(index < seen.size(), "wire index stays in range");
                if (index < seen.size()) {
                    check(!seen[index], "wire index is unique");
                    seen[index] = true;
                }
            }
        }
        for (bool value : seen) check(value, "wire mapping covers every LED");
    }
    check(puzzle_renderer::wire_index(0, 0, puzzle_renderer::Rotation::Deg0) == 7,
          "upright top-left uses documented column mapping");
    check(puzzle_renderer::wire_index(7, 7, puzzle_renderer::Rotation::Deg0) == 56,
          "upright bottom-right uses documented column mapping");
    check(puzzle_renderer::wire_index(0, 0, puzzle_renderer::Rotation::Deg90) == 63,
          "90-degree rotation moves the logical origin clockwise");
    check(puzzle_renderer::wire_index(0, 0, puzzle_renderer::Rotation::Deg180) == 56,
          "180-degree rotation moves the logical origin diagonally");
    check(puzzle_renderer::wire_index(0, 0, puzzle_renderer::Rotation::Deg270) == 0,
          "270-degree rotation moves the logical origin counter-clockwise");
}

void grid_has_six_equal_tiles_and_dark_gutters()
{
    const auto frame = puzzle_renderer::render(deck_input());
    int lit = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool gutter = x == 2 || x == 5 || y == 3 || y == 4;
            const bool is_lit = !dark(frame[puzzle_renderer::logical_index(x, y)]);
            check(gutter ? !is_lit : is_lit, "3x2 grid mask is exact");
            if (is_lit) ++lit;
        }
    }
    check(lit == 36, "six 2x3 tiles light exactly 36 pixels");
}

void statuses_and_selection_preserve_semantics()
{
    check(puzzle_renderer::status_colour(model::Status::Idle, false)
              == puzzle_renderer::Rgb{222, 219, 209}, "idle palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Running, false)
              == puzzle_renderer::Rgb{27, 79, 208}, "running palette is exact");
    check(puzzle_renderer::status_colour(model::Status::NeedsInput, false)
              == puzzle_renderer::Rgb{226, 69, 30}, "input palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Done, true)
              == puzzle_renderer::Rgb{38, 198, 58}, "unseen palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Done, false)
              == puzzle_renderer::Rgb{228, 229, 225}, "viewed palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Error, false)
              == puzzle_renderer::Rgb{}, "error surface palette is black");
    check(puzzle_renderer::status_foreground(model::Status::Error)
              == puzzle_renderer::Rgb{226, 69, 30}, "error mark palette is exact");

    auto input = deck_input();
    input.phase = 0.7853982f;
    input.slots[0].status = model::Status::Idle;
    input.slots[1].status = model::Status::NeedsInput;
    input.slots[2].status = model::Status::Done;
    input.slots[2].unseen_done = true;
    input.slots[3].status = model::Status::Done;
    input.slots[4].status = model::Status::Error;
    input.slots[5].present = false;
    const auto frame = puzzle_renderer::render(input);
    const auto idle = frame[puzzle_renderer::logical_index(0, 0)];
    const auto attention = frame[puzzle_renderer::logical_index(3, 0)];
    const auto unseen = frame[puzzle_renderer::logical_index(6, 0)];
    const auto viewed = frame[puzzle_renderer::logical_index(0, 5)];
    const auto error_edge = frame[puzzle_renderer::logical_index(3, 5)];
    const auto error_mark = frame[puzzle_renderer::logical_index(3, 6)];
    check(idle.r >= idle.b && idle.r > 0, "idle is a dim warm neutral");
    check(idle.r < unseen.g, "idle is visibly dimmer than active status colour");
    check(attention.r > attention.g * 2, "attention is orange-red");
    check(unseen.g > unseen.r * 3, "unseen completion is green");
    check(std::abs(static_cast<int>(viewed.r) - viewed.g) <= 1,
          "viewed completion is neutral");
    check(dark(error_edge) && error_mark.r > 0, "error is a red mark on black");
    check(dark(frame[puzzle_renderer::logical_index(6, 5)]), "unbound slot is off");
}

void brightness_is_bounded_and_zero_is_black()
{
    auto input = deck_input();
    input.brightness = 1.f;
    const auto capped = puzzle_renderer::render(input);
    for (auto pixel : capped) {
        check(pixel.r <= 26 && pixel.g <= 26 && pixel.b <= 26,
              "every channel respects the ten-percent ceiling");
    }
    input.brightness = 0.f;
    const auto off = puzzle_renderer::render(input);
    for (auto pixel : off) check(dark(pixel), "zero brightness clears every pixel");
    input.brightness = 0.1f;
    input.linked = false;
    const auto offline = puzzle_renderer::render(input);
    for (auto pixel : offline) check(dark(pixel), "offline clears every pixel");
}

void selected_tile_breathes_without_changing_hue()
{
    auto input = deck_input();
    input.slots[0].status = model::Status::NeedsInput;
    input.phase = -0.7853982f;
    const auto low = puzzle_renderer::render(input)[0];
    input.phase = 0.7853982f;
    const auto high = puzzle_renderer::render(input)[0];
    check(high.r > low.r && high.g >= low.g && high.b >= low.b,
          "selected tile changes luminance across its breath");
    check(std::abs(high.r * low.g - low.r * high.g) <= 30,
          "selection breath retains the status hue");
}

void takeover_expands_holds_a_number_and_returns()
{
    auto input = deck_input();
    input.takeover.active = true;
    input.takeover.slot = 5;
    input.takeover.status = model::Status::NeedsInput;
    input.takeover.age = status_timing::rail_out;
    const auto start = puzzle_renderer::render(input);
    input.takeover.age = status_timing::rail_out + status_timing::hold_start + 0.2f;
    const auto hold = puzzle_renderer::render(input);
    int start_lit = 0;
    int hold_lit = 0;
    int dark_digit_pixels = 0;
    for (int i = 0; i < puzzle_renderer::kPixelCount; ++i) {
        start_lit += !dark(start[i]);
        hold_lit += !dark(hold[i]);
    }
    // Attention uses a light numeral, so verify a numeral pixel differs from
    // its orange neighbour rather than looking for a dark cutout.
    const auto digit = hold[puzzle_renderer::logical_index(4, 1)];
    const auto background = hold[puzzle_renderer::logical_index(7, 0)];
    dark_digit_pixels += digit != background;
    check(start_lit == 36, "takeover waits through the rail-out bookend");
    check(hold_lit == 64, "takeover hold covers all 64 pixels");
    check(dark_digit_pixels == 1, "takeover hold draws the slot numeral");

    input.takeover.age = status_timing::rail_out + status_timing::colour
                       + status_timing::expand * 0.5f;
    const auto expanding = puzzle_renderer::render(input);
    input.takeover.age = status_timing::rail_out + status_timing::return_start
                       + status_timing::returning + status_timing::collapse * 0.5f;
    const auto collapsing = puzzle_renderer::render(input);
    check(frames_near(expanding, collapsing),
          "takeover collapse is the spatial reverse of its expansion");

    input.takeover.age = status_timing::life;
    const auto returned = puzzle_renderer::render(input);
    check(returned == puzzle_renderer::render(deck_input()),
          "takeover returns to the exact stable deck endpoint");
}

void takeover_draws_exact_digits_one_through_six()
{
    static constexpr uint8_t digits[6][5] = {
        {0b010, 0b110, 0b010, 0b010, 0b111},
        {0b110, 0b001, 0b010, 0b100, 0b111},
        {0b110, 0b001, 0b010, 0b001, 0b110},
        {0b101, 0b101, 0b111, 0b001, 0b001},
        {0b111, 0b100, 0b110, 0b001, 0b110},
        {0b011, 0b100, 0b111, 0b101, 0b111},
    };
    for (int slot = 0; slot < 6; ++slot) {
        auto input = deck_input();
        input.takeover.active = true;
        input.takeover.slot = slot;
        input.takeover.status = model::Status::NeedsInput;
        input.takeover.age = status_timing::rail_out + status_timing::hold_start + 0.1f;
        const auto frame = puzzle_renderer::render(input);
        const auto surface = frame[puzzle_renderer::logical_index(7, 7)];
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                const bool expected = (digits[slot][row] & (1u << (2 - col))) != 0;
                const auto pixel = frame[puzzle_renderer::logical_index(2 + col, 1 + row)];
                check((pixel != surface) == expected,
                      "each takeover numeral has its exact 3x5 mask");
            }
        }
    }
}

void viewed_completion_fades_toward_neutral()
{
    auto input = deck_input();
    input.takeover.active = true;
    input.takeover.slot = 2;
    input.takeover.status = model::Status::Done;
    input.takeover.unseen = true;
    input.takeover.age = status_timing::rail_out + status_timing::hold_start + 0.2f;
    input.takeover.viewed_progress = 0.f;
    const auto green = puzzle_renderer::render(input)[puzzle_renderer::logical_index(7, 7)];
    input.takeover.viewed_progress = 1.f;
    const auto grey = puzzle_renderer::render(input)[puzzle_renderer::logical_index(7, 7)];
    check(green.g > green.r * 3, "fresh completion takeover begins green");
    check(std::abs(static_cast<int>(grey.r) - grey.g) <= 1,
          "viewed completion takeover reaches neutral");
}
}  // namespace

int main()
{
    physical_mapping_is_a_permutation();
    grid_has_six_equal_tiles_and_dark_gutters();
    statuses_and_selection_preserve_semantics();
    brightness_is_bounded_and_zero_is_black();
    selected_tile_breathes_without_changing_hue();
    takeover_expands_holds_a_number_and_returns();
    takeover_draws_exact_digits_one_through_six();
    viewed_completion_fades_toward_neutral();
    if (failures) return EXIT_FAILURE;
    std::cout << "PASS puzzle_renderer (8 scenarios)\n";
    return EXIT_SUCCESS;
}
