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
constexpr int kStatusChannel = 0;
constexpr int kInterfaceChannel = 1;
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

// exp() is a library call for the same reason, and every score evaluates
// several of them per sample to shape its envelopes -- it cost more than the
// oscillators did. The same interpolated-table treatment is exact to far
// better than 16-bit audio needs, and the span ends where exp(-x) has already
// fallen below one LSB, which turns every long tail into an early return.
constexpr size_t kDecayTableSize = 512;
constexpr float  kDecayTableSpan = 16.f;
float decay_table[kDecayTableSize + 1] = {};

void prepare_sine_table()
{
    if (sine_table_ready) return;
    for (size_t i = 0; i <= kSineTableSize; ++i) {
        sine_table[i] = std::sin(6.283185307f * static_cast<float>(i)
                               / static_cast<float>(kSineTableSize));
    }
    for (size_t i = 0; i <= kDecayTableSize; ++i) {
        decay_table[i] = std::exp(-kDecayTableSpan * static_cast<float>(i)
                                / static_cast<float>(kDecayTableSize));
    }
    sine_table_ready = true;
}

// exp(-x) for x >= 0.
inline float fast_decay(float x)
{
    if (x <= 0.f) return 1.f;
    if (x >= kDecayTableSpan) return 0.f;
    const float position = x * (static_cast<float>(kDecayTableSize) / kDecayTableSpan);
    const size_t index = static_cast<size_t>(position);
    const float fraction = position - static_cast<float>(index);
    return decay_table[index]
         + (decay_table[index + 1] - decay_table[index]) * fraction;
}

inline float fast_sin(float radians)
{
    float cycle = radians * 0.159154943f;
    // std::floor is a library call on this target and it sat in the innermost
    // loop of every score. Truncation plus one correction for the descending
    // sweeps, which are the only callers that go negative, is the same result
    // for a fraction of the cost.
    int32_t whole = static_cast<int32_t>(cycle);
    if (cycle < 0.f) --whole;
    cycle -= static_cast<float>(whole);
    const float position = cycle * static_cast<float>(kSineTableSize);
    const size_t index = static_cast<size_t>(position);
    const float fraction = position - static_cast<float>(index);
    return sine_table[index]
         + (sine_table[index + 1] - sine_table[index]) * fraction;
}

// Loudness is not level: this transducer radiates almost nothing below a few
// hundred hertz, and the ear is least sensitive exactly where it gives up. A
// cue written low therefore arrives quiet however carefully its levels are
// balanced against a cue written high -- which is what made the input request
// the quietest thing on the device once it moved down a fifth, and the key
// press the quietest before that.
//
// So every voice in every score is scaled by where it sits. The correction is
// a straight tilt: flat at and above kLoudRef, and rising below it at about
// 4.5 dB per octave -- enough to bring a low note back level with a high one,
// short of the 12 dB per octave a full rolloff correction would demand. Below
// kLoudHold it stops rising: there is nothing down there to recover, and the
// only thing more level buys is cone excursion this speaker answers with a
// rattle. Depth below that point comes from octave pairing, the way the key
// press already builds it.
//
// It is applied per voice, not per cue: within one score a bed two octaves
// under its gesture has the same problem the whole cue has against another
// cue. Every frequency in a score is known before its sample loop, so the
// gains are computed once, up there, and std::pow never touches the samples.
constexpr float kLoudRef  = 760.f;
constexpr float kLoudHold = 280.f;

float voice_gain(float hz)
{
    const float f = std::max(hz, kLoudHold);
    if (f >= kLoudRef) return 1.f;
    return std::pow(kLoudRef / f, 0.75f);
}

// Correcting for register raises some voices by a factor of two, so no score
// can be written against a fixed output scale any more: what one cue peaks at
// now depends on which notes it happens to be playing. Each score is therefore
// rendered at a provisional scale, and trimmed to its target once the last
// sample is in and the true peak is known. This is also the only way the cues
// end up equally loud rather than equally scaled -- the thing that was
// actually wrong when a low cue was written at the same numbers as a high one.
//
// The hierarchy between cues is kept deliberately, in one place: a request and
// a result own the room, a fault matches them, work in progress sits under
// them, and a release is the quietest thing the device does.
constexpr float kRawScale     = 20000.f;
constexpr float kRawCeiling   = 1.6f;
constexpr float kPeakAttention = 0.95f;
constexpr float kPeakDone      = 0.95f;
constexpr float kPeakError     = 0.92f;
constexpr float kPeakRunning   = 0.80f;
constexpr float kPeakIdle      = 0.55f;
constexpr float kPeakThock     = 0.52f;
constexpr float kPeakControl   = 0.62f;
constexpr float kPeakChime     = 0.90f;

void trim_to_peak(int16_t* pcm, size_t len, float peak, float target)
{
    if (peak <= 0.0001f) return;
    const float factor = target * 32700.f / (peak * kRawScale);
    // Below the ceiling the loudest sample in the buffer is exactly
    // peak * kRawScale, so scaling it by this factor lands on target * 32700
    // and nothing can exceed it. The clamp is only needed for a score that
    // ran past the provisional ceiling, and this pass runs over every sample
    // of every score inside the debounce budget -- two compares a sample is
    // not free at that length.
    if (peak <= kRawCeiling) {
        for (size_t i = 0; i < len; ++i)
            pcm[i] = static_cast<int16_t>(static_cast<float>(pcm[i]) * factor);
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        const float scaled = static_cast<float>(pcm[i]) * factor;
        pcm[i] = static_cast<int16_t>(std::clamp(scaled, -32700.f, 32700.f));
    }
}

// The ask's three notes are struck and left to ring, but they are not the
// completion bell and must not be mistaken for it. Building them from the
// bell's spectrum, register and even roll was what made the two cues sound
// like the same event: a task finishing and a task asking are the opposite
// news, and telling them apart must not depend on noticing that one of them
// resolves. Three things separate them now, and none of them is the melody.
//
// Timbre: the bell carries its second partial, an octave up, which is what
// makes it ring. The ask carries its third instead and no second at all --
// odd harmonics only, so it is hollow and reedy where the bell is glassy, the
// difference between a stopped pipe and struck metal. It is also what lets
// the ask speak from a register this transducer cannot really radiate: the
// third partial does the carrying while the fundamental supplies the warmth.
//
// Register: the ask sits a fifth under the bell, around 260-400 Hz where
// completion never goes. Low is the point -- a question is asked at the
// bottom of the voice and a result is announced at the top of it.
//
// Rhythm: the bell rolls its three partials evenly, 35 ms apart. The ask is
// speech, not a roll: two notes close together and then a gap two and a half
// times as long before the one it settles on.
//
// It is also the one gesture that asks rather than reports, so it is the one
// allowed a soft edge: twenty milliseconds of squared attack is far short of
// the swell a blown voice needs -- the note is still struck -- but it takes
// the corner off the onset.
constexpr float kCallTau   = 0.135f;
constexpr float kMidTau    = 0.160f;
constexpr float kAnswerTau = 0.205f;
constexpr float kMidAt     = 0.068f;
constexpr float kAnswerAt  = 0.255f;
constexpr float kAskAttack = 0.020f;
// The note the phrase settles on gets nearly twice the onset of the two that
// reach it, and no note bends its pitch: a rising leading tone is the shape of
// a spoken question, but on a pure tone through a transducer this small it is
// simply a whine. The phrase asks by where it stops, not by pulling upward.
constexpr float kAnswerAttack = 0.038f;
// The glint over the peak of the arc. Short: it is a highlight on the note,
// not a fourth note.
constexpr float kShimmerTau = 0.045f;
// A struck sine is inaudible long before fast_decay() runs off the end of its
// table, and every sample past that point is a voice computing silence at
// full price through most of a 3.3 s score. Three notes could not be afforded
// at the old cut and still leave the render inside the debounce; they can at
// this one. The decay is shifted so it reaches exactly zero there: a voice
// stopped mid-decay leaves a step in the output however quiet it was.
constexpr float kAskSpan   = 6.f;
constexpr float kAskFloor  = 0.00247875f;   // fast_decay(kAskSpan)

// Every ask voice shares one spectrum, so the three read as one instrument
// speaking three times; only the onset and the decay differ between them.
inline float ask_voice(float freq, float onset, float t, float tau)
{
    const float attack = std::min(1.f, t / onset);
    const float decay = (fast_decay(t / tau) - kAskFloor) * (1.f / (1.f - kAskFloor));
    const float phase = 6.28318f * freq * t;
    return (fast_sin(phase) + fast_sin(phase * 3.f) * 0.13f)
         * attack * attack * decay;
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
    // The press is the lowest thing the device plays, so it is the voice the
    // register correction was most obviously missing: its fundamental was
    // being asked to carry the cue from a place this speaker cannot radiate.
    const float g_body = voice_gain(body);
    const float g_second = voice_gain(body * 2.01f);
    const float g_third = voice_gain(body * 3.02f);
    float peak = 0.f;
    for (size_t i = 0; i < kThockLen; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kCueRate);
        // The fundamental is Bb3, roughly a fifth below where it was. It sits
        // under what this transducer can really radiate, so the octave above it
        // is carried at almost equal weight: the ear fuses the pair and hears the
        // lower note, which is the only way to get depth out of a speaker this
        // size. Dropping the fundamental alone -- as 165 Hz did -- just makes the
        // cue quiet, not deep. The partials still die first, so the tail is low.
        float v = std::sin(6.28318f * body * t) * std::exp(-t / (0.032f * decay)) * g_body
                + std::sin(6.28318f * body * 2.01f * t) * std::exp(-t / (0.018f * decay)) * 0.70f * g_second
                + std::sin(6.28318f * body * 3.02f * t) * std::exp(-t / (0.006f * decay)) * 0.14f * g_third;
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
        peak = std::max(peak, std::fabs(v));
        const float clamped = std::clamp(v, -kRawCeiling, kRawCeiling);
        thock_pcm[i] = static_cast<int16_t>(clamped * kRawScale);
    }
    trim_to_peak(thock_pcm, kThockLen, peak, kPeakThock);
}

void thock()
{
    ensure_speaker();
    if (!speaker_live) return;
    // Interface sounds own channel 1. Replacing a previous knock must never
    // stop the status score playing concurrently on channel 0.
    build_thock(variant);
    variant = (variant + 1) % kVariants;
    // Cut any cue still ringing: two knocks overlapping sound like a rattle.
    M5.Speaker.playRaw(thock_pcm, kThockLen, kCueRate, false, 1,
                       kInterfaceChannel, true);
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
        float gain[3] = {}, octave_gain[3] = {};
        for (int note = 0; note < 3; ++note) {
            if (notes[cue][note] <= 0.f) continue;
            gain[note] = voice_gain(notes[cue][note]);
            octave_gain[note] = voice_gain(notes[cue][note] * 2.f);
        }
        float peak = 0.f;
        for (size_t i = 0; i < kControlLen; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kCueRate);
            float v = 0.f;
            for (int note = 0; note < 3 && notes[cue][note] > 0.f; ++note) {
                const float local = t - note * (cue == 1 ? 0.027f : 0.038f);
                if (local < 0.f) continue;
                const float attack = std::min(1.f, local / 0.006f);
                const float decay = cue == 1 && note == 2 ? 0.046f : 0.034f;
                const float env = attack * std::exp(-local / decay);
                v += (fast_sin(6.28318f * notes[cue][note] * local) * 0.42f * gain[note]
                    + fast_sin(6.28318f * notes[cue][note] * 2.f * local) * 0.08f
                      * octave_gain[note])
                   * env;
            }
            peak = std::max(peak, std::fabs(v));
            control_pcm[cue][i] = static_cast<int16_t>(
                std::clamp(v, -kRawCeiling, kRawCeiling) * kRawScale);
        }
        trim_to_peak(control_pcm[cue], kControlLen, peak, kPeakControl);
    }
}

void play_control(int index)
{
    ensure_speaker();
    if (!speaker_live) return;
    M5.Speaker.playRaw(control_pcm[index], kControlLen, kCueRate,
                       false, 1, kInterfaceChannel, true);
}

// A damped voice is silent once fast_decay() has run off the end of its table,
// and the running chirp is silent once its raised-cosine window closes. Past
// those points the oscillators are computing zeroes at full price for most of a
// 3.3 s score, so each opening gesture states how long it is actually alive.
constexpr float kChirpLife   = 0.16f;
constexpr float kBellLife    = 0.070f + kDecayTableSpan * 0.100f;
constexpr float kAskLife     = kAnswerAt + kAskSpan * kAnswerTau;
constexpr float kFaultLife   = kDecayTableSpan * 0.095f;
constexpr float kReleaseLife = kDecayTableSpan * 0.055f;

bool build_status_gesture(Cue cue, int16_t* output)
{
    // Vary the key, never the identity. Every event transposes its whole
    // interval shape; attention also chooses one of a few open call intervals.
    // Rhythm and envelope remain invariant.
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

    // A run of questions is a phrase, not one figure returned to. Each ask
    // takes the next step of a four-chord progression in one key: the bed
    // moves under it, and the three notes are drawn from the chord that is
    // standing at the time, so answering four things in a row is heard as a
    // line arriving somewhere rather than as the same request repeated. The
    // arc peaks on the third step and leans back down on the fourth, which
    // wants the first again -- the cycle closes but never resolves, so a long
    // series keeps going round without ever sounding finished.
    //
    // The key is chosen once for a series and held. Transposing every ask,
    // the way every other cue transposes every play, is exactly what would
    // stop a progression from being one: four chords in four unrelated keys
    // are four unrelated events. A new key is rolled only when a series has
    // gone quiet long enough to have ended.
    //
    // The bed always sits below the call, the way it does in every other
    // score: gesture on top, sustained chord underneath, tail last. Both
    // layers are ratios of one anchor, so neither can drift out of tune with
    // the other.
    //
    // All of this is settled here, once per score, outside the sample loop:
    // inside it the step would turn over thousands of times and every voice
    // would flip pitch from sample to sample.
    struct AskStep {
        float chord_mult;   // the bed's root, relative to the series key
        float notes[3];     // the contour, as ratios of the call
        bool  shimmer;      // the peak of the arc, and only it, is lit
    };
    // Read in A: A -> F -> C -> G. Every step lifts to its peak and then
    // settles a step below it -- an intonation contour, not a climb. The
    // completion bell climbs and lands on top; if the ask did the same thing
    // an octave down it would still be the same gesture. Settling is not
    // descending: the fault cue falls through its start and keeps going, while
    // the ask comes to rest above where it began, on a chord tone that is
    // never the root. That is what leaves it open.
    static constexpr AskStep kAskSteps[4] = {
        {1.f,      {1.f, 1.5f,    1.25f},   false}, // up to the fifth, rests on the third
        {0.8f,     {1.f, 1.3333f, 1.125f},  false}, // narrower, rests on the second
        {1.2f,     {1.f, 1.5f,    1.3333f}, true},  // the peak, and the one that glints
        {0.8889f,  {1.f, 1.25f,   1.125f},  false}, // smallest of the four, still open
    };
    // Long enough that two questions in one exchange are always one series,
    // short enough that tomorrow morning's first question is a fresh key.
    constexpr int64_t kSeriesGapUs = 30LL * 1000000LL;
    static uint8_t ask_step = 3;
    static int64_t last_ask_us = 0;
    static float series_pitch = 1.f;
    float pad_root = 0.f, call_root = 0.f, mid_note = 0.f, answer_note = 0.f;
    bool ask_shimmer = false;
    if (cue == Cue::Attention) {
        const int64_t now_us = esp_timer_get_time();
        if (last_ask_us == 0 || now_us - last_ask_us > kSeriesGapUs) {
            ask_step = 0;
            series_pitch = pitch;
        } else {
            ask_step = static_cast<uint8_t>((ask_step + 1) & 3);
        }
        last_ask_us = now_us;
        const AskStep& step = kAskSteps[ask_step];
        const float anchor = 220.f * series_pitch * step.chord_mult;
        pad_root    = anchor;
        // A fifth above the chord, not an octave: that is the register
        // completion does not use, and the odd-harmonic voice is what makes
        // it carry from down there.
        call_root   = anchor * 1.5f * step.notes[0];
        mid_note    = anchor * 1.5f * step.notes[1];
        answer_note = anchor * 1.5f * step.notes[2];
        ask_shimmer = step.shimmer;
    }

    // Every frequency this score will play is settled by now, so its register
    // correction is worked out here: std::pow must never reach the samples.
    // A swept voice is corrected at the geometric middle of its sweep -- it
    // spends the audible part of its life there, and a gain that moved with
    // the sweep would be a second envelope nobody asked for.
    const float g_run_sweep = voice_gain(500.f * pitch);
    const float g_run_low   = voice_gain(220.f * pitch);
    const float g_run_fifth = voice_gain(330.f * pitch);
    const float g_run_tail  = voice_gain(440.f * pitch);
    const float g_bell_1 = voice_gain(659.f * pitch);
    const float g_bell_2 = voice_gain(831.f * pitch);
    const float g_bell_3 = voice_gain(988.f * pitch);
    const float g_done_bed_1 = voice_gain(329.6f * pitch);
    const float g_done_bed_2 = voice_gain(415.3f * pitch);
    const float g_done_bed_3 = voice_gain(493.9f * pitch);
    const float g_call    = voice_gain(call_root);
    const float g_mid     = voice_gain(mid_note);
    const float g_glint   = voice_gain(mid_note * 1.5f);
    const float g_answer  = voice_gain(answer_note);
    const float g_ask_bed_1 = voice_gain(pad_root);
    const float g_ask_bed_2 = voice_gain(pad_root * 1.3333f);
    const float g_fault_sweep = voice_gain(420.f * pitch);
    const float g_fault_bed_1 = voice_gain(196.f * pitch);
    const float g_fault_bed_2 = voice_gain(207.f * pitch);
    const float g_fault_tail  = voice_gain(147.f * pitch);
    const float g_idle_sweep  = voice_gain(330.f * pitch);
    const float g_idle_bed    = voice_gain(220.f * pitch);
    const float g_idle_tail   = voice_gain(165.f * pitch);
    const float target_peak = cue == Cue::Running   ? kPeakRunning
                            : cue == Cue::Done      ? kPeakDone
                            : cue == Cue::Attention ? kPeakAttention
                            : cue == Cue::Error     ? kPeakError
                                                    : kPeakIdle;
    float peak = 0.f;

    const int64_t render_started_us = esp_timer_get_time();
    uint32_t noise = 0xB5297A4Du ^ variation;
    // The first/last rail gestures remain tactile and silent. The musical clock
    // begins with the visible colour surface and ends before the rail returns,
    // so perceived duration follows what is actually on screen. The offset is
    // one setting for the whole score, so it is read once rather than per sample.
    const float configured_lag = status_timing::audio_base_lag
        + static_cast<float>(model::state.status_audio_offset_ms) / 1000.f;
    const float step_t = 1.f / static_cast<float>(kStatusRate);
    for (size_t i = 0; i < kStatusLen; ++i) {
        const float t = static_cast<float>(i) * step_t;
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
            if (gesture_t >= 0.f && gesture_t < kChirpLife) {
                const float phase = 6.28318f * pitch
                                  * (300.f * gesture_t + 750.f * gesture_t * gesture_t);
                const float env = fast_sin(std::min(1.f, gesture_t / 0.16f) * 3.14159f)
                                * fast_decay(gesture_t / 0.20f);
                v = (fast_sin(phase) * 0.62f + fast_sin(phase * 2.01f) * 0.18f)
                  * env * g_run_sweep;
            }
            // Track under the hold: an asymmetric motor pulse, quiet but alive.
            if (bed > 0.f) {
                const float pulse = 0.48f + 0.30f * fast_sin(6.28318f * 1.73f * cue_t)
                                  + 0.12f * fast_sin(6.28318f * 4.11f * cue_t + 0.8f);
                v += (fast_sin(6.28318f * 220.f * pitch * cue_t) * 0.065f * g_run_low
                    + fast_sin(6.28318f * 330.f * pitch * cue_t) * 0.035f * g_run_fifth)
                   * std::clamp(pulse, 0.08f, 0.9f) * bed;
            }
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 440.f * pitch * return_t)
                   * fast_decay(return_t / 0.24f) * 0.09f * g_run_tail;
            }
        } else if (cue == Cue::Done) {
            // Three staggered bell partials resolve into one bright consonance.
            if (gesture_t >= 0.f && gesture_t < kBellLife) {
                const float on1 = gesture_t;
                const float on2 = std::max(0.f, gesture_t - 0.035f);
                const float on3 = std::max(0.f, gesture_t - 0.070f);
                v = fast_sin(6.28318f * pitch * 659.f * on1) * fast_decay(on1 / 0.090f) * 0.42f * g_bell_1;
                if (gesture_t >= 0.035f) v += fast_sin(6.28318f * pitch * 831.f * on2) * fast_decay(on2 / 0.085f) * 0.38f * g_bell_2;
                if (gesture_t >= 0.070f) v += fast_sin(6.28318f * pitch * 988.f * on3) * fast_decay(on3 / 0.100f) * 0.34f * g_bell_3;
            }
            // A consonant major bed holds the completed state without another melody.
            if (bed > 0.f) {
                v += (fast_sin(6.28318f * 329.6f * pitch * cue_t) * 0.040f * g_done_bed_1
                    + fast_sin(6.28318f * 415.3f * pitch * cue_t) * 0.032f * g_done_bed_2
                    + fast_sin(6.28318f * 493.9f * pitch * cue_t) * 0.028f * g_done_bed_3) * bed;
            }
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 659.f * pitch * return_t)
                   * fast_decay(return_t / 0.28f) * 0.075f * g_bell_1;
            }
        } else if (cue == Cue::Attention) {
            // Three notes that rise and are left unresolved. This is the
            // completion bell's voice and rhythm -- a fundamental with a light
            // second partial, staggered onsets, one exponential decay each --
            // pointed upward instead of settling onto a consonance, so the ask
            // is plainly a member of the same family as the other four scores
            // rather than a different instrument. It was previously blown
            // resonators over a pulsing chord, and no amount of tuning made
            // that sit beside a chirp, a bell and a release.
            //
            // Which of the three asks sounds -- two rising through a second
            // onto an open fifth, one hanging on a major seventh -- was chosen
            // once for this whole score, before the sample loop.
            if (gesture_t >= 0.f && gesture_t < kAskLife) {
                const float on1 = gesture_t;
                const float on2 = gesture_t - kMidAt;
                const float on3 = gesture_t - kAnswerAt;
                // All three ring together: the first is still sounding when
                // the last answers it, which is what makes the phrase one
                // gesture rather than three events. Each arrives quieter than
                // the one before, so the figure leans forward instead of
                // hammering three times.
                if (on1 < kAskSpan * kCallTau)
                    v = ask_voice(call_root, kAskAttack, on1, kCallTau) * 0.34f * g_call;
                if (on2 >= 0.f && on2 < kAskSpan * kMidTau) {
                    v += ask_voice(mid_note, kAskAttack, on2, kMidTau) * 0.28f * g_mid;
                    // One step of the four is lit, and it is lit at the top of
                    // the phrase rather than at its end: a fifth over the peak
                    // note, quiet and gone almost at once. A series that
                    // reaches something and comes back is worth listening
                    // through -- but only if the glint happens once a cycle.
                    // On every ask it would just be the cue being bright.
                    if (ask_shimmer && on2 < kAskSpan * kShimmerTau)
                        v += ask_voice(mid_note * 1.5f, kAskAttack, on2,
                                       kShimmerTau) * 0.050f * g_glint;
                }
                if (on3 >= 0.f) {
                    // The note the phrase settles on is the softest thing in
                    // it and the longest: it is arrived at rather than struck,
                    // and it is still fading when the bed takes the wait over.
                    v += ask_voice(answer_note, kAnswerAttack, on3,
                                   kAnswerTau) * 0.17f * g_answer;
                }
            }
            // A quiet open bed under the wait, at the same weight every other
            // score gives its hold. Root and fourth, bare: the chord that is
            // standing changes with every ask now, so the movement of the
            // progression is what keeps the bed alive, and a third voice
            // would only thicken what is already the most expensive layer in
            // the score. It is a fourth rather than a fifth because the ask
            // speaks a fifth above the chord -- a bed fifth would sit exactly
            // on the phrase's first note and fight it for the same air. It
            // never gates, pulses or repeats: a cue that keeps restating
            // itself at a fixed rate is what turns a request into nagging.
            if (bed > 0.f) {
                // Every voice is a ratio of one phase, so the phase is built
                // once rather than per voice: this is the only layer that runs
                // for the whole hold, and the score has to finish inside the
                // debounce that runs ahead of the takeover it accompanies.
                const float bed_phase = 6.28318f * pad_root * cue_t;
                v += (fast_sin(bed_phase) * 0.044f * g_ask_bed_1
                    + fast_sin(bed_phase * 1.3333f) * 0.032f * g_ask_bed_2) * bed;
            }
            if (return_t > 0.f) {
                // The tail lifts back to the answering note rather than
                // dropping to the bed: the request is still open when the
                // panel leaves.
                v += fast_sin(6.28318f * answer_note * return_t)
                   * fast_decay(return_t / 0.20f) * 0.075f * g_answer;
            }
        } else if (cue == Cue::Error) {
            // A descending, slightly roughened interval that refuses to resolve.
            noise = noise * 1664525u + 1013904223u;
            const float grit = static_cast<float>(static_cast<int32_t>(noise >> 9) % 1000 - 500) / 500.f;
            if (gesture_t >= 0.f && gesture_t < kFaultLife) {
                const float phase = 6.28318f * pitch
                                  * (620.f * gesture_t - 900.f * gesture_t * gesture_t);
                v = (fast_sin(phase) * 0.52f + grit * 0.10f)
                  * fast_decay(gesture_t / 0.095f) * g_fault_sweep;
            }
            // Two close, non-harmonic voices beat slowly through the hold.
            if (bed > 0.f) {
                v += (fast_sin(6.28318f * 196.f * pitch * cue_t) * 0.050f * g_fault_bed_1
                    + fast_sin(6.28318f * 207.f * pitch * cue_t) * 0.042f * g_fault_bed_2
                    + grit * 0.010f) * bed;
            }
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 147.f * pitch * return_t)
                   * fast_decay(return_t / 0.24f) * 0.08f * g_fault_tail;
            }
        } else { // Idle: a soft downward release, intentionally least prominent.
            if (gesture_t >= 0.f && gesture_t < kReleaseLife) {
                const float phase = 6.28318f * pitch
                                  * (440.f * gesture_t - 240.f * gesture_t * gesture_t);
                v = fast_sin(phase) * fast_decay(gesture_t / 0.055f) * 0.38f * g_idle_sweep;
            }
            if (bed > 0.f)
                v += fast_sin(6.28318f * 220.f * pitch * cue_t) * bed * 0.035f * g_idle_bed;
            if (return_t > 0.f) {
                v += fast_sin(6.28318f * 165.f * pitch * return_t)
                   * fast_decay(return_t / 0.30f) * 0.055f * g_idle_tail;
            }
        }
        if (cue_t < 0.001f) v *= cue_t / 0.001f;
        peak = std::max(peak, std::fabs(v));
        output[i] = static_cast<int16_t>(
            std::clamp(v, -kRawCeiling, kRawCeiling) * kRawScale);
    }
    trim_to_peak(output, kStatusLen, peak, target_peak);
    const int64_t render_us = esp_timer_get_time() - render_started_us;
    // Publishing the step makes the progression observable while tuning on
    // hardware -- a series has to be watched over several asks to be judged at
    // all; every other cue keeps the plain line.
    if (cue == Cue::Attention) {
        std::printf("CCP_AUDIO|status=%u|step=%u|rnd=%08x|peak=%.2f|render_us=%lld|samples=%u|core=%d\n",
                    static_cast<unsigned>(cue), static_cast<unsigned>(ask_step),
                    static_cast<unsigned>(variation), static_cast<double>(peak),
                    static_cast<long long>(render_us),
                    static_cast<unsigned>(kStatusLen), xPortGetCoreID());
    } else {
        std::printf("CCP_AUDIO|status=%u|peak=%.2f|render_us=%lld|samples=%u|core=%d\n",
                    static_cast<unsigned>(cue), static_cast<double>(peak),
                    static_cast<long long>(render_us),
                    static_cast<unsigned>(kStatusLen), xPortGetCoreID());
    }
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
    float note_gain[8] = {};
    for (int note = 0; note < composition.note_count; ++note) {
        note_hz[note] = 523.251f * std::pow(2.f,
            static_cast<float>(composition.notes[note].semitone) / 12.f) * pitch;
        note_gain[note] = voice_gain(note_hz[note]);
    }
    // The pad is written two octaves under the melody, which is precisely
    // where this speaker stops answering: without the correction the chord was
    // carried almost entirely by its own upper voices.
    const float g_pad_root  = voice_gain(chord_root * pitch);
    const float g_pad_third = voice_gain(chord_third * pitch);
    const float g_pad_fifth = voice_gain(chord_fifth * pitch);
    const float g_pad_octave = voice_gain(chord_root * 2.f * pitch);
    float peak = 0.f;

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
            pad = (fast_sin(6.28318f * chord_root * pitch * local) * 0.055f * g_pad_root
                 + fast_sin(6.28318f * chord_third * pitch * local) * 0.090f * g_pad_third
                 + fast_sin(6.28318f * chord_fifth * pitch * local) * 0.072f * g_pad_fifth
                 + fast_sin(6.28318f * chord_root * 2.f * pitch * local) * 0.055f * g_pad_octave
                 + fast_sin(6.28318f * chord_third * 1.004f * pitch * local) * 0.026f * g_pad_third)
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
                  * env * 0.23f * note_gain[note];
        }

        peak = std::max(peak, std::fabs(pad + lead));
        status_pcm[0][i] = static_cast<int16_t>(
            std::clamp(pad + lead, -kRawCeiling, kRawCeiling) * kRawScale);
    }
    trim_to_peak(status_pcm[0], kBootLen, peak, kPeakChime);
    std::printf("CCP_CHIME_READY|variant=%u|peak=%.2f|render_us=%lld\n",
                static_cast<unsigned>(model::state.startup_chime),
                static_cast<double>(peak),
                static_cast<long long>(esp_timer_get_time() - render_started_us));
    // Reserve buffer 0 before DMA starts. Codex often reconnects while the boot
    // score is still playing; without this marker the status worker also chose
    // buffer 0 and replaced CLOUD in flight, reducing it to a DUO-like fragment.
    playing_buffer.store(0, std::memory_order_release);
    M5.Speaker.playRaw(status_pcm[0], kBootLen, kCueRate, false, 1,
                       kStatusChannel, true);
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
    const BaseType_t task_result = xTaskCreatePinnedToCore(synth_task, "status_synth", 4096, nullptr, 1, &synth_task_handle, 1);
    if (task_result != pdPASS) { synth_task_handle = nullptr; std::printf("CCP_AUDIO|worker_start_failed|result=%ld\n", static_cast<long>(task_result)); }
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

bool play_prepared_status()
{
    const int target = armed_buffer.load(std::memory_order_acquire);
    if (target < 0) return true;
    if (model::state.sound_volume == 0) {
        armed_buffer.store(-1, std::memory_order_release);
        return true;
    }
    ensure_speaker();
    if (!speaker_live) {
        std::printf("CCP_AUDIO_PLAY|retry=speaker_unavailable\n");
        return false;
    }
    if (!M5.Speaker.playRaw(status_pcm[target], kStatusLen, kStatusRate,
                            false, 1, kStatusChannel, true)) {
        std::printf("CCP_AUDIO_PLAY|retry=channel_busy|buffer=%d\n", target);
        return false;
    }
    armed_buffer.store(-1, std::memory_order_release);
    playing_buffer.store(static_cast<int8_t>(target), std::memory_order_release);
    std::printf("CCP_AUDIO_PLAY|buffer=%d|channel=%d|core=%d\n",
                target, kStatusChannel, xPortGetCoreID());
    return true;
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
