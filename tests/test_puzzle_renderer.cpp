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

void grid_has_six_square_islands_on_a_lit_status_field()
{
    static constexpr puzzle_renderer::Bounds expected[6] = {
        {0, 1, 2, 2}, {3, 1, 2, 2}, {6, 1, 2, 2},
        {0, 5, 2, 2}, {3, 5, 2, 2}, {6, 5, 2, 2},
    };
    for (int slot = 0; slot < 6; ++slot) {
        const auto actual = puzzle_renderer::slot_bounds(slot);
        check(actual.x == expected[slot].x && actual.y == expected[slot].y
                  && actual.w == expected[slot].w && actual.h == expected[slot].h,
              "six equal slot bounds are exact");
    }

    int slot_pixels = 0;
    int field_pixels = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            bool in_slot = false;
            for (const auto& bounds : expected) {
                in_slot |= x >= bounds.x && x < bounds.x + bounds.w
                        && y >= bounds.y && y < bounds.y + bounds.h;
            }
            const bool expected_slot = (y == 1 || y == 2 || y == 5 || y == 6)
                                    && (x != 2 && x != 5);
            check(in_slot == expected_slot, "square-island mask is exact");
            slot_pixels += in_slot;
            field_pixels += !in_slot;
        }
    }
    check(slot_pixels == 24, "six 2x2 islands use 24 pixels");
    check(field_pixels == 40, "selected-status field uses the other 40 pixels");

    const auto frame = puzzle_renderer::render(deck_input());
    int lit = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool is_lit = !dark(frame[puzzle_renderer::logical_index(x, y)]);
            check(is_lit, "islands and status field have no dark holes");
            if (is_lit) ++lit;
        }
    }
    check(lit == 64, "status field and six islands cover all 64 pixels");
    const auto field = frame[puzzle_renderer::logical_index(2, 1)];
    const auto slot = frame[puzzle_renderer::logical_index(0, 1)];
    check(field.b > field.r * 3 && field.b >= field.g * 2,
          "background field carries the selected running colour");
    check(slot.b > field.b, "status islands remain distinct from the quiet field");
}

void statuses_and_selection_preserve_semantics()
{
    check(puzzle_renderer::status_colour(model::Status::Idle, false)
              == puzzle_renderer::Rgb{190, 146, 72}, "idle palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Running, false)
              == puzzle_renderer::Rgb{30, 108, 255}, "running palette is exact");
    check(puzzle_renderer::status_colour(model::Status::NeedsInput, false)
              == puzzle_renderer::Rgb{255, 122, 18}, "input palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Done, true)
              == puzzle_renderer::Rgb{32, 208, 90}, "unseen palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Done, false)
              == puzzle_renderer::Rgb{168, 183, 199}, "viewed palette is exact");
    check(puzzle_renderer::status_colour(model::Status::Error, false)
              == puzzle_renderer::Rgb{}, "error surface palette is black");
    check(puzzle_renderer::status_foreground(model::Status::Error)
              == puzzle_renderer::Rgb{255, 50, 50}, "error mark palette is exact");

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
    const auto idle = frame[puzzle_renderer::logical_index(0, 1)];
    const auto attention = frame[puzzle_renderer::logical_index(3, 1)];
    const auto unseen = frame[puzzle_renderer::logical_index(6, 1)];
    const auto viewed = frame[puzzle_renderer::logical_index(0, 5)];
    const auto error_mark = frame[puzzle_renderer::logical_index(3, 5)];
    const auto error_dark = frame[puzzle_renderer::logical_index(4, 5)];
    check(idle.r >= idle.b && idle.r > 0, "idle is a dim warm neutral");
    check(idle.r < unseen.g, "idle is visibly dimmer than active status colour");
    check(attention.r > attention.g * 2, "attention is orange-red");
    check(unseen.g > unseen.r * 3, "unseen completion is green");
    check(viewed.b > viewed.g && viewed.g >= viewed.r,
          "viewed completion is a cool neutral");
    check(error_mark.r > 0 && dark(error_dark), "error is a red mark on black");
    check(dark(frame[puzzle_renderer::logical_index(6, 5)]), "unbound slot is off");
    check(!dark(frame[puzzle_renderer::logical_index(5, 5)]),
          "field beside an unbound slot keeps the selected status colour");
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
    input.slots[0].status = model::Status::Done;
    input.slots[0].unseen_done = false;
    input.phase = -0.7853982f;
    const auto low_frame = puzzle_renderer::render(input);
    const auto low = low_frame[puzzle_renderer::logical_index(0, 1)];
    const auto low_field = low_frame[puzzle_renderer::logical_index(2, 1)];
    input.phase = 0.7853982f;
    const auto high_frame = puzzle_renderer::render(input);
    const auto high = high_frame[puzzle_renderer::logical_index(0, 1)];
    const auto high_field = high_frame[puzzle_renderer::logical_index(2, 1)];
    check(high.r > low.r && high.g >= low.g && high.b >= low.b,
          "selected tile changes luminance across its breath");
    check(static_cast<int>(high.b) - low.b >= 7,
          "selection breath remains visible after the ten-percent output cap");
    check(high_field == low_field,
          "selected status field stays constant while the tile breathes");
    check(std::abs(high.r * low.g - low.r * high.g) <= 20,
          "selection breath retains the status hue");
}

void statuses_have_distinct_two_by_two_microanimations()
{
    puzzle_renderer::Input input;
    input.linked = true;
    input.brightness = 0.10f;
    input.slots[0].present = true;

    input.slots[0].status = model::Status::Running;
    input.phase = 0.f;
    const auto running_a = puzzle_renderer::render(input);
    input.phase = 0.3926991f;
    const auto running_b = puzzle_renderer::render(input);
    check(running_a[puzzle_renderer::logical_index(0, 1)].b
              > running_a[puzzle_renderer::logical_index(1, 1)].b,
          "running begins with one bright orbit corner");
    check(running_b[puzzle_renderer::logical_index(1, 1)].b
              > running_b[puzzle_renderer::logical_index(0, 1)].b,
          "running orbit advances to the next corner");

    input.slots[0].status = model::Status::NeedsInput;
    input.phase = -0.3272492f;
    const auto attention_low = puzzle_renderer::render(input);
    input.phase = 0.3272492f;
    const auto attention_high = puzzle_renderer::render(input);
    check(attention_high[puzzle_renderer::logical_index(0, 1)].r
              > attention_low[puzzle_renderer::logical_index(0, 1)].r + 8,
          "input-needed pulses the complete square");

    input.slots[0].status = model::Status::Done;
    input.slots[0].unseen_done = true;
    input.phase = 0.f;
    const auto done_quiet = puzzle_renderer::render(input);
    input.phase = 0.21f;
    const auto done_sparkle = puzzle_renderer::render(input);
    check(done_sparkle[puzzle_renderer::logical_index(0, 1)].g
              > done_quiet[puzzle_renderer::logical_index(0, 1)].g,
          "unread completion sparkles on a short diagonal");

    input.slots[0].unseen_done = false;
    input.phase = 0.f;
    const auto viewed_a = puzzle_renderer::render(input);
    input.phase = 1.f;
    const auto viewed_b = puzzle_renderer::render(input);
    check(viewed_a[puzzle_renderer::logical_index(0, 1)]
              == viewed_b[puzzle_renderer::logical_index(0, 1)],
          "viewed completion stays still");

    input.slots[0].status = model::Status::Error;
    input.phase = 0.f;
    const auto error_a = puzzle_renderer::render(input);
    input.phase = 0.7f;
    const auto error_b = puzzle_renderer::render(input);
    check(error_a[puzzle_renderer::logical_index(0, 1)].r > 0
              && dark(error_a[puzzle_renderer::logical_index(1, 1)]),
          "error begins on the first diagonal");
    check(dark(error_b[puzzle_renderer::logical_index(0, 1)])
              && error_b[puzzle_renderer::logical_index(1, 1)].r > 0,
          "error alternates to the other diagonal");
}

void selection_travels_only_through_the_background_field()
{
    auto input = deck_input();
    input.selected = 2;
    input.phase = 0.33f;
    const auto stable = puzzle_renderer::render(input);
    input.selection_travel.active = true;
    input.selection_travel.from = 0;
    input.selection_travel.to = 2;
    input.selection_travel.progress = 0.5f;
    const auto travelling = puzzle_renderer::render(input);

    int changed = 0;
    for (int y = 0; y < puzzle_renderer::kHeight; ++y) {
        for (int x = 0; x < puzzle_renderer::kWidth; ++x) {
            const auto pixel = puzzle_renderer::logical_index(x, y);
            if (stable[pixel] == travelling[pixel]) continue;
            ++changed;
            check(puzzle_renderer::slot_at(x, y) < 0,
                  "selection travel never overwrites a task square");
        }
    }
    check(changed > 0, "selection travel lights a route through the field");
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
    check(start_lit == 64, "takeover waits through the rail-out bookend");
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
    static constexpr uint8_t digits[6][6] = {
        {0b0110, 0b1110, 0b0110, 0b0110, 0b0110, 0b1111},
        {0b0110, 0b1001, 0b0001, 0b0010, 0b0100, 0b1111},
        {0b1110, 0b0001, 0b0110, 0b0001, 0b0001, 0b1110},
        {0b1001, 0b1001, 0b1111, 0b0001, 0b0001, 0b0001},
        {0b1111, 0b1000, 0b1110, 0b0001, 0b0001, 0b1110},
        {0b0111, 0b1000, 0b1110, 0b1001, 0b1001, 0b0110},
    };
    for (int slot = 0; slot < 6; ++slot) {
        auto input = deck_input();
        input.takeover.active = true;
        input.takeover.slot = slot;
        input.takeover.status = model::Status::NeedsInput;
        input.takeover.age = status_timing::rail_out + status_timing::hold_start + 0.1f;
        const auto frame = puzzle_renderer::render(input);
        const auto surface = frame[puzzle_renderer::logical_index(7, 7)];
        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 4; ++col) {
                const bool expected = (digits[slot][row] & (1u << (3 - col))) != 0;
                const auto pixel = frame[puzzle_renderer::logical_index(2 + col, 1 + row)];
                check((pixel != surface) == expected,
                      "each takeover numeral has its exact centred 4x6 mask");
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
    grid_has_six_square_islands_on_a_lit_status_field();
    statuses_and_selection_preserve_semantics();
    brightness_is_bounded_and_zero_is_black();
    selected_tile_breathes_without_changing_hue();
    statuses_have_distinct_two_by_two_microanimations();
    selection_travels_only_through_the_background_field();
    takeover_expands_holds_a_number_and_returns();
    takeover_draws_exact_digits_one_through_six();
    viewed_completion_fades_toward_neutral();
    if (failures) return EXIT_FAILURE;
    std::cout << "PASS puzzle_renderer (10 scenarios)\n";
    return EXIT_SUCCESS;
}
