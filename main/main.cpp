// Codex Microputer ADV firmware.
//
// The device is a native Codex Micro HID controller. USB serial remains a
// diagnostic path for synthetic UI and hardware tests.

#include <M5Unified.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio.h"
#include "keys.h"
#include "ble_transport.h"
#include "usb_transport.h"
#include "codex_micro_protocol.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "link.h"
#include "model.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "store.h"
#include "theme.h"
#include "ui.h"

namespace model { State state; }

namespace {

constexpr char kFirmwareVersion[] = "0.4.1";

// Task list staged by TASK lines and swapped in atomically on TASKS.
model::Task staged[model::kMaxTasks];
int         staged_count = 0;

// Two-step confirmation for handing the device back to M5Apps.
bool exit_armed = false;

// True only after G0 successfully started Codex Micro push-to-talk. A wake-only
// press never arms it, so releasing the button cannot trigger voice by accident.
bool voice_button_active = false;

// Keyboard arrows emulate a short planar-stick deflection. A separate timed
// centre report is essential: without it the host would keep the virtual stick
// held after a key that has no continuous analogue position.
uint32_t joystick_release_ms = 0;
bool joystick_deflected = false;

using keys::Key;
using keys::Press;

// The bundled efont covers Latin, Cyrillic and CJK but not the typographic
// punctuation Codex titles are full of, so those code points render as blanks.
// Fold them onto ASCII on the way in rather than shipping a second font.
void sanitize_utf8(const char* in, char* out, int out_size)
{
    struct Fold { const char* from; const char* to; };
    static const Fold kFolds[] = {
        {"\xE2\x80\x94", "-"},   {"\xE2\x80\x93", "-"},    // em / en dash
        {"\xE2\x80\x95", "-"},   {"\xE2\x88\x92", "-"},    // horizontal bar, minus
        {"\xE2\x80\xA6", "..."},                            // ellipsis
        {"\xE2\x80\x9C", "\""},  {"\xE2\x80\x9D", "\""},   // curly double quotes
        {"\xE2\x80\x98", "'"},   {"\xE2\x80\x99", "'"},    // curly single quotes
        {"\xC2\xAB", "\""},       {"\xC2\xBB", "\""},        // guillemets
        {"\xE2\x86\x92", "->"},  {"\xE2\x86\x90", "<-"},   // arrows
        {"\xE2\x80\xA2", "*"},   {"\xC2\xB7", "*"},         // bullets
        {"\xC2\xA0", " "},        {"\xE2\x80\xAF", " "},     // no-break spaces
    };

    int written = 0;
    for (const char* p = *in ? in : ""; *p && written < out_size - 1;) {
        const Fold* hit = nullptr;
        for (const Fold& fold : kFolds) {
            const size_t length = std::strlen(fold.from);
            if (std::strncmp(p, fold.from, length) == 0) { hit = &fold; break; }
        }
        if (hit) {
            const int length = static_cast<int>(std::strlen(hit->to));
            if (written + length >= out_size - 1) break;
            std::memcpy(out + written, hit->to, length);
            written += length;
            p += std::strlen(hit->from);
            continue;
        }
        out[written++] = *p++;
    }
    out[written] = 0;
}

// ================================================================== protocol
model::Status parse_status(const char* text)
{
    if (std::strstr(text, "RUN"))    return model::Status::Running;
    if (std::strstr(text, "INPUT"))  return model::Status::NeedsInput;
    if (std::strstr(text, "APPROV")) return model::Status::NeedsInput;
    if (std::strstr(text, "DONE"))   return model::Status::Done;
    if (std::strstr(text, "COMPLETE")) return model::Status::Done;
    if (std::strstr(text, "ERROR"))  return model::Status::Error;
    return model::Status::Idle;
}

// Two different statements, deliberately kept apart:
//   CCP_CFG    - the user changed something here; the host should apply it.
//   CCP_CFGACK - we adopted what the host sent; informational only.
// Collapsing them into one line makes the host act on its own echo, which
// rewrites the user's Codex config on every reconnect.
void publish_settings(bool user_initiated)
{
    const auto& s = model::state;
    hostlink::sendf("%s|%s|%s|%s|%s\n", user_initiated ? "CCP_CFG" : "CCP_CFGACK",
                s.models.current_wire(), s.efforts.current_wire(),
                s.speeds.current_wire(), s.sound_volume > 0 ? "on" : "off");
}

// ------------------------------------------------------------ encoder gestures
// The Codex app listens for the Micro's dial on the same v.oai.hid channel as
// the keys: rotation arrives as ENC_CW / ENC_CC with act 2, and the dial itself
// as ENC with act 1 then 0. Meaning belongs entirely to Codex's current dial
// mode and focused composer control; the firmware must never reinterpret a
// detent as reasoning effort.
bool send_encoder_step(bool right)
{
    return companion_codex_key(right ? "ENC_CC" : "ENC_CW", 2);
}

// Preserve the physical down edge. keys.cpp emits the matching release, so
// Codex can distinguish a click from the long press that opens device settings.
bool send_encoder_press()
{
    // Codex opens a temporary lighting preview for the model picker. Its first
    // breath slot is not a chat selection, so guard before the host can reply.
    codex_micro::suppress_host_selection(2000);
    return companion_codex_key("ENC", 1);
}

bool send_native_action(int slot, bool down, int agent = -1)
{
    if ((slot < 6 || slot > 12) && slot != 1011) return false;
    char key[12];
    if (slot == 1011) std::snprintf(key, sizeof(key), "ACT10_ACT11");
    else std::snprintf(key, sizeof(key), "ACT%02d", slot);
    return companion_codex_key(key, down ? 1 : 0, agent);
}

bool send_agent_key(int slot, bool down)
{
    if (slot < 0 || slot >= model::kMaxTasks) return false;
    char key[5];
    std::snprintf(key, sizeof(key), "AG%02d", slot);
    return companion_codex_key(key, down ? 1 : 0, slot);
}

void toggle_sound()
{
    auto& s = model::state;
    if (s.sound_volume > 0) {
        audio::play(audio::Cue::Select);
        s.unmuted_volume = s.sound_volume;
        s.sound_volume = 0;
    } else {
        s.sound_volume = std::max<uint8_t>(10, s.unmuted_volume);
        audio::apply_volume();
        audio::play(audio::Cue::Unmute);
    }
    store::save_settings();
    publish_settings(true);
    ui::toast(s.sound_volume > 0 ? "SOUND ON" : "MUTED", "", theme::kInk);
    ui::invalidate();
}

void adjust_volume(int delta)
{
    auto& s = model::state;
    const uint8_t previous = s.sound_volume;
    const int next = std::clamp(static_cast<int>(s.sound_volume) + delta * 10, 0, 100);
    s.sound_volume = static_cast<uint8_t>(next);
    if (next > 0) s.unmuted_volume = s.sound_volume;
    audio::apply_volume();
    // Audition the value itself, not the old setting: the same short neutral
    // sound at every step makes adjacent levels directly comparable.
    if (s.sound_volume != previous) audio::play(audio::Cue::Select);
    store::save_settings();
    publish_settings(true);
    char value[12];
    std::snprintf(value, sizeof(value), "%d%%", next);
    ui::toast("VOLUME", value, theme::kInk);
    ui::invalidate();
}

void toggle_startup_sound()
{
    auto& s = model::state;
    s.startup_sound_on = !s.startup_sound_on;
    store::save_settings();
    ui::toast(s.startup_sound_on ? "STARTUP CHIME ON" : "STARTUP CHIME OFF",
              "", theme::kInk);
    audio::play(audio::Cue::Select);
    ui::invalidate();
}

bool send_joystick_impulse(Key key)
{
    // Codex consumes normalized screen-space polar coordinates: zero points
    // right and angles advance clockwise because positive Y points down.
    float angle = 0.f;
    switch (key) {
        case Key::Right: angle = 0.00f; break;
        case Key::Down:  angle = 0.25f; break;
        case Key::Left:  angle = 0.50f; break;
        case Key::Up:    angle = 0.75f; break;
        default: return false;
    }
    if (!companion_codex_joystick(angle, 1.f)) return false;
    joystick_deflected = true;
    joystick_release_ms = lgfx::millis() + 85;
    return true;
}

// Commit the staged list, then compare against what was on screen so newly
// finished work can announce itself.
void commit_tasks(int selected_hint)
{
    auto& s = model::state;

    model::Task previous[model::kMaxTasks];
    const int previous_count = s.task_count;
    std::memcpy(previous, s.tasks, sizeof(previous));

    int newly_done = 0;
    int newly_input = 0;
    const char* headline = nullptr;
    for (int i = 0; i < staged_count; ++i) {
        // Carry the unseen flag across refreshes, keyed by thread id rather than
        // slot, because the host reorders by recency.
        const model::Task* before = nullptr;
        for (int j = 0; j < previous_count; ++j) {
            if (std::strcmp(previous[j].id, staged[i].id) == 0) { before = &previous[j]; break; }
        }
        if (before) {
            staged[i].unseen_done = before->unseen_done;
            staged[i].completion_hold = before->completion_hold;
            if (before->status != model::Status::Done && staged[i].status == model::Status::Done) {
                staged[i].unseen_done = true;
                staged[i].completion_hold = true;
                ++newly_done;
                if (!headline) headline = staged[i].title;
            }
            if (staged[i].status != model::Status::Done) {
                staged[i].unseen_done = false;
                staged[i].completion_hold = false;
            }
            if (before->status != model::Status::NeedsInput && staged[i].status == model::Status::NeedsInput) {
                ++newly_input;
                if (!headline) headline = staged[i].title;
            }
            if (before->status != staged[i].status && i < theme::kCellCount) {
                model::queue_announcement(i, staged[i].status, before->status,
                                          staged[i].unseen_done,
                                          staged[i].unseen_done,
                                          before->unseen_done,
                                          lgfx::millis());
            }
        } else if (staged[i].status == model::Status::Done && previous_count > 0) {
            // A thread that arrives already-done is new to us but not news.
            staged[i].unseen_done = false;
        }
    }

    std::memcpy(s.tasks, staged, sizeof(s.tasks));
    s.task_count = staged_count;
    if (selected_hint >= 0 && selected_hint < staged_count) s.selected = selected_hint;
    ui::select(s.selected, true);
    ui::relayout();

    if (newly_input > 0) {
        ui::toast("NEEDS INPUT", headline ? headline : "", theme::kInput);
    } else if (newly_done > 0) {
        char title[24];
        if (newly_done == 1) std::snprintf(title, sizeof(title), "TASK DONE");
        else std::snprintf(title, sizeof(title), "%d TASKS DONE", newly_done);
        ui::toast(title, headline ? headline : "", theme::kDone);
    }
    ui::invalidate();
}

void enable_m5apps_autostart()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition("apps_nvs", "system", NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, "last_app", 1);
    if (err == ESP_OK) err = nvs_set_i32(handle, "last_app_to", 2);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    hostlink::emit("m5apps_autostart", err == ESP_OK, esp_err_to_name(err));
}

void return_to_m5apps()
{
    const esp_partition_t* factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (!factory) { hostlink::emit("return_to_m5apps", false, "factory_not_found"); return; }
    const esp_err_t err = esp_ota_set_boot_partition(factory);
    hostlink::emit("return_to_m5apps", err == ESP_OK, esp_err_to_name(err));
    if (err == ESP_OK) { vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
}

void send_screenshot(const char* scene)
{
    const uint16_t* pixels = ui::capture_frame(scene);
    if (!pixels) {
        char error[96];
        const int length = std::snprintf(error, sizeof(error),
            "CCP_SHOT|ERROR|unknown_scene|%s\n", scene ? scene : "");
        hostlink::send_usb(error, static_cast<size_t>(std::max(0, length)));
        return;
    }

    constexpr int kPixels = theme::kScreenW * theme::kScreenH;
    constexpr int kChunkPixels = 120;
    char line[32 + kChunkPixels * 4];
    hostlink::begin_usb_bulk();
    int length = std::snprintf(line, sizeof(line),
        "CCP_SHOT|BEGIN|%s|%d|%d|RGB565|%d\n",
        scene, theme::kScreenW, theme::kScreenH, kPixels);
    hostlink::send_usb(line, static_cast<size_t>(length));

    uint32_t checksum = 2166136261u;
    int sequence = 0;
    for (int offset = 0; offset < kPixels; offset += kChunkPixels, ++sequence) {
        const int count = std::min(kChunkPixels, kPixels - offset);
        int used = std::snprintf(line, sizeof(line), "CCP_SHOT|DATA|%d|", sequence);
        for (int i = 0; i < count; ++i) {
            const uint16_t value = pixels[offset + i];
            checksum = (checksum ^ static_cast<uint8_t>(value >> 8)) * 16777619u;
            checksum = (checksum ^ static_cast<uint8_t>(value)) * 16777619u;
            used += std::snprintf(line + used, sizeof(line) - used, "%04X", value);
        }
        line[used++] = '\n';
        hostlink::send_usb(line, static_cast<size_t>(used));
        if ((sequence & 7) == 7) vTaskDelay(pdMS_TO_TICKS(1));
    }
    length = std::snprintf(line, sizeof(line), "CCP_SHOT|END|%08lX\n",
                           static_cast<unsigned long>(checksum));
    hostlink::send_usb(line, static_cast<size_t>(length));
    hostlink::end_usb_bulk();
}

// ============================================================== input actions
void handle_press(const Press& press)
{
    auto& s = model::state;
    if (press.key == Key::None) return;
    // Developer previews are inert captures of real rendering paths. Escape is
    // their only control, so testing a screen cannot accidentally operate Codex.
    if (ui::developer_preview_active()) {
        if (press.down && press.key == Key::Back) ui::close_developer_preview();
        return;
    }
    // Host actions stay disabled without Codex, but local settings remain
    // reachable from the splash. Tab returns to the splash, never to a fake
    // task deck; Opt+Tab keeps the same rule for diagnostics.
    if (s.link == model::Link::Offline) {
        if (press.key == Key::Settings) {
            ui::go(ui::screen() == ui::Screen::Settings
                       ? ui::Screen::Boot : ui::Screen::Settings);
            return;
        }
        if (press.key == Key::DebugSettings) {
            ui::go(ui::screen() == ui::Screen::DebugSettings
                       ? ui::Screen::Boot : ui::Screen::DebugSettings);
            return;
        }
        const auto screen = ui::screen();
        if (screen != ui::Screen::Settings
            && screen != ui::Screen::DebugSettings
            && screen != ui::Screen::StatusDebug
            && screen != ui::Screen::ChimeLab) return;
    }


    // Native controls preserve their physical edge. Codex uses these releases
    // to distinguish an Agent Key tap from a double tap and an encoder click
    // from the long press that opens its own device configuration.
    if (!press.down) {
        if (press.key == Key::Digit) {
            const int slot = (press.digit == 0 ? 10 : press.digit) - 1;
            send_agent_key(slot, false);
        } else if (press.key == Key::NativeAction) {
            send_native_action(press.digit, false, s.selected);
        } else if (press.key == Key::EncoderPress) {
            companion_codex_key("ENC", 0);
        }
        return;
    }

    // Modal UI owns physical arrow navigation before global hardware
    // assignments. Brackets remain exclusively encoder detents everywhere.
    if (ui::screen() == ui::Screen::DebugSettings) {
        if (press.key == Key::Up) {
            ui::debug_settings_move(-1);
            hostlink::sendf("CCP_MENU|debug|move=-1|focus=%d\n",
                            static_cast<int>(ui::debug_settings_focus()));
            audio::play(audio::Cue::Select);
            return;
        }
        if (press.key == Key::Down) {
            ui::debug_settings_move(1);
            hostlink::sendf("CCP_MENU|debug|move=1|focus=%d\n",
                            static_cast<int>(ui::debug_settings_focus()));
            audio::play(audio::Cue::Select);
            return;
        }
        if (press.key == Key::Enter) {
            const ui::DebugSettingsRow row = ui::debug_settings_focus();
            if (row == ui::DebugSettingsRow::UsbHid) {
                const bool requested = !s.usb_hid_enabled;
                s.usb_hid_enabled = requested;
                // Persist before removing USB so an unexpected cable event or
                // reset cannot resurrect a mode the user explicitly disabled.
                store::flush();
                if (!companion_usb_set_enabled(requested)) {
                    s.usb_hid_enabled = !requested;
                    store::flush();
                    ui::toast("USB HID ERROR", "State unchanged", theme::kError);
                } else {
                    ui::toast(requested ? "USB HID ON" : "BLUETOOTH ONLY",
                              requested ? "Codex Micro enabled" : "USB data disabled",
                              requested ? theme::kRun : theme::kInput);
                    audio::play(audio::Cue::Select);
                }
                ui::invalidate();
                return;
            }
            if (row == ui::DebugSettingsRow::PreviewSplash) {
                ui::show_developer_preview(ui::DeveloperPreview::Splash);
            } else if (row == ui::DebugSettingsRow::PreviewPairing) {
                ui::show_developer_preview(ui::DeveloperPreview::Pairing);
            } else if (row == ui::DebugSettingsRow::PreviewControl) {
                ui::show_developer_preview(ui::DeveloperPreview::Control);
            } else {
                ui::go(row == ui::DebugSettingsRow::ChimeLab
                           ? ui::Screen::ChimeLab : ui::Screen::StatusDebug);
            }
            return;
        }
    }

    switch (press.key) {
        case Key::Mute:
            toggle_sound();
            break;

        case Key::Record:
            // Keyboard-generated Record has no release edge. Treat it as a
            // short native microphone gesture; the physical G0 button below
            // supplies true press/release push-to-talk.
            companion_codex_key("ACT10", 1, s.selected);
            companion_codex_key("ACT10", 0, s.selected);
            break;

        // These keys are the physical substitute for Codex Micro's dial. In
        // Composer navigation they move focus/options; in Reasoning only they
        // adjust effort; custom mappings may do something else entirely.
        case Key::EncoderLeft:
        case Key::EncoderRight: {
            const bool right = press.key == Key::EncoderRight;
            if (ui::composer_control_active()) codex_micro::suppress_host_selection(2000);
            if (!send_encoder_step(right)) {
                ui::toast("DIAL", "no host", theme::kOrange);
                break;
            }
            audio::play(right ? audio::Cue::StepRight : audio::Cue::StepLeft);
            if (ui::composer_control_active())
                ui::notify_composer_control_step(right ? 1 : -1);
            break;
        }

        case Key::EncoderPress: {
            // A click opens or selects whatever Codex currently focuses. Wait
            // for a host light preview before presenting a local control UI.
            const bool had_preview = ui::composer_control_active();
            if (had_preview) ui::dismiss_composer_control_preview();
            else ui::allow_composer_control_preview();
            if (!send_encoder_press()) {
                ui::toast("DIAL", "no host", theme::kOrange);
                break;
            }
            if (had_preview) {
                audio::play(audio::Cue::MenuApply);
            }
            break;
        }

        case Key::NativeAction: {
            // T..P are the six physical command slots ACT06..ACT11. Codex owns
            // their current command/Skill mapping exactly as it does for Micro.
            if (!send_native_action(press.digit, true, s.selected)) {
                ui::toast("NO HOST", "action not sent", theme::kOrange);
                break;
            }
            if (press.digit == 7) ui::toast("APPROVE", "sent to Codex", theme::kDone);
            else if (press.digit == 8) ui::toast("REJECT", "sent to Codex", theme::kInput);
            audio::play(audio::Cue::Select);
            break;
        }

        case Key::Other:
            break;   // no action of its own; it already woke the panel

        case Key::Settings:
            ui::go(ui::screen() == ui::Screen::Settings ? ui::Screen::Deck : ui::Screen::Settings);
            break;

        case Key::DebugSettings:
            ui::go(ui::screen() == ui::Screen::DebugSettings
                       ? ui::Screen::Deck : ui::Screen::DebugSettings);
            break;

        case Key::Help:
            ui::go(ui::screen() == ui::Screen::Help ? ui::Screen::Deck : ui::Screen::Help);
            break;

        case Key::Back:
            // Back never leaves the app. Handing the device back to M5Apps is a
            // deliberate action from Settings, because it costs a reboot and
            // makes the companion unreachable until the user returns to it.
            ui::go((ui::screen() == ui::Screen::StatusDebug
                    || ui::screen() == ui::Screen::ChimeLab)
                       ? ui::Screen::DebugSettings : ui::Screen::Deck);
            break;

        default: break;
    }

    switch (ui::screen()) {
        case ui::Screen::Settings:
            if (press.key == Key::Up)    ui::settings_move(-1);
            if (press.key == Key::Down)  ui::settings_move(1);
            if (ui::settings_focus() == ui::SettingsRow::BleProfile
                && (press.key == Key::Left || press.key == Key::Right)) {
                auto& profile = model::state.ble_profile;
                profile = static_cast<uint8_t>((profile
                    + (press.key == Key::Right ? 1 : 2)) % 3);
                store::flush();
                const bool switching = companion_ble_select_profile(profile);
                ui::toast(switching ? "HOST CHANNEL" : "BLE SWITCH FAILED",
                          switching ? "Connecting" : "Try again", theme::kInput);
            }
            if (ui::settings_focus() == ui::SettingsRow::Volume
                && (press.key == Key::Left || press.key == Key::Right))
                adjust_volume(press.key == Key::Right ? 1 : -1);
            if (press.key == Key::Enter) {
                if (ui::settings_focus() == ui::SettingsRow::Exit) {
                    if (exit_armed) { return_to_m5apps(); }
                    else { exit_armed = true; ui::toast("EXIT TO M5APPS?", "Press enter again", theme::kInput); }
                } else if (ui::settings_focus() == ui::SettingsRow::Volume) {
                    adjust_volume(1);
                } else if (ui::settings_focus() == ui::SettingsRow::StartupSound) {
                    toggle_startup_sound();
                }
            }
            if (press.key != Key::Enter) exit_armed = false;
            break;

        case ui::Screen::DebugSettings:
            break;

        case ui::Screen::StatusDebug:
            if (press.key == Key::Up) ui::debug_move(-1);
            if (press.key == Key::Down) ui::debug_move(1);
            if ((press.key == Key::Left && ui::debug_adjust(-1))
                || (press.key == Key::Right && ui::debug_adjust(1)))
                store::save_settings();
            if (press.key == Key::Enter && ui::debug_run()) store::save_settings();
            break;

        case ui::Screen::ChimeLab:
            if (press.key == Key::Left)  ui::chime_move(-1, 0);
            if (press.key == Key::Right) ui::chime_move(1, 0);
            if (press.key == Key::Up)    ui::chime_move(0, -1);
            if (press.key == Key::Down)  ui::chime_move(0, 1);
            if (press.key == Key::Left || press.key == Key::Right
                || press.key == Key::Up || press.key == Key::Down)
                store::save_settings();
            if (press.key == Key::Enter) audio::play(audio::Cue::Boot);
            break;

        case ui::Screen::Deck:
        default:
            if (press.key == Key::Up || press.key == Key::Right
                || press.key == Key::Down || press.key == Key::Left) {
                if (!send_joystick_impulse(press.key))
                    ui::toast("NO HOST", "stick not sent", theme::kOrange);
            }
            if (press.key == Key::Digit) {
                const int slot = (press.digit == 0 ? 10 : press.digit) - 1;
                if (slot < s.task_count) {
                    ui::select(slot);
                    ui::notify_press(slot);
                    send_agent_key(slot, true);
                    audio::play(audio::Cue::Select);
                }
            }
            if (press.key == Key::Enter) {
                // Stock Codex Micro's CODEX key submits the composer. Ending
                // push-to-talk only prepares text; Enter is the deliberate,
                // separate send gesture and therefore uses ACT12 press/release.
                if (!companion_codex_key("ACT12", 1, s.selected)) {
                    ui::toast("NO HOST", "message not sent", theme::kOrange);
                    break;
                }
                companion_codex_key("ACT12", 0, s.selected);
                audio::play(audio::Cue::Select);
            }
            if (press.key == Key::Interrupt) {
                const model::Task* task = model::selected_task();
                if (task) {
                    hostlink::sendf("CCP_INTERRUPT|%s\n", task->id);
                    ui::toast("INTERRUPT SENT", task->title, theme::kInput);
                }
            }
            break;
    }
}

}  // namespace

// ============================================================ link callbacks
namespace hostlink {

void handle_line(char* line)
{
    ui::wake();

    if (std::strcmp(line, "PING") == 0) { sendf("CCP_PONG|%s\n", kFirmwareVersion); return; }
    if (std::strncmp(line, "SCREENSHOT|", 11) == 0) {
        send_screenshot(line + 11);
        return;
    }
    if (std::strcmp(line, "AUTOSTART") == 0) { enable_m5apps_autostart(); return; }
    if (std::strncmp(line, "HOST|", 5) == 0) {
        std::snprintf(model::state.host, sizeof(model::state.host), "%s", line + 5);
        ui::invalidate();
        return;
    }

    if (std::strncmp(line, "CFG|", 4) == 0) {
        // Host-authoritative current values. The option lists themselves arrive
        // via OPT lines; here we only move the cursor onto what Codex is using.
        char* fields[3] = {};
        char* cursor = line + 4;
        for (int i = 0; i < 3 && cursor; ++i) {
            fields[i] = cursor;
            char* next = std::strchr(cursor, '|');
            if (next) { *next = 0; cursor = next + 1; } else cursor = nullptr;
        }
        auto& s = model::state;
        if (fields[0]) s.models.select_wire(fields[0]);
        if (fields[1]) s.efforts.select_wire(fields[1]);
        if (fields[2]) s.speeds.select_wire(fields[2]);
        store::save_settings();
        publish_settings(false);
        ui::invalidate();
        return;
    }

    // OPT|<MODEL|EFFORT|SPEED>|<current wire>|<label>=<wire>|...
    // The host owns these catalogues; hardcoding them in firmware would show
    // choices Codex does not offer and hide the ones it does.
    if (std::strncmp(line, "OPT|", 4) == 0) {
        char* cursor = line + 4;
        char* dimension = cursor;
        if ((cursor = std::strchr(cursor, '|')) == nullptr) return;
        *cursor++ = 0;
        char* current = cursor;
        if ((cursor = std::strchr(cursor, '|')) == nullptr) return;
        *cursor++ = 0;

        model::OptionList* list = nullptr;
        if (std::strcmp(dimension, "MODEL") == 0)       list = &model::state.models;
        else if (std::strcmp(dimension, "EFFORT") == 0) list = &model::state.efforts;
        else if (std::strcmp(dimension, "SPEED") == 0)  list = &model::state.speeds;
        if (!list) return;

        list->count = 0;
        char* save = nullptr;
        for (char* token = ::strtok_r(cursor, "|", &save);
             token && list->count < model::kMaxOptions;
             token = ::strtok_r(nullptr, "|", &save)) {
            char* equals = std::strchr(token, '=');
            if (!equals) continue;
            *equals = 0;
            std::snprintf(list->label[list->count], model::kLabelMax, "%s", token);
            std::snprintf(list->wire[list->count], model::kWireMax, "%s", equals + 1);
            ++list->count;
        }
        list->index = 0;
        list->select_wire(current);
        store::save_settings();
        publish_settings(false);
        ui::invalidate();
        return;
    }

    // TASK|<slot>|<status>|<threadId>|<title>. Title is last and unescaped, so a
    // '|' inside a task name can never shift the other fields.
    if (std::strncmp(line, "TASK|", 5) == 0) {
        char* cursor = line + 5;
        char* slot_text = cursor;
        char* status_text = nullptr;
        char* id_text = nullptr;
        char* title_text = nullptr;
        if ((cursor = std::strchr(cursor, '|')) == nullptr) return;
        *cursor++ = 0; status_text = cursor;
        if ((cursor = std::strchr(cursor, '|')) == nullptr) return;
        *cursor++ = 0; id_text = cursor;
        if ((cursor = std::strchr(cursor, '|')) == nullptr) return;
        *cursor++ = 0; title_text = cursor;

        const int slot = std::atoi(slot_text);
        if (slot < 0 || slot >= model::kMaxTasks) return;
        staged[slot] = model::Task{};
        staged[slot].status = parse_status(status_text);
        // A synthetic slot is bound, so diagnostics exercise the same visual
        // state as native thstatus rather than only the unbound outline.
        staged[slot].present     = true;
        staged[slot].seen        = true;
        std::snprintf(staged[slot].id, sizeof(staged[slot].id), "%s", id_text);
        sanitize_utf8(title_text, staged[slot].title, sizeof(staged[slot].title));
        if (slot + 1 > staged_count) staged_count = slot + 1;
        return;
    }

    if (std::strncmp(line, "TASKS|", 6) == 0) {
        char* count_text = line + 6;
        char* selected_text = std::strchr(count_text, '|');
        if (selected_text) *selected_text++ = 0;
        int count = std::atoi(count_text);
        if (count < 0) count = 0;
        if (count > model::kMaxTasks) count = model::kMaxTasks;
        staged_count = count;
        commit_tasks(selected_text ? std::atoi(selected_text) : -1);
        staged_count = 0;
        // Acknowledge the committed list so the host can stop resending it. The
        // The previous diagnostic protocol had no ack, so the host re-pushed state every few
        // seconds and kept waking the panel.
        sendf("CCP_DECK|%d|%d\n", model::state.task_count, model::state.selected);
        return;
    }

}

}  // namespace hostlink

extern "C" void companion_receive_line(const char* line)
{
    char copy[512];
    std::snprintf(copy, sizeof(copy), "%s", line);
    hostlink::note_host_activity();
    hostlink::handle_line(copy);
}

extern "C" void companion_transport_activity() { hostlink::note_host_activity(); }

extern "C" void companion_ble_link_changed(bool connected)
{
    hostlink::emit("ble_link", connected, connected ? "connected" : "disconnected");
    ui::invalidate();
}

// ==================================================================== startup
extern "C" void app_main(void)
{
    auto config = M5.config();
    M5.begin(config);

    hostlink::init();
    usb_serial_jtag_vfs_use_driver();
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    nvs_flash_init();
    store::init();
    ui::init();

    hostlink::emit("boot", true, kFirmwareVersion);
    hostlink::sendf("CCP_HELLO|%s|%s\n", kFirmwareVersion, esp_get_idf_version());
    hostlink::sendf("CCP_BOOT|reset_reason=%d\n", static_cast<int>(esp_reset_reason()));

    keys::init();
    {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "%s,board=%d",
                      keys::backend_name(), static_cast<int>(M5.getBoard()));
        hostlink::emit("keyboard", keys::backend() != keys::Backend::None, detail);
    }
    audio::init();

    if (model::state.usb_hid_enabled) companion_usb_start();
    companion_ble_start();
    char bonds[24];
    std::snprintf(bonds, sizeof(bonds), "bonds=%d", companion_ble_bond_count());
    hostlink::emit("ble_start", true, bonds);

    publish_settings(false);
    if (model::state.startup_sound_on) audio::play(audio::Cue::Boot);

    uint32_t last_status_ms = 0;
    uint32_t offline_since_ms = 0;
    bool pairing_was_active = false;
    constexpr uint32_t kOfflineGraceMs = 3000;
    // Codex currently refreshes device.status about once a minute. Leave
    // enough margin for scheduling jitter while still rejecting a stale HID
    // mount promptly after the native session disappears.
    constexpr uint32_t kCodexSessionIdleMs = 90000;

    while (true) {
        M5.update();
        hostlink::poll();
        companion_ble_service();
        const bool pairing_is_active = companion_ble_pairing_active();
        if (pairing_is_active != pairing_was_active) {
            pairing_was_active = pairing_is_active;
            ui::set_pairing_pin(pairing_is_active, companion_ble_pairing_passkey());
        }
        if (joystick_deflected
            && static_cast<int32_t>(lgfx::millis() - joystick_release_ms) >= 0) {
            companion_codex_joystick(0.f, 0.f);
            joystick_deflected = false;
        }

        // Transport arbitration: a physical link becomes visible only after a
        // valid native Codex RPC. USB wins while its HID interface is mounted;
        // BLE remains paired and takes over when the cable disappears.
        const model::Link previous_link = model::state.link;
        const bool codex_session_alive = hostlink::silence_ms() <= kCodexSessionIdleMs;
        const model::Link detected_link = !codex_session_alive ? model::Link::Offline
                                        : companion_usb_connected() ? model::Link::Usb
                                        : companion_ble_connected() ? model::Link::Ble
                                        : model::Link::Offline;
        const uint32_t link_now = lgfx::millis();
        if (detected_link == model::Link::Offline
            && previous_link != model::Link::Offline) {
            if (offline_since_ms == 0) {
                offline_since_ms = link_now;
                hostlink::sendf("CCP_LINK|offline_grace|start|ms=%u\n",
                                static_cast<unsigned>(kOfflineGraceMs));
            }
            if (link_now - offline_since_ms < kOfflineGraceMs) {
                model::state.link = previous_link;
            } else {
                model::state.link = model::Link::Offline;
                hostlink::sendf("CCP_LINK|offline_grace|expired\n");
            }
        } else {
            if (offline_since_ms != 0 && detected_link != model::Link::Offline) {
                hostlink::sendf("CCP_LINK|offline_grace|recovered|after_ms=%u\n",
                                static_cast<unsigned>(link_now - offline_since_ms));
            }
            offline_since_ms = 0;
            model::state.link = detected_link;
        }
        if (model::state.link != previous_link) {
            ui::invalidate();
            if (previous_link == model::Link::Offline && model::state.link != model::Link::Offline) {
                if (ui::screen() == ui::Screen::Boot) ui::go(ui::Screen::Deck);
                else ui::toast("LINKED", model::state.link == model::Link::Usb ? "USB" : "Bluetooth", theme::kRun);
                publish_settings(false);
            } else if (model::state.link == model::Link::Offline) {
                codex_micro::begin_session_sync();
                if (voice_button_active) {
                    voice_button_active = false;
                    ui::set_voice_active(false, model::state.selected);
                }
                ui::go(ui::Screen::Boot);
            }
        }

        // G0 mirrors the stock Codex Micro microphone key. Codex records from
        // the Mac and attaches the transcription to its currently selected
        // chat; Cardputer stores no audio and does not need an SD card.
        if (M5.BtnA.wasPressed()) {
            voice_button_active = false;
            if (!ui::wake()) {
                char key[5]; std::snprintf(key, sizeof(key), "AG%02d", model::state.selected);
                companion_codex_key(key, 1, model::state.selected);
                voice_button_active = companion_codex_key("ACT10", 1, model::state.selected);
                ui::set_voice_active(voice_button_active, model::state.selected);
                hostlink::sendf("CCP_VOICE|press|slot=%d|sent=%d\n",
                                model::state.selected, voice_button_active ? 1 : 0);
            }
        }
        if (M5.BtnA.wasReleased() && voice_button_active) {
            companion_codex_key("ACT10", 0, model::state.selected);
            hostlink::sendf("CCP_VOICE|release|slot=%d\n", model::state.selected);
            voice_button_active = false;
            ui::set_voice_active(false, model::state.selected);
        }

        for (Press press = keys::next(); press.key != Key::None; press = keys::next()) {
            // Telemetry first: the host sees every key even when the panel was
            // asleep and swallowed it, which is what makes input debuggable.
            hostlink::sendf("CCP_KEY|%s|%d|%s\n", keys::name(press.key), press.digit,
                            press.down ? "down" : "up");
            // A blanked panel eats the first key so the user never acts blind.
            if (ui::wake()) {
                // WAKE restores Codex lighting too. It is intentionally unknown
                // to the action map so
                // this first key cannot also approve, reject or switch a task.
                companion_codex_key("WAKE", 1);
                companion_codex_key("WAKE", 0);
                continue;
            }
            handle_press(press);
        }

        const uint32_t now = lgfx::millis();
        if (now - last_status_ms >= 5000) {
            last_status_ms = now;
            model::state.battery  = M5.Power.getBatteryLevel();
            model::state.charging = M5.Power.isCharging();
            ui::invalidate();
        }
        store::service();
        ui::service_power();
        ui::service();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
