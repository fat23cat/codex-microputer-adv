// Design tokens.
//
// Semantic colours stay few and literal: blue is working, vermilion needs the
// user, green is newly completed, and grey is completed work already viewed.
//
// The deck follows Codex Micro literally: six tall agent keys in one row. The
// Cardputer panel has almost exactly the same aspect ratio as the reference,
// so splitting it into two rows only wastes the form factor.
#pragma once

#include <cstdint>

namespace theme {

// RGB565 in the byte order M5Canvas expects.
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ------------------------------------------------------------------ palette
constexpr uint16_t kPaper = rgb(244, 242, 236);   // bone
constexpr uint16_t kInk   = rgb(23,  21,  15);
constexpr uint16_t kBlue  = rgb(27,  79,  208);   // working
constexpr uint16_t kBlueLift = rgb(42, 118, 205); // cool blue pulse with only a slight cyan lift
constexpr uint16_t kVerm  = rgb(226, 69,  30);    // wants you
constexpr uint16_t kVoice = rgb(211, 63,  27);    // warm red-orange: live microphone
constexpr uint16_t kGreen = rgb(38,  198, 58);    // saturated leaf: completed, unread
constexpr uint16_t kPale  = rgb(222, 219, 209);   // idle fill, unbound outline
constexpr uint16_t kGrey  = rgb(154, 150, 140);   // secondary type
constexpr uint16_t kIdleDigit = rgb(142, 138, 128); // quiet inactive numeral
constexpr uint16_t kIdleDigitSelected = rgb(78, 76, 71); // selected idle, never black
constexpr uint16_t kViewed = rgb(228, 229, 225);  // near-paper neutral: completed, viewed
constexpr uint16_t kDim   = rgb(142, 138, 128);   // chrome type
// A lighter tint is the first thing to disappear at 10 % brightness, and the
// layout depends on this rule being visible.
constexpr uint16_t kRule  = rgb(201, 198, 187);

// Semantic aliases.
constexpr uint16_t kRun     = kBlue;
constexpr uint16_t kInput   = kVerm;
constexpr uint16_t kError   = kVerm;
constexpr uint16_t kDone    = kGreen;
constexpr uint16_t kIdle    = kGrey;
constexpr uint16_t kAccent  = kVerm;
constexpr uint16_t kInkSoft = kGrey;
constexpr uint16_t kMuted   = kPale;
constexpr uint16_t kOrange  = kVerm;

// --------------------------------------------------------------- deck layout
constexpr int kScreenW = 240;
constexpr int kScreenH = 135;

constexpr int kMargin = 6;

constexpr int kCols      = 6;
constexpr int kRows      = 1;
constexpr int kCellCount = kCols * kRows;
constexpr int kCellW     = 39;  // first five; the last absorbs the spare pixel
constexpr int kCellH     = 135;
constexpr int kCellGap   = 1;
constexpr int kCellPitchX = 40;
constexpr int kCellPitchY = kCellH + kCellGap;
constexpr int kGridW = kScreenW;
constexpr int kGridH = kCellH;
constexpr int kGridX = 0;
constexpr int kGridY = 0;

constexpr int kRuleY   = 112;
constexpr int kChromeY = 120;    // Font0, 6x8, +1 px tracking

// Kept for the screens that still draw a full-width answer line.
constexpr int kAnswerY = 60;

// ------------------------------------------------- legacy screens (voice etc.)
// These sit below the cell row so the screens that still reason in the old
// terms keep working without overlapping it.
constexpr int kGutter     = kMargin;
constexpr int kBandY      = 46;
constexpr int kBandH      = 20;
constexpr int kTitleY     = 72;
constexpr int kLineH      = 19;
constexpr int kTitleW     = kScreenW - 2 * kMargin;
constexpr int kTitleLines = 2;
constexpr int kRailY      = 116;
constexpr int kMeterW     = 24;
constexpr int kMeterH     = 3;
constexpr int kTopRailH   = 24;
constexpr int kBotRailH   = kScreenH - kRailY;
constexpr int kListTop    = 28;
constexpr int kListBottom = kRailY - 4;

// Blend two RGB565 colors, `t` in 0..255. With flat fills either side the
// result is exact rather than approximate.
inline uint16_t mix(uint16_t a, uint16_t b, uint8_t t)
{
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r  = ar + ((br - ar) * t >> 8);
    const int g  = ag + ((bg - ag) * t >> 8);
    const int bl = ab + ((bb - ab) * t >> 8);
    return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

}  // namespace theme
