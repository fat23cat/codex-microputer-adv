// Speaker cues. Voice input is owned by Codex on the host; Cardputer only
// forwards the native microphone button state.
#pragma once

#include <cstddef>
#include <cstdint>

namespace audio {

enum class Cue : uint8_t {
    Done,      // a task finished
    Running,   // a task started working
    Attention, // a task wants input or approval
    Error,
    Idle,      // a task returned to rest
    Select,    // selection moved
    MenuOpen,  // native encoder surface opened
    MenuApply, // native encoder choice confirmed
    StepLeft,  // encoder detent left / shallower
    StepRight, // encoder detent right / deeper
    Boot,      // powered up
    Unmute,    // sound switched back on; plays even while muted
};

void init();
void apply_volume();

constexpr uint8_t kStartupChimeCount = 10;
const char* startup_chime_name(uint8_t index);

// Prepare a status score on the second core while the configured debounce runs.
// `token` identifies one queued event, so a replacement cannot reuse stale PCM.
void request_status(Cue cue, uint32_t token);
bool status_ready(Cue cue, uint32_t token);
bool arm_status(Cue cue, uint32_t token);
// Returns false when the speaker could not accept the prepared buffer yet; the
// caller must retry without advancing the audio edge.
bool play_prepared_status();

// No-op when the user has muted the device.
void play(Cue cue);

}  // namespace audio
