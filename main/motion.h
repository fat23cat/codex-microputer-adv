// Motion primitives.
//
// Every animated value on the deck is a damped spring rather than a fixed-length
// tween: springs retarget mid-flight without a visible restart, which matters
// because the host can push a new selection while the previous one is still
// travelling.
#pragma once

#include <cmath>

namespace motion {

struct Spring {
    float x = 0.f;       // current value
    float v = 0.f;       // velocity
    float target = 0.f;

    void snap(float value) { x = target = value; v = 0.f; }
    void to(float value) { target = value; }

    // omega: angular frequency (higher = faster). zeta: damping ratio;
    // 1.0 is critically damped, ~0.7 gives the slight settle that reads as
    // physical without looking springy.
    void step(float dt, float omega = 20.f, float zeta = 0.85f)
    {
        // Clamp dt so a stalled frame (SD write, BLE notify) cannot explode the
        // integrator into a visible jump.
        if (dt > 0.05f) dt = 0.05f;
        const float force = -omega * omega * (x - target) - 2.f * zeta * omega * v;
        v += force * dt;
        x += v * dt;
        if (settled()) { x = target; v = 0.f; }
    }

    bool settled() const
    {
        return std::fabs(x - target) < 0.12f && std::fabs(v) < 0.6f;
    }
};

// Normalised 0->1 ramp used for one-shot reveals (boot, toast, screen push).
struct Ramp {
    float t = 1.f;
    float speed = 4.f;

    void restart(float rate = 4.f) { t = 0.f; speed = rate; }
    void finish() { t = 1.f; }
    bool running() const { return t < 1.f; }
    void step(float dt)
    {
        if (dt > 0.05f) dt = 0.05f;
        t += speed * dt;
        if (t > 1.f) t = 1.f;
    }
};

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// One timing scale for the whole device, so unrelated elements never disagree
// about how fast "fast" is.
constexpr float kFast = 0.16f;   // marks, indicators
constexpr float kBase = 0.22f;   // numerals, titles
constexpr float kSlow = 0.36f;   // whole-screen changes

// Staggering is what turns several moving parts into one gesture: the eye reads
// the order as cause and effect rather than as things twitching together.
constexpr float kStagTape   = 0.00f;
constexpr float kStagNumber = 0.04f;
constexpr float kStagStatus = 0.07f;
constexpr float kStagTitle  = 0.10f;

// Progress of one staged element, given seconds since the gesture started.
inline float staged(float elapsed, float delay, float duration)
{
    return clamp01((elapsed - delay) / duration);
}

inline float ease_out_cubic(float t)
{
    t = clamp01(t);
    const float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

inline float ease_in_out_cubic(float t)
{
    t = clamp01(t);
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}

// Symmetric 0->1->0 pulse, for breathing indicators.
inline float pulse(float phase)
{
    return 0.5f - 0.5f * std::cos(phase * 6.28318f);
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace motion
