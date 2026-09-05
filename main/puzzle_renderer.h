// Pure 8x8 renderer for the optional M5Stack Puzzle Unit task mirror.
// Hardware transport lives in puzzle_unit.cpp; keeping pixels here makes the
// layout, palette, animation, brightness ceiling, and physical mapping host-testable.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "model.h"
#include "motion.h"
#include "status_timing.h"

namespace puzzle_renderer {

constexpr int kWidth = 8;
constexpr int kHeight = 8;
constexpr int kPixelCount = kWidth * kHeight;
constexpr int kSlotCount = 6;
constexpr float kSafetyBrightnessCeiling = 0.10f;

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

inline bool operator==(const Rgb& a, const Rgb& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

inline bool operator!=(const Rgb& a, const Rgb& b) { return !(a == b); }

using Frame = std::array<Rgb, kPixelCount>;

enum class Rotation : uint8_t { Deg0, Deg90, Deg180, Deg270 };

struct Slot {
    bool present = false;
    model::Status status = model::Status::Idle;
    bool unseen_done = false;
};

struct Takeover {
    bool active = false;
    int slot = -1;
    model::Status status = model::Status::Idle;
    bool unseen = false;
    float viewed_progress = 0.f;
    float age = 0.f;
};

struct Input {
    Slot slots[kSlotCount] = {};
    int selected = -1;
    bool linked = false;
    float phase = 0.f;
    // Physical output scale, not a UI percentage. render() clamps it to the
    // manufacturer's 10% continuous-use recommendation.
    float brightness = 0.f;
    Takeover takeover;
};

struct Bounds {
    int x;
    int y;
    int w;
    int h;
};

constexpr Bounds slot_bounds(int slot)
{
    // Two 2x3 rows, with one dark column between neighbours and two dark
    // rows through the middle. Every slot therefore has identical area.
    return Bounds{(slot % 3) * 3, (slot / 3) * 5, 2, 3};
}

constexpr std::size_t logical_index(int x, int y)
{
    return static_cast<std::size_t>(y * kWidth + x);
}

constexpr std::size_t wire_index(int x, int y, Rotation rotation)
{
    int panel_x = x;
    int panel_y = y;
    switch (rotation) {
    case Rotation::Deg90:
        panel_x = kWidth - 1 - y;
        panel_y = x;
        break;
    case Rotation::Deg180:
        panel_x = kWidth - 1 - x;
        panel_y = kHeight - 1 - y;
        break;
    case Rotation::Deg270:
        panel_x = y;
        panel_y = kHeight - 1 - x;
        break;
    case Rotation::Deg0:
        break;
    }
    // Unit Puzzle is wired column-major, bottom-to-top in the documented
    // upright orientation.
    return static_cast<std::size_t>((kHeight - 1 - panel_y) + panel_x * kHeight);
}

inline Rgb mix(Rgb a, Rgb b, float amount)
{
    const float t = motion::clamp01(amount);
    return Rgb{
        static_cast<uint8_t>(a.r + (b.r - a.r) * t + 0.5f),
        static_cast<uint8_t>(a.g + (b.g - a.g) * t + 0.5f),
        static_cast<uint8_t>(a.b + (b.b - a.b) * t + 0.5f),
    };
}

inline Rgb status_colour(model::Status status, bool unseen)
{
    switch (status) {
    case model::Status::Running:    return {27, 79, 208};
    case model::Status::NeedsInput: return {226, 69, 30};
    case model::Status::Done:       return unseen ? Rgb{38, 198, 58}
                                                 : Rgb{228, 229, 225};
    case model::Status::Error:      return {0, 0, 0};
    case model::Status::Idle:       return {222, 219, 209};
    }
    return {};
}

inline Rgb status_foreground(model::Status status)
{
    switch (status) {
    case model::Status::Running:
    case model::Status::NeedsInput:
        return {244, 242, 236};
    case model::Status::Error:
        return {226, 69, 30};
    case model::Status::Done:
    case model::Status::Idle:
        return {0, 0, 0};
    }
    return {};
}

inline Rgb scaled(Rgb colour, float amount)
{
    const float level = std::clamp(amount, 0.f, kSafetyBrightnessCeiling);
    return Rgb{
        static_cast<uint8_t>(colour.r * level + 0.5f),
        static_cast<uint8_t>(colour.g * level + 0.5f),
        static_cast<uint8_t>(colour.b * level + 0.5f),
    };
}

inline float expansion_coverage(int x, int y, Bounds source, float spread)
{
    int dx = 0;
    if (x < source.x) dx = source.x - x;
    else if (x >= source.x + source.w) dx = x - (source.x + source.w - 1);
    int dy = 0;
    if (y < source.y) dy = source.y - y;
    else if (y >= source.y + source.h) dy = y - (source.y + source.h - 1);
    const int distance = std::max(dx, dy);
    const int far_x = std::max(source.x, kWidth - (source.x + source.w));
    const int far_y = std::max(source.y, kHeight - (source.y + source.h));
    const int farthest = std::max(1, std::max(far_x, far_y));
    // A one-step soft boundary keeps the coarse 8x8 expansion from popping.
    return motion::clamp01(spread * static_cast<float>(farthest + 1)
                           - static_cast<float>(distance) + 1.f);
}

inline Frame render(const Input& input)
{
    Frame frame{};
    if (!input.linked || input.brightness <= 0.f) return frame;

    for (int slot = 0; slot < kSlotCount; ++slot) {
        const Slot& task = input.slots[slot];
        if (!task.present) continue;
        const Bounds bounds = slot_bounds(slot);
        const float selected = slot == input.selected
            ? 0.925f + 0.075f * std::sin(input.phase * 2.f)
            : 0.82f;
        for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
            for (int x = bounds.x; x < bounds.x + bounds.w; ++x) {
                float activity = selected;
                if (task.status == model::Status::Idle) activity *= 0.42f;
                if (task.status == model::Status::Running) {
                    const float wave = std::sin(input.phase * (1.62f + 0.113f * slot)
                                                + x * 0.73f + y * 1.19f);
                    activity *= 0.91f + 0.09f * wave;
                }
                Rgb colour = status_colour(task.status, task.unseen_done);
                if (task.status == model::Status::Error) {
                    // Black is the error surface on the LCD. One red crossbar
                    // keeps the tiny external slot addressable in the dark.
                    colour = y == bounds.y + 1 ? Rgb{226, 69, 30} : Rgb{};
                }
                frame[logical_index(x, y)] = mix(Rgb{}, colour, activity);
            }
        }
    }

    const Takeover& takeover = input.takeover;
    if (takeover.active && takeover.slot >= 0 && takeover.slot < kSlotCount
        && takeover.age >= status_timing::rail_out
        && takeover.age < status_timing::life) {
        const float age = takeover.age - status_timing::rail_out;
        if (age < status_timing::visual_life) {
            const float expand_start = status_timing::colour;
            const float centre_start = expand_start + status_timing::expand;
            const float hold_start = centre_start + status_timing::centre;
            const float return_start = hold_start + status_timing::hold;
            const float collapse_start = return_start + status_timing::returning;

            float spread = 0.f;
            if (age >= expand_start && age < centre_start) {
                spread = motion::ease_in_out_cubic(
                    (age - expand_start) / status_timing::expand);
            } else if (age >= centre_start && age < collapse_start) {
                spread = 1.f;
            } else if (age >= collapse_start) {
                spread = 1.f - motion::ease_in_out_cubic(
                    (age - collapse_start) / status_timing::collapse);
            }

            float numeral = 0.f;
            if (age >= centre_start && age < hold_start) {
                numeral = motion::ease_in_out_cubic(
                    (age - centre_start) / status_timing::centre);
            } else if (age >= hold_start && age < return_start) {
                numeral = 1.f;
            } else if (age >= return_start && age < collapse_start) {
                numeral = 1.f - motion::ease_in_out_cubic(
                    (age - return_start) / status_timing::returning);
            }

            Rgb surface = status_colour(takeover.status, takeover.unseen);
            if (takeover.status == model::Status::Done && takeover.viewed_progress > 0.f)
                surface = mix(surface, status_colour(model::Status::Done, false),
                              takeover.viewed_progress);
            const Bounds source = slot_bounds(takeover.slot);
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const float coverage = expansion_coverage(x, y, source, spread);
                    frame[logical_index(x, y)] = mix(frame[logical_index(x, y)],
                                                     surface, coverage);
                }
            }

            static constexpr uint8_t digits[kSlotCount][5] = {
                {0b010, 0b110, 0b010, 0b010, 0b111},
                {0b110, 0b001, 0b010, 0b100, 0b111},
                {0b110, 0b001, 0b010, 0b001, 0b110},
                {0b101, 0b101, 0b111, 0b001, 0b001},
                {0b111, 0b100, 0b110, 0b001, 0b110},
                {0b011, 0b100, 0b111, 0b101, 0b111},
            };
            const Rgb foreground = status_foreground(takeover.status);
            for (int row = 0; row < 5; ++row) {
                for (int col = 0; col < 3; ++col) {
                    if ((digits[takeover.slot][row] & (1u << (2 - col))) == 0) continue;
                    const std::size_t pixel = logical_index(2 + col, 1 + row);
                    frame[pixel] = mix(frame[pixel], foreground, numeral);
                }
            }
        }
    }

    for (Rgb& pixel : frame) pixel = scaled(pixel, input.brightness);
    return frame;
}

}  // namespace puzzle_renderer
