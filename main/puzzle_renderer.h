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

struct SelectionTravel {
    bool active = false;
    int from = -1;
    int to = -1;
    float progress = 1.f;
};

struct Input {
    Slot slots[kSlotCount] = {};
    int selected = -1;
    bool linked = false;
    float phase = 0.f;
    // Physical output scale, not a UI percentage. render() clamps it to the
    // manufacturer's 10% continuous-use recommendation.
    float brightness = 0.f;
    SelectionTravel selection_travel;
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
    // Six true 2x2 squares sit as separated islands. Columns 2 and 5, the
    // centre rows, and the top/bottom edges belong to the selected-status
    // field, so the complete 8x8 panel stays visually cohesive.
    return Bounds{(slot % 3) * 3, 1 + (slot / 3) * 4, 2, 2};
}

constexpr bool contains(Bounds bounds, int x, int y)
{
    return x >= bounds.x && x < bounds.x + bounds.w
        && y >= bounds.y && y < bounds.y + bounds.h;
}

constexpr int slot_at(int x, int y)
{
    for (int slot = 0; slot < kSlotCount; ++slot) {
        if (contains(slot_bounds(slot), x, y)) return slot;
    }
    return -1;
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
    // These are deliberately separated after the final ten-percent scale:
    // electric blue, amber-orange, fresh green, cool viewed grey, and a warm
    // low idle all remain recognisable on real WS2812E emitters.
    case model::Status::Running:    return {30, 108, 255};
    case model::Status::NeedsInput: return {255, 122, 18};
    case model::Status::Done:       return unseen ? Rgb{32, 208, 90}
                                                 : Rgb{168, 183, 199};
    case model::Status::Error:      return {0, 0, 0};
    case model::Status::Idle:       return {190, 146, 72};
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
        return {255, 50, 50};
    case model::Status::Done:
    case model::Status::Idle:
        return {0, 0, 0};
    }
    return {};
}

inline Rgb slot_accent(const Slot& slot)
{
    return slot.status == model::Status::Error
        ? status_foreground(model::Status::Error)
        : status_colour(slot.status, slot.unseen_done);
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

using DistanceMap = std::array<int8_t, kPixelCount>;

inline DistanceMap field_distances_from(int slot)
{
    DistanceMap distances;
    distances.fill(-1);
    if (slot < 0 || slot >= kSlotCount) return distances;

    std::array<uint8_t, kPixelCount> queue{};
    int head = 0;
    int tail = 0;
    const Bounds source = slot_bounds(slot);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (slot_at(x, y) >= 0) continue;
            const bool adjacent = ((x == source.x - 1 || x == source.x + source.w)
                                   && y >= source.y && y < source.y + source.h)
                               || ((y == source.y - 1 || y == source.y + source.h)
                                   && x >= source.x && x < source.x + source.w);
            if (!adjacent) continue;
            const std::size_t pixel = logical_index(x, y);
            distances[pixel] = 0;
            queue[tail++] = static_cast<uint8_t>(pixel);
        }
    }

    static constexpr int dx[] = {1, 0, -1, 0};
    static constexpr int dy[] = {0, 1, 0, -1};
    while (head < tail) {
        const int pixel = queue[head++];
        const int x = pixel % kWidth;
        const int y = pixel / kWidth;
        for (int direction = 0; direction < 4; ++direction) {
            const int nx = x + dx[direction];
            const int ny = y + dy[direction];
            if (nx < 0 || nx >= kWidth || ny < 0 || ny >= kHeight
                || slot_at(nx, ny) >= 0) continue;
            const std::size_t neighbour = logical_index(nx, ny);
            if (distances[neighbour] >= 0) continue;
            distances[neighbour] = static_cast<int8_t>(distances[pixel] + 1);
            queue[tail++] = static_cast<uint8_t>(neighbour);
        }
    }
    return distances;
}

inline Frame render(const Input& input)
{
    Frame frame{};
    if (!input.linked || input.brightness <= 0.f) return frame;

    const SelectionTravel& travel = input.selection_travel;
    Rgb field_colour = status_colour(model::Status::Idle, false);
    float field_activity = 0.14f;
    if (input.selected >= 0 && input.selected < kSlotCount
        && input.slots[input.selected].present) {
        field_colour = slot_accent(input.slots[input.selected]);
        field_activity = 0.24f;
    }
    if (travel.active && travel.from >= 0 && travel.from < kSlotCount
        && travel.to >= 0 && travel.to < kSlotCount
        && input.slots[travel.from].present && input.slots[travel.to].present) {
        field_colour = mix(slot_accent(input.slots[travel.from]),
                           slot_accent(input.slots[travel.to]),
                           motion::ease_in_out_cubic(travel.progress));
    }
    // Every pixel outside the six square islands carries the selected status
    // colour. It is intentionally static, so the unavoidable 8x8 remainder
    // supports focus instead of competing with the task microanimations.
    const Rgb field = mix(Rgb{}, field_colour, field_activity);
    frame.fill(field);

    for (int slot = 0; slot < kSlotCount; ++slot) {
        const Slot& task = input.slots[slot];
        const Bounds bounds = slot_bounds(slot);
        if (!task.present) {
            // Unbound slots remain explicit dark squares rather than being
            // mistaken for part of the selected-status background.
            for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
                for (int x = bounds.x; x < bounds.x + bounds.w; ++x)
                    frame[logical_index(x, y)] = {};
            }
            continue;
        }
        // Selection is a stable brightness step. Status animations remain
        // independent, but selecting a task never makes the whole tile pulse.
        const float selected = slot == input.selected ? 1.f : 0.55f;
        for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
            for (int x = bounds.x; x < bounds.x + bounds.w; ++x) {
                float activity = selected;
                Rgb colour = status_colour(task.status, task.unseen_done);
                const int local_x = x - bounds.x;
                const int local_y = y - bounds.y;
                if (task.status == model::Status::Idle) {
                    activity *= 0.62f;
                } else if (task.status == model::Status::Running) {
                    // A bright corner makes one smooth lap around the 2x2.
                    const int corner = local_y == 0 ? local_x : 3 - local_x;
                    const float orbit = 0.5f + 0.5f * std::cos(
                        input.phase * 4.f - corner * 1.5707963f);
                    activity *= 0.52f + 0.48f * orbit * orbit;
                } else if (task.status == model::Status::NeedsInput) {
                    const float attention = 0.5f + 0.5f * std::sin(input.phase * 4.8f);
                    activity *= 0.64f + 0.36f * attention;
                } else if (task.status == model::Status::Done && task.unseen_done) {
                    float cycle = std::fmod(input.phase + slot * 0.17f, 2.4f);
                    if (cycle < 0.f) cycle += 2.4f;
                    const float sparkle = cycle < 0.42f
                        ? std::sin(cycle * 3.1415927f / 0.42f) : 0.f;
                    const bool sparkle_diagonal = local_x == local_y;
                    activity *= 0.72f + sparkle * (sparkle_diagonal ? 0.28f : 0.08f);
                } else if (task.status == model::Status::Done) {
                    activity *= 0.82f;
                } else if (task.status == model::Status::Error) {
                    // Alternate the two diagonals. The other pair stays black,
                    // retaining the LCD's red-mark-on-dark error semantics.
                    const bool first_diagonal = std::sin(input.phase * 5.f + slot) >= 0.f;
                    const bool lit = first_diagonal ? local_x == local_y
                                                    : local_x + local_y == 1;
                    colour = lit ? status_foreground(model::Status::Error) : Rgb{};
                }
                frame[logical_index(x, y)] = mix(Rgb{}, colour, activity);
            }
        }
    }

    if (travel.active && travel.from >= 0 && travel.from < kSlotCount
        && travel.to >= 0 && travel.to < kSlotCount && travel.from != travel.to
        && input.slots[travel.from].present && input.slots[travel.to].present) {
        const DistanceMap from = field_distances_from(travel.from);
        const DistanceMap to = field_distances_from(travel.to);
        int shortest = kPixelCount;
        for (int pixel = 0; pixel < kPixelCount; ++pixel) {
            if (from[pixel] >= 0 && to[pixel] >= 0)
                shortest = std::min(shortest, from[pixel] + to[pixel]);
        }
        const Rgb destination = slot_accent(input.slots[travel.to]);
        const float position = motion::ease_in_out_cubic(travel.progress)
                             * static_cast<float>(std::max(1, shortest));
        for (int pixel = 0; pixel < kPixelCount; ++pixel) {
            if (from[pixel] < 0 || to[pixel] < 0
                || from[pixel] + to[pixel] != shortest) continue;
            const float distance = std::fabs(static_cast<float>(from[pixel]) - position);
            const float glow = motion::clamp01(1.f - distance / 1.35f);
            frame[pixel] = mix(frame[pixel], mix(Rgb{}, destination, 0.78f), glow);
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

            // A 4x6 cell centres exactly on the even 8x8 panel: two columns
            // of margin on each side and one row above and below.
            static constexpr uint8_t digits[kSlotCount][6] = {
                {0b0110, 0b1110, 0b0110, 0b0110, 0b0110, 0b1111},
                {0b0110, 0b1001, 0b0001, 0b0010, 0b0100, 0b1111},
                {0b1110, 0b0001, 0b0110, 0b0001, 0b0001, 0b1110},
                {0b1001, 0b1001, 0b1111, 0b0001, 0b0001, 0b0001},
                {0b1111, 0b1000, 0b1110, 0b0001, 0b0001, 0b1110},
                {0b0111, 0b1000, 0b1110, 0b1001, 0b1001, 0b0110},
            };
            const Rgb foreground = status_foreground(takeover.status);
            for (int row = 0; row < 6; ++row) {
                for (int col = 0; col < 4; ++col) {
                    if ((digits[takeover.slot][row] & (1u << (3 - col))) == 0) continue;
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
