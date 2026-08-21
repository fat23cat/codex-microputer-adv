// Host lamp wire colours. These are Codex Micro protocol values carried in
// thstatus frames and echoed by the diagnostic TASK stream. They live apart
// from theme.h because they are never drawn directly: status_reducer
// classifies incoming frames byte-for-byte against them, so the values must
// not change. They are RGB888 wire format, unlike the RGB565 canvas palette.
#pragma once

#include <cstdint>

namespace lamp {

constexpr uint32_t kRunning = 0x304ffe;    // host blue: working
constexpr uint32_t kNeedsInput = 0xff6d00; // host orange: action required
constexpr uint32_t kDoneSeen = 0xffffff;   // host white: completed, viewed
constexpr uint32_t kDoneUnseen = 0x00ff4c; // host green: completed, unread
constexpr uint32_t kError = 0xff0033;      // host red: fault

}  // namespace lamp
