#pragma once

// One timing contract for the full-screen status gesture and its score.
// The rails bookend the visible colour surface; music is intentionally silent
// during those bookends and follows the surface between them.
namespace status_timing {

constexpr float colour   = 0.12f;
constexpr float rail_out = 0.24f;
constexpr float expand   = 0.22f;
constexpr float centre   = 0.18f;
// Keep a clear apex, but begin the return 150 ms earlier so the visual tail
// lands with the audible phrase instead of hanging after it.
constexpr float hold     = 1.85f;
constexpr float returning = 0.18f;
constexpr float collapse = 0.22f;
constexpr float settle   = 0.12f;
constexpr float rail_in  = 0.28f;

constexpr float visual_life = colour + expand + centre + hold
                            + returning + collapse + settle;
constexpr float life = rail_out + visual_life + rail_in;
constexpr float hold_start = colour + expand + centre;
constexpr float return_start = hold_start + hold;
constexpr float return_life = returning + collapse + settle;
// The first colour frame is mathematically non-zero but still imperceptible on
// the LCD. Keep the attack behind the complete colour reveal so the movement
// is visibly under way before the speaker starts. The score keeps its natural
// clock; the final rail gives it room to finish without tempo compression.
constexpr float audio_base_lag = colour;
// The score itself starts in the right place, but the short semantic motion
// gesture (chirp / bell / knock / sweep) needs a little more visual lead than
// the sustained bed. Delay that layer only; do not move or stretch the score.
constexpr float initial_gesture_delay = 0.025f;
constexpr float audio_max_offset = 0.300f;
constexpr float audio_scale = 1.0f;

}  // namespace status_timing
