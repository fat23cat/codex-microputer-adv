#include "audio.h"

#include <M5Unified.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include "esp_random.h"
#include "esp_timer.h"
#include "model.h"
#include "motion.h"
#include "status_timing.h"

namespace audio {
namespace {

bool     speaker_live   = false;
std::atomic<uint32_t> requested_token{0};
std::atomic<uint32_t> prepared_token{0};
std::atomic<uint8_t> requested_cue{0};
std::atomic<uint8_t> prepared_cue{0};
std::atomic<int8_t> prepared_buffer{-1};
std::atomic<int8_t> armed_buffer{-1};
std::atomic<int8_t> playing_buffer{-1};
TaskHandle_t synth_task_handle = nullptr;

uint8_t hardware_volume()
{
    // Before user volume control existed, every sound played at hardware 150.
    // Keep that original loudness at the default 60%, while retaining useful
    // adjustment below it and exposing the speaker's remaining headroom above.
    return static_cast<uint8_t>((static_cast<unsigned>(model::state.sound_volume) * 250u + 50u) / 100u);
}

void ensure_speaker()
{
    if (speaker_live) return;
    if (M5.Mic.isEnabled()) M5.Mic.end();
    speaker_live = M5.Speaker.begin();
    // 110 was set for tone() cues, which are pure and carry easily. A percussive
    // sample spends most of its length decaying, so it needs the headroom.
    if (speaker_live) M5.Speaker.setVolume(hardware_volume());
}

// A key press should sound like something being *struck*, not like a tone being
// held. tone() can only hold a pitch, and any pitch short enough not to sing
// reads as a chirp -- so the press cue is synthesised instead: three damped sines
// in octaves for the body, and a few milliseconds of dulled noise for the moment
// of contact. Depth here is mostly the envelope and the octave pairing, not the
// fundamental, because almost nothing below 200 Hz leaves a case this size.
constexpr uint32_t kCueRate   = 16000;
constexpr size_t   kThockLen  = 1280;  // 80 ms: tactile, with no synthetic tail
int16_t thock_pcm[kThockLen]  = {};
constexpr size_t kControlLen = 1760;  // 110 ms, short but recognisably melodic
int16_t control_pcm[4][kControlLen] = {};
// The status score follows the complete 3.56 s visual arc. 8 kHz is ample for
// this speaker and keeps the persistent DMA buffer at a responsible 57 kB.
constexpr uint32_t kStatusRate = 8000;
constexpr float    kStatusSeconds = status_timing::visual_life
                                  + status_timing::audio_base_lag
                                  + status_timing::audio_max_offset;
constexpr size_t   kStatusLen = static_cast<size_t>(kStatusRate * kStatusSeconds);
int16_t status_pcm[2][kStatusLen] = {};
constexpr float    kBootSeconds = 1.60f;
constexpr size_t   kBootLen = static_cast<size_t>(kCueRate * kBootSeconds);
static_assert(kBootLen <= kStatusLen,
              "the boot cue reuses one status buffer after startup");

// libm trigonometry is extremely expensive on ESP32-S3: one status score took
// 1.4-1.9 seconds to render and froze the UI. An interpolated lookup table is
// acoustically transparent on this speaker and removes that critical-path cost.
constexpr size_t kSineTableSize = 1024;
float sine_table[kSineTableSize + 1] = {};
bool sine_table_ready = false;

void prepare_sine_table()
{
    if (sine_table_ready) return;
    for (size_t i = 0; i <= kSineTableSize; ++i) {
        sine_table[i] = std::sin(6.283185307f * static_cast<float>(i)
                               / static_cast<float>(kSineTableSize));
    }
    sine_table_ready = true;
}

inline float fast_sin(float radians)
{
    float cycle = radians * 0.159154943f;
    cycle -= std::floor(cycle);
    const float position = cycle * static_cast<float>(kSineTableSize);
    const size_t index = static_cast<size_t>(position);
    const float fraction = position - static_cast<float>(index);
    return sine_table[index]
         + (sine_table[index + 1] - sine_table[index]) * fraction;
}

// A real key never sounds exactly the same twice. Eight deterministic variants
// trace a restrained pentatonic contour and vary decay slightly, so the result
// feels played rather than randomly pitch-shifted.
constexpr int kVariants = 8;
// A tiny pentatonic contour rather than random detune. Consecutive presses have
// character, while the range stays narrow enough to remain one instrument.
constexpr float kBodyHz[kVariants] = {220.f, 233.f, 247.f, 233.f, 220.f, 247.f, 233.f, 220.f};
constexpr float kDecayScale[kVariants] = {1.00f, 0.96f, 1.03f, 0.98f, 1.02f, 0.95f, 1.01f, 0.97f};
int variant = 0;

void build_thock(int which)
{
    const float body   = kBodyHz[which];
    const float decay  = kDecayScale[which];
    // Seeded per variant, so each one has its own contact noise but every play of
    // the same variant is identical.
    uint32_t noise = 0x2545F491u + static_cast<uint32_t>(which) * 0x9E3779B9u;
    float    prev_white = 0.f;
    for (size_t i = 0; i < kThockLen; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kCueRate);
        // The fundamental is Bb3, roughly a fifth below where it was. It sits
        // under what this transducer can really radiate, so the octave above it
        // is carried at almost equal weight: the ear fuses the pair and hears the
        // lower note, which is the only way to get depth out of a speaker this
        // size. Dropping the fundamental alone -- as 165 Hz did -- just makes the
        // cue quiet, not deep. The partials still die first, so the tail is low.
        float v = std::sin(6.28318f * body * t) * std::exp(-t / (0.032f * decay))
                + std::sin(6.28318f * body * 2.01f * t) * std::exp(-t / (0.018f * decay)) * 0.70f
                + std::sin(6.28318f * body * 3.02f * t) * std::exp(-t / (0.006f * decay)) * 0.14f;
        // The contact transient. Short enough to be felt rather than heard, and
        // it is what keeps the cue from sounding like a soft synth pad.
        if (t < 0.004f) {
            noise = noise * 1664525u + 1013904223u;
            const float white = static_cast<float>(static_cast<int32_t>(noise >> 8) % 2000 - 1000) / 1000.f;
            // Low-passed by averaging with the previous sample: raw white noise
            // is a bright tick, which is the thing we are getting away from.
            const float dull = (white + prev_white) * 0.5f;
            prev_white = white;
            v += dull * 0.20f * std::exp(-t / 0.0010f);
        }
        // Half a millisecond of fade-in: starting on a non-zero sample would add
        // a click of its own, and a bright one at that.
        if (t < 0.0005f) v *= t / 0.0005f;
        const float clamped = std::max(-1.f, std::min(1.f, v * 0.48f));
        thock_pcm[i] = static_cast<int16_t>(clamped * 32000.f);
    }
}

void thock()
{
    ensure_speaker();
    if (!speaker_live) return;
    // Stop before rebuilding: the buffer we are about to overwrite is the one the
    // DMA is still reading from if a press lands during the previous knock.
    M5.Speaker.stop();
    build_thock(variant);
    variant = (variant + 1) % kVariants;
    // Cut any cue still ringing: two knocks overlapping sound like a rattle.
    M5.Speaker.playRaw(thock_pcm, kThockLen, kCueRate, false, 1, -1, true);
}

void build_control_cues()
{
    // One soft instrument, four distinct gestures. Apply is a compact rising
    // major voicing: it reads as accepted/successful instead of the old octave
    // drop, which sounded like cancellation. Navigation remains two-note and
    // directional, so confirmation cannot be mistaken for another detent.
    constexpr float notes[4][3] = {
        {392.f, 523.25f, 0.f},    // open: G4 -> C5
        {523.25f, 659.25f, 783.99f}, // apply: C5 -> E5 -> G5
        {659.25f, 523.25f, 0.f},  // left: E5 -> C5
        {523.25f, 659.25f, 0.f}   // right: C5 -> E5
    };
    for (int cue = 0; cue < 4; ++cue) {
        for (size_t i = 0; i < kControlLen; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kCueRate);
            float v = 0.f;
            for (int note = 0; note < 3 && notes[cue][note] > 0.f; ++note) {
                const float local = t - note * (cue == 1 ? 0.027f : 0.038f);
                if (local < 0.f) continue;
                const float attack = std::min(1.f, local / 0.006f);
                const float decay = cue == 1 && note == 2 ? 0.046f : 0.034f;
                const float env = attack * std::exp(-local / decay);
                v += (fast_sin(6.28318f * notes[cue][note] * local) * 0.42f
                    + fast_sin(6.28318f * notes[cue][note] * 2.f * local) * 0.08f)
                   * env;
            }
            control_pcm[cue][i] = static_cast<int16_t>(
                std::clamp(v, -1.f, 1.f) * 23000.f);
        }
    }
}

void play_control(int index)
{
    ensure_speaker();
    if (!speaker_live) return;
    M5.Speaker.stop();
    M5.Speaker.playRaw(control_pcm[index], kControlLen, kCueRate,
                       false, 1, -1, true);
}

bool build_status_gesture(Cue cue, int16_t* output)
{
    // Vary the key, never the identity. Consonant events transpose their whole
    // interval shape; attention/error may also choose one of a few deliberately
    // unresolved interval ratios. Rhythm and envelope remain invariant.
    // Four registers are far enough apart to survive this tiny transducer.
    // Keep one history cell per semantic cue and forbid an immediate repeat,
    // otherwise true randomness often sounds like no variation at all.
    static constexpr int8_t kTranspose[] = {-4, 0, 3, 6};
    static uint8_t previous_variant[5] = {0xff, 0xff, 0xff, 0xff, 0xff};
    const uint32_t variation = esp_random();
    const uint8_t cue_index = cue == Cue::Running ? 0
                            : cue == Cue::Done ? 1
                            : cue == Cue::Attention ? 2
                            : cue == Cue::Error ? 3 : 4;
    uint8_t variant_index = variation % 4;
    if (variant_index == previous_variant[cue_index])
        variant_index = (variant_index + 1 + ((variation >> 12) & 1u)) % 4;
    previous_variant[cue_index] = variant_index;
    const int semitones = kTranspose[variant_index];
    const float pitch = std::pow(2.f, static_cast<float>(semitones) / 12.f);
    static constexpr float kTenseRatio[] = {1.4142f, 1.4667f, 1.5833f};
    const float tense_ratio = kTenseRatio[(variant_index + (variation >> 8)) % 3];

    const int64_t render_started_us = esp_timer_get_time();
    uint32_t noise = 0xB5297A4Du ^ variation;
    for (size_t i = 0; i < kStatusLen; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kStatusRate);
        // The first/last rail gestures remain tactile and silent. The musical
        // clock begins with the visible colour surface and ends before the rail
        // returns, so perceived duration follows what is actually on screen.
        const float configured_lag = status_timing::audio_base_lag
            + static_cast<float>(model::state.status_audio_offset_ms) / 1000.f;
        const float audible_t = t - configured_lag;
        const float score_t = audible_t * status_timing::audio_scale;
        if (audible_t < 0.f || score_t >= status_timing::visual_life) {
            output[i] = 0;
            continue;
        }
        const float hold_in = std::clamp(
            (score_t - (status_timing::colour + status_timing::expand))
            / status_timing::centre, 0.f, 1.f);
        const float hold_out = std::clamp(
            (status_timing::visual_life - score_t) / status_timing::return_life,
            0.f, 1.f);
        const float bed = hold_in * hold_out;
        const float return_t = std::max(0.f,
            score_t - status_timing::return_start);
        const float cue_t = score_t;
        const float gesture_t = cue_t - status_timing::initial_gesture_delay;
        float v = 0.f;
        if (cue == Cue::Running) {
            // A compact accelerating rotor: an upward chirp with a quiet octave.
            if (gesture_t >= 0.f) {
                const float phase = 6.28318f * pitch
                                  * (300.f * gesture_t + 750.f * gesture_t * gesture_t);
                const float env = fast_sin(std::min(1.f, gesture_t / 0.16f) * 3.14159f)
                                * std::exp(-gesture_t / 0.20f);
                v = (fast_sin(phase) * 0.62f + fast_sin(phase * 2.01f) * 0.18f) * env;
            }
            // Track under the hold: an asymmetric motor pulse, quiet but alive.
            const float pulse = 0.48f + 0.30f * fast_sin(6.28318f * 1.73f * cue_t)
                              + 0.12f * fast_sin(6.28318f * 4.11f * cue_t + 0.8f);
            v += (fast_sin(6.28318f * 220.f * pitch * cue_t) * 0.065f
                + fast_sin(6.28318f * 330.f * pitch * cue_t) * 0.035f)
               * std::clamp(pulse, 0.08f, 0.9f) * bed;
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 440.f * pitch * return_t)
                   * std::exp(-return_t / 0.24f) * 0.09f;
            }
        } else if (cue == Cue::Done) {
            // Three staggered bell partials resolve into one bright consonance.
            if (gesture_t >= 0.f) {
                const float on1 = gesture_t;
                const float on2 = std::max(0.f, gesture_t - 0.035f);
                const float on3 = std::max(0.f, gesture_t - 0.070f);
                v = fast_sin(6.28318f * pitch * 659.f * on1) * std::exp(-on1 / 0.090f) * 0.42f;
                if (gesture_t >= 0.035f) v += fast_sin(6.28318f * pitch * 831.f * on2) * std::exp(-on2 / 0.085f) * 0.38f;
                if (gesture_t >= 0.070f) v += fast_sin(6.28318f * pitch * 988.f * on3) * std::exp(-on3 / 0.100f) * 0.34f;
            }
            // A consonant major bed holds the completed state without another melody.
            v += (fast_sin(6.28318f * 329.6f * pitch * cue_t) * 0.040f
                + fast_sin(6.28318f * 415.3f * pitch * cue_t) * 0.032f
                + fast_sin(6.28318f * 493.9f * pitch * cue_t) * 0.028f) * bed;
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 659.f * pitch * return_t)
                   * std::exp(-return_t / 0.28f) * 0.075f;
            }
        } else if (cue == Cue::Attention) {
            // Two dry syncopated knocks with a metallic upper sideband.
            const float local = gesture_t < 0.085f ? gesture_t : gesture_t - 0.095f;
            if (local >= 0.f && local < 0.060f) {
                const float env = std::exp(-local / 0.020f);
                const float root = 740.f * pitch;
                v = (fast_sin(6.28318f * root * local) * 0.55f
                   + fast_sin(6.28318f * root * tense_ratio * local) * 0.25f) * env;
            }
            const float raw_pulse = std::max(0.f,
                fast_sin(6.28318f * 1.35f * cue_t + 0.4f));
            const float pulse = raw_pulse * raw_pulse * raw_pulse * raw_pulse;
            const float root = 246.f * pitch;
            v += (fast_sin(6.28318f * root * cue_t) * 0.060f
                + fast_sin(6.28318f * root * tense_ratio * cue_t) * 0.045f)
               * pulse * bed;
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * root * return_t)
                   * std::exp(-return_t / 0.18f) * 0.07f;
            }
        } else if (cue == Cue::Error) {
            // A descending, slightly roughened interval that refuses to resolve.
            noise = noise * 1664525u + 1013904223u;
            const float grit = static_cast<float>(static_cast<int32_t>(noise >> 9) % 1000 - 500) / 500.f;
            if (gesture_t >= 0.f) {
                const float phase = 6.28318f * pitch
                                  * (620.f * gesture_t - 900.f * gesture_t * gesture_t);
                v = (fast_sin(phase) * 0.52f + grit * 0.10f)
                  * std::exp(-gesture_t / 0.095f);
            }
            // Two close, non-harmonic voices beat slowly through the hold.
            v += (fast_sin(6.28318f * 196.f * pitch * cue_t) * 0.050f
                + fast_sin(6.28318f * 207.f * pitch * cue_t) * 0.042f
                + grit * 0.010f) * bed;
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 147.f * pitch * return_t)
                   * std::exp(-return_t / 0.24f) * 0.08f;
            }
        } else { // Idle: a soft downward release, intentionally least prominent.
            if (gesture_t >= 0.f) {
                const float phase = 6.28318f * pitch
                                  * (440.f * gesture_t - 240.f * gesture_t * gesture_t);
                v = fast_sin(phase) * std::exp(-gesture_t / 0.055f) * 0.38f;
            }
            v += fast_sin(6.28318f * 220.f * pitch * cue_t) * bed * 0.035f;
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 165.f * pitch * return_t)
                   * std::exp(-return_t / 0.30f) * 0.055f;
            }
        }
        if (cue_t < 0.001f) v *= cue_t / 0.001f;
        output[i] = static_cast<int16_t>(std::clamp(v, -1.f, 1.f) * 28000.f);
    }
    const int64_t render_us = esp_timer_get_time() - render_started_us;
    std::printf("CCP_AUDIO|status=%u|render_us=%lld|samples=%u|core=%d\n",
                static_cast<unsigned>(cue), static_cast<long long>(render_us),
                static_cast<unsigned>(kStatusLen), xPortGetCoreID());
    return true;
}

void synth_task(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t token = requested_token.load(std::memory_order_acquire);
        if (token == 0) continue;
        const Cue cue = static_cast<Cue>(requested_cue.load(std::memory_order_relaxed));
        const int active = playing_buffer.load(std::memory_order_acquire);
        const int target = active == 0 ? 1 : 0;
        build_status_gesture(cue, status_pcm[target]);
        // A newer state for this chat may have replaced the request while the
        // worker was rendering. Never publish stale PCM in that case.
        if (requested_token.load(std::memory_order_acquire) != token
            || requested_cue.load(std::memory_order_relaxed)
                != static_cast<uint8_t>(cue)) {
            continue;
        }
        prepared_cue.store(static_cast<uint8_t>(cue), std::memory_order_relaxed);
        prepared_buffer.store(static_cast<int8_t>(target), std::memory_order_relaxed);
        prepared_token.store(token, std::memory_order_release);
    }
}

enum class StartupVoice : uint8_t { Pluck, Bell, Soft, Pulse, Glass, Organ };
enum class StartupPad : uint8_t { None, Tail, Swell, Rhythm };

struct StartupNote {
    int8_t semitone;             // above C5
    uint16_t start_ms;
    uint16_t decay_ms;
    uint8_t level;
};

struct StartupComposition {
    const char* name;
    StartupNote notes[8];
    uint8_t note_count;
    int8_t root_semitone;
    bool minor;
    StartupVoice voice;
    StartupPad pad;
    uint16_t pad_start_ms;
};

#define N(semi, start, decay, level) StartupNote{semi, start, decay, level}
constexpr StartupComposition kStartupCompositions[kStartupChimeCount] = {
    {"DUO",     {N(7,70,170,175),N(12,355,250,195)}, 2,12,false,StartupVoice::Soft,StartupPad::Tail,315},
    {"BLOOM",   {N(0,80,260,145),N(4,80,260,125),N(7,80,260,120),N(12,470,320,170)}, 4,12,false,StartupVoice::Organ,StartupPad::Swell,390},
    {"DEW",     {N(9,65,230,150),N(4,250,240,140),N(11,470,260,150),N(7,710,300,165)}, 4,7,false,StartupVoice::Glass,StartupPad::Tail,650},
    {"CLOUD",   {N(-5,70,320,165),N(2,390,340,155),N(7,760,380,180)}, 3,7,false,StartupVoice::Soft,StartupPad::Swell,610},
    {"LULL",    {N(0,70,150,170),N(3,230,160,160),N(7,410,180,165),N(3,610,210,155),N(0,830,300,180)}, 5,0,true,StartupVoice::Pluck,StartupPad::Tail,760},
    {"PEARL",   {N(0,60,170,150),N(4,180,180,145),N(7,310,190,140),N(12,460,220,155),N(7,650,240,145)}, 5,7,false,StartupVoice::Bell,StartupPad::None,0},
    {"WARMTH",  {N(-5,70,300,130),N(0,70,300,125),N(4,70,300,115),N(0,520,340,145),N(4,520,340,130),N(7,520,340,120)}, 6,0,false,StartupVoice::Organ,StartupPad::Tail,450},
    {"NIGHT",   {N(10,80,240,150),N(7,300,250,145),N(3,540,280,150),N(0,820,360,175)}, 4,0,true,StartupVoice::Soft,StartupPad::Swell,720},
    {"DAWN",    {N(-3,70,170,155),N(0,220,180,150),N(4,390,190,150),N(7,580,220,160),N(12,810,320,185)}, 5,12,false,StartupVoice::Pluck,StartupPad::Tail,740},
    {"HUSH",    {N(4,90,260,145),N(0,420,300,150),N(7,780,380,170)}, 3,7,false,StartupVoice::Soft,StartupPad::None,0},
};
#undef N

float startup_voice(StartupVoice voice, float phase)
{
    switch (voice) {
        case StartupVoice::Bell:
            return fast_sin(phase) * 0.80f + fast_sin(phase * 2.01f) * 0.14f
                 + fast_sin(phase * 3.97f) * 0.06f;
        case StartupVoice::Soft:
            return fast_sin(phase) * 0.82f + fast_sin(phase * 0.5f) * 0.18f;
        case StartupVoice::Pulse:
            return fast_sin(phase) * 0.72f + fast_sin(phase * 3.f) * 0.18f
                 + fast_sin(phase * 5.f) * 0.10f;
        case StartupVoice::Glass:
            return fast_sin(phase) * 0.76f + fast_sin(phase * 2.99f) * 0.16f
                 + fast_sin(phase * 5.03f) * 0.08f;
        case StartupVoice::Organ:
            return fast_sin(phase) * 0.62f + fast_sin(phase * 2.f) * 0.25f
                 + fast_sin(phase * 3.f) * 0.13f;
        default:
            return fast_sin(phase) * 0.86f + fast_sin(phase * 2.f) * 0.14f;
    }
}

void boot_music()
{
    const int64_t render_started_us = esp_timer_get_time();
    ensure_speaker();
    if (!speaker_live) return;
    M5.Speaker.stop();

    prepare_sine_table();
    // Keep variation close to concert pitch. The former +/-6-semitone range
    // pushed the tiny speaker into a brittle, horror-box register.
    static constexpr int8_t shifts[] = {-2, 0, 2};
    const uint32_t variation = esp_random();
    const float pitch = std::pow(2.f,
        static_cast<float>(shifts[variation % 3]) / 12.f);
    const StartupComposition& composition =
        kStartupCompositions[model::state.startup_chime % kStartupChimeCount];
    std::printf("CCP_CHIME|variant=%u|name=%s\n",
                static_cast<unsigned>(model::state.startup_chime), composition.name);
    const float resolution_hz = 523.251f * std::pow(2.f,
        static_cast<float>(composition.root_semitone) / 12.f);
    const float chord_root = resolution_hz * 0.25f;
    const float chord_third = chord_root * (composition.minor ? 1.189207f : 1.259921f);
    const float chord_fifth = chord_root * 1.498307f;
    float note_hz[8] = {};
    for (int note = 0; note < composition.note_count; ++note) {
        note_hz[note] = 523.251f * std::pow(2.f,
            static_cast<float>(composition.notes[note].semitone) / 12.f) * pitch;
    }

    for (size_t i = 0; i < kBootLen; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kCueRate);

        // Pad behaviour is part of the composition, not a global layer pasted
        // under every melody. Some pieces are deliberately dry.
        float pad = 0.f;
        const float pad_start = static_cast<float>(composition.pad_start_ms) / 1000.f;
        if (composition.pad != StartupPad::None && t >= pad_start) {
            const float local = t - pad_start;
            const float attack_time = composition.pad == StartupPad::Swell ? 0.26f : 0.075f;
            const float attack = motion::ease_out_cubic(std::min(1.f, local / attack_time));
            const float decay = std::exp(-local /
                (composition.pad == StartupPad::Swell ? 0.82f : 0.58f));
            const float end_fade = std::min(1.f, (kBootSeconds - t) / 0.12f);
            float modulation = 1.f;
            if (composition.pad == StartupPad::Rhythm)
                modulation = 0.38f + 0.62f * std::max(0.f,
                    fast_sin(6.28318f * 4.f * local));
            else if (composition.pad == StartupPad::Swell)
                modulation = 0.90f + 0.10f * fast_sin(6.28318f * 0.9f * local);
            pad = (fast_sin(6.28318f * chord_root * pitch * local) * 0.055f
                 + fast_sin(6.28318f * chord_third * pitch * local) * 0.090f
                 + fast_sin(6.28318f * chord_fifth * pitch * local) * 0.072f
                 + fast_sin(6.28318f * chord_root * 2.f * pitch * local) * 0.055f
                 + fast_sin(6.28318f * chord_third * 1.004f * pitch * local) * 0.026f)
                * attack * decay * end_fade * modulation;
        }

        // Each score owns its note count, rhythm, envelope and instrument.
        float lead = 0.f;
        for (int note = 0; note < composition.note_count; ++note) {
            const StartupNote& event = composition.notes[note];
            const float local = t - static_cast<float>(event.start_ms) / 1000.f;
            const float decay_time = static_cast<float>(event.decay_ms) / 1000.f;
            if (local < 0.f || local > std::min(1.2f, decay_time * 5.f)) continue;
            const float attack_time = composition.voice == StartupVoice::Organ ? 0.035f
                                    : composition.voice == StartupVoice::Soft ? 0.020f : 0.008f;
            const float attack = std::min(1.f, local / attack_time);
            const float env = attack * std::exp(-local / decay_time)
                            * (static_cast<float>(event.level) / 255.f);
            lead += startup_voice(composition.voice, 6.28318f * note_hz[note] * local)
                  * env * 0.23f;
        }

        const float mixed = std::clamp(pad + lead, -1.f, 1.f);
        status_pcm[0][i] = static_cast<int16_t>(mixed * 25000.f);
    }
    std::printf("CCP_CHIME_READY|variant=%u|render_us=%lld\n",
                static_cast<unsigned>(model::state.startup_chime),
                static_cast<long long>(esp_timer_get_time() - render_started_us));
    // Reserve buffer 0 before DMA starts. Codex often reconnects while the boot
    // score is still playing; without this marker the status worker also chose
    // buffer 0 and replaced CLOUD in flight, reducing it to a DUO-like fragment.
    playing_buffer.store(0, std::memory_order_release);
    M5.Speaker.playRaw(status_pcm[0], kBootLen, kCueRate, false, 1, -1, true);
}

}  // namespace

const char* startup_chime_name(uint8_t index)
{
    return kStartupCompositions[index % kStartupChimeCount].name;
}

void init()
{
    ensure_speaker();
    prepare_sine_table();
    build_control_cues();
    xTaskCreatePinnedToCore(synth_task, "status_synth", 4096, nullptr, 1,
                            &synth_task_handle, 1);
}

void apply_volume()
{
    ensure_speaker();
    if (speaker_live) M5.Speaker.setVolume(hardware_volume());
}

void request_status(Cue cue, uint32_t token)
{
    if (model::state.sound_volume == 0 || token == 0 || !synth_task_handle) return;
    if (requested_token.load(std::memory_order_acquire) == token
        && requested_cue.load(std::memory_order_relaxed) == static_cast<uint8_t>(cue))
        return;
    requested_cue.store(static_cast<uint8_t>(cue), std::memory_order_relaxed);
    requested_token.store(token, std::memory_order_release);
    xTaskNotifyGive(synth_task_handle);
}

bool status_ready(Cue cue, uint32_t token)
{
    if (model::state.sound_volume == 0) return true;
    return prepared_token.load(std::memory_order_acquire) == token
        && prepared_cue.load(std::memory_order_relaxed) == static_cast<uint8_t>(cue)
        && prepared_buffer.load(std::memory_order_relaxed) >= 0;
}

bool arm_status(Cue cue, uint32_t token)
{
    if (model::state.sound_volume == 0) return false;
    if (!status_ready(cue, token)) return false;
    armed_buffer.store(prepared_buffer.load(std::memory_order_relaxed),
                       std::memory_order_release);
    return true;
}

void play_prepared_status()
{
    const int target = armed_buffer.exchange(-1, std::memory_order_acq_rel);
    if (target < 0 || !speaker_live || model::state.sound_volume == 0) return;
    playing_buffer.store(static_cast<int8_t>(target), std::memory_order_release);
    std::printf("CCP_AUDIO_PLAY|buffer=%d|core=%d\n", target, xPortGetCoreID());
    M5.Speaker.playRaw(status_pcm[target], kStatusLen, kStatusRate,
                       false, 1, -1, true);
}

void play(Cue cue)
{
    // The mute cue itself must be audible, otherwise turning sound back on
    // gives no confirmation that it worked.
    if (model::state.sound_volume == 0 && cue != Cue::Unmute) return;
    // Short, dry, percussive. These are instrument blips, not alerts: the
    // interval carries the meaning and the envelope stays out of the way, so
    // hearing one twenty times an hour never becomes nagging.
    switch (cue) {
        case Cue::Select:     // a knock: low, dry, over before it can annoy
            thock(); break;
        case Cue::MenuOpen:  play_control(0); break;
        case Cue::MenuApply: play_control(1); break;
        case Cue::StepLeft:  play_control(2); break;
        case Cue::StepRight: play_control(3); break;
        case Cue::Running:
        case Cue::Done:
        case Cue::Attention:
        case Cue::Error:
        case Cue::Idle:
            // Status cues are owned by the announcement queue. Direct playback
            // would bypass debounce and the first-frame visual contract.
            break;
        case Cue::Unmute:     // two knocks, so "on" is the press cue confirming itself
            thock();
            vTaskDelay(pdMS_TO_TICKS(85));
            thock();
            break;
        case Cue::Boot: {
            boot_music(); break;
        }
    }
}

}  // namespace audio
