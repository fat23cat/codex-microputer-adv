// Presentation layer: screen stack, animation clock, rendering.
//
// main.cpp never touches the canvas directly. It mutates model::state, calls
// ui::notify_* when something happens, and calls ui::service() once per loop.
#pragma once

#include <cstdint>

namespace ui {

enum class Screen : uint8_t { Boot, Deck, Settings, DebugSettings, StatusDebug, ChimeLab, Help };

enum class SettingsRow : uint8_t { BleProfile, Volume, StartupSound, Exit, Count };
enum class DebugSettingsRow : uint8_t {
    UsbHid, PreviewSplash, PreviewPairing, PreviewControl, ChimeLab, StatusDebug, Count
};
enum class DeveloperPreview : uint8_t { None, Splash, Pairing, Control };

void init();

// Advances springs and repaints when anything is still moving or dirty.
// Call every loop iteration; it rate-limits itself to the frame budget.
void service();

// Force a repaint on the next service() even if nothing is animating.
void invalidate();

// Renders a stable representative frame into the real display canvas and
// returns its RGB565 pixels. Demo state is restored before this returns.
// `live` returns the current framebuffer without changing state. Stable
// representative scenes are splash, pairing, deck, recording, composer,
// settings and debug. The pointer remains valid until the next UI render.
const uint16_t* capture_frame(const char* scene);

Screen screen();
void   go(Screen target);

// Selection changes route through here so the highlight animates and the list
// scrolls to keep the selection in view.
void select(int index, bool animate = true);

// Re-derive scroll/highlight targets after the host replaces the task list.
void relayout();

// A slot was just pressed. Drives the cell's own press feedback, which has to be
// separate from select() because pressing the key you are already on still has
// to answer.
void notify_press(int slot);

// Native Codex voice capture owns the panel while G0 is held. The slot is
// retained so the takeover visibly belongs to the chat receiving the audio.
void set_voice_active(bool active, int slot);

// Native Codex composer control preview. The dial can navigate the composer,
// adjust reasoning, scroll, or run custom actions; this overlay deliberately
// does not guess which mode the host currently owns.
void set_composer_control_active(bool active);
void set_pairing_pin(bool active, uint32_t passkey);
bool composer_control_active();
void dismiss_composer_control_preview();
void notify_composer_control_step(int direction);
void notify_composer_control_select();
// Mirror the host's own six-lamp preview frame onto the control page. Colours
// are rgb888, levels 0..1, six of each.
void note_composer_control_lamps(const uint32_t* rgb, const float* level);
// Codex blanks or whites out all six lamps when it closes a control surface.
// That frame, not a timer, is what takes the page down.
void note_composer_control_closed();
void note_composer_control_preview();
void cancel_status_announcements();

// Local settings screen: move between rows and toggle the focused local value.
void settings_move(int delta);
SettingsRow settings_focus();
void debug_settings_move(int delta);
DebugSettingsRow debug_settings_focus();
void show_developer_preview(DeveloperPreview preview);
void close_developer_preview();
bool developer_preview_active();
void debug_move(int delta);
bool debug_adjust(int delta);
bool debug_run();
void chime_move(int dx, int dy);
uint8_t chime_focus();

// Flap a host-supplied setting chip on legacy diagnostic screens.
void flash_setting(int which);

// Transient banner across the top rail. `accent` tints the bar.
void toast(const char* title, const char* detail, uint16_t accent);

// Display power management. Returns true if the call only woke the panel and
// the triggering input should therefore not also act.
bool wake();
void service_power();
bool asleep();

}  // namespace ui
