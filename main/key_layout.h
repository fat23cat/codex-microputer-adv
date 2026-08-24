#pragma once

#include "keys.h"

namespace keys {

// Pure physical-layout mapping shared by both keyboard backends and host tests.
// Keep punctuation explicit: row-1 brackets are the encoder detents, while the
// row-3 slash carries the printed right-arrow and remains the analog stick.
// Row-1 backslash is unbound. It was the encoder click, but Enter is the click
// now -- one confirm key, not two -- and the control page says so.
inline Press map_layout(int row, int col)
{
    switch (row) {
        case 0:   // ` 1 2 3 4 5 6 7 8 9 0 - = del
            if (col == 0)  return {Key::Back, 0};
            if (col <= 10) return {Key::Digit, col % 10};
            if (col == 11) return {Key::Mute, 0};
            if (col == 12) return {Key::Help, 0};
            if (col == 13) return {Key::Interrupt, 0};
            break;
        case 1:   // tab q w e r t y u i o p [ ] backslash
            if (col == 0)  return {Key::Settings, 0};
            if (col >= 5 && col <= 10) return {Key::NativeAction, col + 1};
            if (col == 11) return {Key::EncoderLeft, 0};
            if (col == 12) return {Key::EncoderRight, 0};
            break;
        case 2:   // fn shift a s d f g h j k l ; ' enter
            if (col == 2)  return {Key::NativeAction, 1011};
            if (col == 11) return {Key::Up, 0};
            if (col == 13) return {Key::Enter, 0};
            break;
        case 3:   // ctrl opt alt z x c v b n m , . / space
            if (col == 10) return {Key::Left, 0};
            if (col == 11) return {Key::Down, 0};
            if (col == 12) return {Key::Right, 0};
            if (col == 13) return {Key::Enter, 0};
            break;
        default: break;
    }
    return {Key::Other, 0};
}

inline bool needs_release(Key key)
{
    // Enter carries the encoder click, and that release has to stay physical
    // so Codex can still time a long press on it.
    return key == Key::Digit || key == Key::NativeAction
        || key == Key::Enter;
}

}  // namespace keys
