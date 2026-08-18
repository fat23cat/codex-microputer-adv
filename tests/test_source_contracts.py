#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
failures: list[str] = []


def require(path: str, pattern: str, message: str) -> None:
    text = (ROOT / path).read_text()
    if not re.search(pattern, text, re.MULTILINE):
        failures.append(message)


def forbid(path: str, pattern: str, message: str) -> None:
    text = (ROOT / path).read_text()
    if re.search(pattern, text, re.MULTILINE):
        failures.append(message)


require("main/model.h", r"status_debounce_ms\s*=\s*100;", "default debounce must be 100 ms")
require("main/model.h", r"status_audio_offset_ms\s*=\s*200;", "default audio offset must be +200 ms")
require("main/store.cpp", r'"audio_of3"', "current NVS audio-offset key must be audio_of3")
require("main/ui.cpp", r"std::clamp\([\s\S]{0,160}-300,\s*300\)",
        "audio offset UI must clamp to -300..+300")
require("main/status_timing.h", r"hold\s*=\s*1\.85f;", "status hold must be 1.85 s")
require("README.md", r"-300\.\.[+]300 ms status-audio offset", "README must publish -300..+300 ms")
require("SCENARIOS.ru.md", r"фиксируется на 1,85 секунды",
        "scenario duration must match status_timing.h")
require("main/codex_micro_protocol.cpp",
        r'"lights\.preview"[\s\S]{0,180}ui::note_composer_control_preview\(\)',
        "native light preview must feed the composer control overlay")
require("main/ui.cpp", r"if \(composer_control_idle >= 8\.f\) set_composer_control_active\(false\)",
        "composer control overlay must not remain stuck")
require("main/ui.cpp",
        r"void note_composer_control_preview\(\)[\s\S]{0,450}if \(composer_control_target\)",
        "late light preview must not reopen a confirmed control")
require("main/codex_micro_protocol.cpp",
        r"if \(selection_guarded\(\) \|\| ui::composer_control_active\(\)\)[\s\S]{0,520}picker_preview_ignored[\s\S]{0,80}return false;",
        "picker lamp preview must bypass the task status reducer")
require("main/main.cpp",
        r"Cue::StepRight\s*:\s*audio::Cue::StepLeft",
        "encoder detents must have directional audio")
require("main/main.cpp",
        r"if \(had_preview\)[\s\S]{0,120}Cue::MenuApply",
        "host-previewed composer selection must have a distinct apply cue")
require("main/audio.cpp",
        r"apply: C5 -> E5 -> G5",
        "composer confirmation must use the successful rising major cue")
require("main/codex_micro_protocol.cpp",
        r"selection_guarded\(\) \|\| ui::composer_control_active\(\)",
        "host-opened composer control must keep previews out of task status")
require("main/ui.cpp",
        r"composer_control_open_sound_pending[\s\S]{0,260}audio::play\(audio::Cue::MenuOpen\)",
        "host-opened composer cue must be deferred to the main UI task")
require("main/main.cpp",
        r"if \(ui::composer_control_active\(\)\)\s*\n\s*ui::notify_composer_control_step",
        "encoder rotation must not invent a local control mode")
require("main/main.cpp",
        r"press\.key == Key::Enter[\s\S]{0,420}companion_codex_key\(\"ACT12\", 1[\s\S]{0,180}companion_codex_key\(\"ACT12\", 0",
        "Enter must send the native Codex Micro composer action")
require("main/key_layout.h",
        r"col >= 5 && col <= 10[\s\S]{0,80}Key::NativeAction, col \+ 1",
        "T through P must expose native command slots ACT06 through ACT11")
require("main/main.cpp",
        r"if \(!press\.down\)[\s\S]{0,350}Key::NativeAction[\s\S]{0,100}send_native_action\(press\.digit, false",
        "all native command slots must preserve their release edge")
require("main/main.cpp",
        r"if \(!press\.down\)[\s\S]{0,180}send_agent_key\(slot, false\)[\s\S]{0,15000}send_agent_key\(slot, true\)",
        "agent keys must preserve down and up for native double tap")
require("main/main.cpp",
        r"press\.key == Key::EncoderPress[\s\S]{0,100}companion_codex_key\(\"ENC\", 0\)[\s\S]{0,5000}send_encoder_press\(\)",
        "encoder release must remain physical so Codex can detect long press")
require("main/ble_transport.cpp",
        r"if \(companion_usb_connected\(\)\) return companion_usb_send_rpc",
        "native USB must have exclusive priority over BLE")
require("main/ble_transport.cpp", r"kNativeAddresses\[3\]\[6\]",
        "three BLE host identities must exist")
require("main/store.cpp", r'"ble_slot"',
        "selected BLE host channel must persist")
require("main/ble_transport.cpp",
        r"ble_gap_adv_stop\(\)[\s\S]{0,900}ble_gap_terminate",
        "BLE profile switching must restart only the radio session")
require("main/main.cpp", r"companion_ble_select_profile\(profile\)",
        "settings must hot-switch the selected BLE profile")
require("main/main.cpp", r"SettingsRow::BleProfile[\s\S]{0,500}(?!esp_restart)",
        "BLE profile settings must not restart the device")
require("main/main.cpp",
        r"companion_usb_connected\(\) \? model::Link::Usb[\s\S]{0,100}companion_ble_connected",
        "visible link state must follow native USB then BLE")
require("main/codex_micro_protocol.cpp",
        r"if \(!cJSON_IsString\(method\)\)[\s\S]{0,260}hostlink::note_host_activity\(\)",
        "a valid native Codex RPC must refresh the live session")
require("main/main.cpp",
        r"codex_session_alive\s*=\s*hostlink::silence_ms\(\)[\s\S]{0,220}!codex_session_alive\s*\?\s*model::Link::Offline",
        "a mounted transport without a live Codex session must stay offline")
require("main/main.cpp",
        r"s\.link == model::Link::Offline[\s\S]{0,260}press\.key == Key::Settings[\s\S]{0,180}ui::Screen::Boot\s*:\s*ui::Screen::Settings",
        "Tab must toggle local settings from the offline splash")
require("main/main.cpp",
        r"developer_preview_active\(\)[\s\S]{0,180}Key::Back[\s\S]{0,100}close_developer_preview\(\)[\s\S]{0,40}return;",
        "developer previews must capture all input and close only on Esc")
require("main/main.cpp",
        r"PreviewSplash[\s\S]{0,650}DeveloperPreview::Splash[\s\S]{0,300}DeveloperPreview::Pairing[\s\S]{0,300}DeveloperPreview::Control",
        "developer settings must expose splash, PIN and control previews")
require("main/audio.cpp",
        r"sound_volume\) \* 250u",
        "default 60 percent must preserve original hardware output 150")
require("main/audio.cpp",
        r"CCP_CHIME_READY[\s\S]{0,500}playing_buffer\.store\(0,[\s\S]{0,160}playRaw\(status_pcm\[0\]",
        "startup chime buffer must be reserved before speaker DMA can consume it")
require("main/model.h", r"sound_volume\s*=\s*60;[\s\S]{0,80}unmuted_volume\s*=\s*60;[\s\S]{0,120}startup_chime\s*=\s*3;",
        "clean settings must default to original 60% volume and CLOUD")
require("main/store.cpp", r'"volume_v4"', "restored original volume scale must use a fresh NVS key")
require("main/store.cpp", r'"chime_d3"', "CLOUD default must use a new NVS key")
require("main/ui.cpp",
        r"draw_pairing_takeover\(\)[\s\S]{0,500}draw_splash_field\(0\.48f, 63\)[\s\S]{0,1000}micro5_pin_glyph[\s\S]{0,500}draw_micro5_digit",
        "pairing must reuse splash styling and centre PIN in the deck numeral face")
require("tools/generate_micro5_digits.py", r'GLYPHS = "1234560789"',
        "Micro5 asset must cover PIN digits without changing deck glyph indices")
require("main/ui.cpp",
        r"packet < 24[\s\S]{0,500}0\.145f \+ \(packet % 5\) \* 0\.014f[\s\S]{0,700}packet % 6 == 0 \? kBlue : kDim[\s\S]{0,500}tail <= 2",
        "the splash background must animate hard-edged pixel packets and tails")
require("main/ui.cpp", r'USB OR BLUETOOTH  OPEN CODEX',
        "the splash must present USB and Bluetooth as equal native transports")
require("main/ui.cpp",
        r'mark_y = static_cast<int>\(motion::lerp\(51\.f, 44\.f, intro\)\)[\s\S]{0,260}draw_tracked_transparent\("CODEX", mark_x \+ 1',
        "the splash wordmark must be lowered and pixel-overprinted for weight")
require("main/ui.cpp",
        r"fmod\(clock_phase, 2\.00f\) < 1\.55f[\s\S]{0,180}USB OR BLUETOOTH",
        "the connection prompt must retain its readable arcade blink cadence")
require("main/theme.h", r"kGreen\s*=\s*rgb\(38,\s*198,\s*58\)",
        "unread completion green must remain the saturated product colour")
require("main/theme.h", r"kViewed\s*=\s*rgb\(228,\s*229,\s*225\)",
        "viewed completion gray must remain close to the paper background")
require("main/store.cpp", r'"usb_hid"',
        "developer USB HID preference must persist")
require("main/usb_transport.cpp",
        r"companion_usb_set_enabled\(bool requested\)[\s\S]{0,650}tinyusb_driver_uninstall\(\)[\s\S]{0,1600}tinyusb_driver_install",
        "USB HID must support real runtime detach and reattach")
require("main/main.cpp",
        r"DebugSettingsRow::UsbHid[\s\S]{0,600}companion_usb_set_enabled\(requested\)",
        "developer settings must control the USB HID transport")
require("main/ble_transport.cpp",
        r"if \(companion_usb_connected\(\)\) return companion_usb_send_rpc[\s\S]{0,180}return send_ble_rpc_body",
        "BLE must remain the authoritative fallback while USB HID is disabled")
require("main/ble_transport.cpp",
        r"esp_hid_ble_gap_adv_init\(ESP_HID_APPEARANCE_KEYBOARD[\s\S]{0,450}sm_mitm\s*=\s*1;[\s\S]{0,100}sm_io_cap\s*=\s*BLE_HS_IO_DISPLAY_ONLY",
        "BLE pairing policy must be applied after helper defaults with authenticated display PIN")
require("main/ble_transport.cpp",
        r"constexpr auto kReportMap[\s\S]{0,180}0x05,0x01,0x09,0x06[\s\S]{0,500}0x06,0x00,0xFF[\s\S]{0,250}0x85,0x06",
        "BLE HID map must expose the complete Codex Micro keyboard and vendor RPC collections")
require("main/ui.cpp",
        r"void draw_pairing_takeover\(\)[\s\S]{0,1500}ENTER PIN ON MAC",
        "Cardputer must display the BLE passkey and where to enter it")
require("main/main.cpp",
        r"companion_ble_pairing_active\(\)[\s\S]{0,240}ui::set_pairing_pin",
        "BLE security events must control the pairing screen")
require("main/ble_transport.cpp",
        r"stored_bond_count\(\) == 0[\s\S]{0,100}pairing_active\.store\(needs_pairing",
        "bonded BLE reconnects must not reopen the pairing overlay")
require("main/ble_transport.cpp",
        r"BLE_GAP_EVENT_ENC_CHANGE[\s\S]{0,180}pairing_active\.store\(false",
        "successful encryption must dismiss the pairing PIN")
require("main/ble_transport.cpp",
        r"ESP_HIDD_OUTPUT_EVENT[\s\S]{0,400}pairing_active\.store\(false[\s\S]{0,100}codex_micro::receive_report",
        "the first native Codex report must dismiss a stale pairing overlay")
require("main/ble_transport.cpp",
        r"ble_gap_conn_rssi[\s\S]{0,1100}adaptive_ble::next_tier",
        "connected BLE power must adapt to measured RSSI")
require("main/ble_transport.cpp",
        r"void apply_connection_power[\s\S]{0,350}esp_ble_tx_power_set_enhanced",
        "adaptive BLE tiers must be applied to the active connection handle")
require("main/ble_transport.cpp",
        r"BLE_GAP_LE_PHY_CODED_MASK[\s\S]{0,220}BLE_GAP_LE_PHY_CODED_S2",
        "weak BLE links must be able to negotiate coded S2 PHY")
require("main/ble_transport.cpp",
        r"adaptive_ble::tuning_ready\(connected_ms\) && next_coded != coded_phy[\s\S]{0,450}ble_gap_set_prefered_le_phy",
        "PHY negotiation must wait until the controller connection has settled")
require("main/ble_transport.cpp",
        r"ble_hid_task_start_up[\s\S]{0,300}BLE_HS_CONN_HANDLE_NONE[\s\S]{0,180}esp_hid_ble_gap_adv_start",
        "HID lifecycle hook must not restart advertising during an active connection")
require("main/ble_transport.cpp",
        r"ble_gap_adv_active\(\)[\s\S]{0,220}advertise_profile",
        "advertising watchdog must recover a stopped reconnect path")
require("main/ble_transport.cpp",
        r"ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P20",
        "disconnected advertising must use maximum discovery power")
require("main/ble_transport.cpp",
        r"adaptive_ble::weak_signal[\s\S]{0,180}ui::invalidate",
        "RSSI hysteresis must repaint when weak-link state changes")
require("main/ui.cpp",
        r"ble_signal_weak[\s\S]{0,500}fill_rect\(badge_x, badge_y, badge_w, badge_h, kInk\)[\s\S]{0,180}LOW SIGNAL",
        "weak BLE must use a compact dark text badge rather than an icon")
require("tools/install.py",
        r'--after", "no_reset"[\s\S]{0,500}select_ota_partition\(args\.port, label, args\.baud\)[\s\S]{0,100}installed and launched',
        "M5Apps installer must select and launch the flashed OTA app without a manual picker")
require("main/codex_micro_protocol.cpp",
        r"status_reducer::apply\(\s*task, frame, session\.baseline\(\)\)",
        "all status snapshots during control-plane bootstrap must be baseline-only")
require("main/codex_micro_protocol.cpp",
        r"if \(apply_thread_lights\(params\)\)\s*session\.note\(session_sync::Method::ThreadStatus\)",
        "all-off and picker preview frames must not complete session baseline")
require("main/ui.cpp",
        r"current == Screen::Boot && !transition\.running\(\)[\s\S]{0,180}display_fade::apply",
        "low-brightness colour collapse must be restricted to the stable splash")
require("main/theme.h",
        r"kIdleDigit\s*=\s*rgb\(142,\s*138,\s*128\)[\s\S]{0,100}kIdleDigitSelected\s*=\s*rgb\(78,\s*76,\s*71\)",
        "idle digits must use distinct dark-grey resting and selected tones")
require("main/ui.cpp",
        r"Status::Done\)\s+return t\.unseen_done \? kInk : kIdleDigit[\s\S]{0,300}Status::Done && !t\.unseen_done",
        "viewed completed tasks must use the inactive grey digit scale")
require("main/main.cpp",
        r"had_preview[\s\S]{0,120}dismiss_composer_control_preview[\s\S]{0,100}else ui::allow_composer_control_preview",
        "encoder press must explicitly toggle the composer control overlay")
require("main/ui.cpp",
        r"dismiss_composer_control_preview\(\)[\s\S]{0,180}suppressed_until_ms = lgfx::millis\(\) \+ 5000",
        "late host previews must not reopen an explicitly dismissed overlay")
require("main/ui.cpp",
        r"kDimHoldMs\s*=\s*180000[\s\S]{0,100}kDarkAfterMs\s*=\s*kDimAfterMs \+ kDimHoldMs",
        "the readable ten-percent dim state must last three full minutes")
require("main/ui.cpp",
        r"now - lighting_changed_ms >= kDimHoldMs[\s\S]{0,180}configured / 10",
        "host auto-dim must use the same three-minute readable hold")

if failures:
    for failure in failures:
        print(f"FAIL source_contracts: {failure}", file=sys.stderr)
    raise SystemExit(1)
print("PASS source_contracts (68 invariants)")
