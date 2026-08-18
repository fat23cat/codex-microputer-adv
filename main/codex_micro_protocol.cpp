#include "codex_micro_protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_timer.h"
#include "link.h"
#include "model.h"
#include "rpc_framer.h"
#include "session_sync.h"
#include "status_reducer.h"
#include "ui.h"

namespace codex_micro {
namespace {

constexpr uint8_t kRpcChannel = rpc_framer::kChannel;
constexpr size_t kPayload = rpc_framer::kPayload;
constexpr size_t kBody = 63;  // BLE characteristic excludes report id 6.

SendReport send_report = nullptr;
rpc_framer::Assembler input;
uint32_t selection_guard_until_ms = 0;
session_sync::Tracker session;

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void extend_selection_guard(uint32_t duration_ms)
{
    const uint32_t candidate = now_ms() + duration_ms;
    if (static_cast<int32_t>(candidate - selection_guard_until_ms) > 0)
        selection_guard_until_ms = candidate;
}

bool selection_guarded()
{
    return static_cast<int32_t>(selection_guard_until_ms - now_ms()) > 0;
}

bool transmit(const char* json)
{
    if (!send_report || !json) return false;
    std::string wire(json);
    wire.push_back('\n');
    for (size_t offset = 0; offset < wire.size(); offset += kPayload) {
        uint8_t report[kBody] = {};
        const size_t count = std::min(kPayload, wire.size() - offset);
        report[0] = kRpcChannel;
        report[1] = static_cast<uint8_t>(count);
        std::memcpy(report + 2, wire.data() + offset, count);
        if (!send_report(report, sizeof(report))) return false;
    }
    return true;
}

void response(cJSON* id, cJSON* result)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON_AddItemToObject(root, "result", result ? result : cJSON_CreateObject());
    char* text = cJSON_PrintUnformatted(root);
    if (text) { transmit(text); cJSON_free(text); }
    cJSON_Delete(root);
}

bool apply_thread_lights(cJSON* params)
{
    if (!cJSON_IsArray(params)) return false;

    // Codex publishes a transient all-off frame while rebuilding Micro lights
    // after model/reasoning controls and some focus changes. It is not a task
    // transition. Applying it would turn every slot Idle and make the restored
    // frame look like six fresh status events, replaying the whole deck after
    // an unrelated key such as D. Keep the last truthful task state, while the
    // all-off edge still participates in display power together with rgbcfg.
    bool any_lit = false;
    float brightest = 0.f;
    cJSON* probe = nullptr;
    cJSON_ArrayForEach(probe, params) {
        cJSON* brightness = cJSON_GetObjectItemCaseSensitive(probe, "b");
        cJSON* effect = cJSON_GetObjectItemCaseSensitive(probe, "e");
        if (cJSON_IsNumber(brightness)) {
            brightest = std::max(brightest, std::clamp(
                static_cast<float>(brightness->valuedouble), 0.f, 1.f));
        }
        if (cJSON_IsNumber(brightness) && brightness->valuedouble > 0.001
            && (!cJSON_IsNumber(effect) || effect->valueint != 0)) {
            any_lit = true;
            break;
        }
    }
    if (!any_lit) {
        for (auto& task : model::state.tasks) task.lighting_interrupted = true;
        if (model::state.host_threads_enabled) {
            model::state.host_threads_enabled = false;
            ++model::state.host_lighting_serial;
        }
        std::printf("CCP_NATIVE|thstatus|all_off_power_only\n");
        return false;
    }
    if (brightest > 0.001f) model::state.host_brightness = brightest;

    // Model/reasoning surfaces temporarily repaint Micro's six lamps with UI
    // colours. They are presentation, not task state: feeding (for example) a
    // red picker preview into the status reducer falsely announces Error.
    // Preserve the truthful deck until the preview guard expires.
    if (selection_guarded() || ui::composer_control_active()) {
        if (!model::state.host_threads_enabled) {
            model::state.host_threads_enabled = true;
            ++model::state.host_lighting_serial;
        }
        ++model::state.host_activity_serial;
        ui::note_composer_control_preview();
        std::printf("CCP_NATIVE|thstatus|picker_preview_ignored\n");
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, params) {
        cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsNumber(id) || id->valueint < 0 || id->valueint >= 6) continue;
        auto& task = model::state.tasks[id->valueint];
        cJSON* color = cJSON_GetObjectItemCaseSensitive(item, "c");
        cJSON* brightness = cJSON_GetObjectItemCaseSensitive(item, "b");
        cJSON* effect = cJSON_GetObjectItemCaseSensitive(item, "e");
        cJSON* speed = cJSON_GetObjectItemCaseSensitive(item, "s");
        status_reducer::LampFrame frame{
            task.color, task.brightness, task.effect_speed, task.effect};
        if (cJSON_IsNumber(color))
            frame.color = static_cast<uint32_t>(color->valuedouble) & 0xffffff;
        if (cJSON_IsNumber(brightness))
            frame.brightness = static_cast<float>(brightness->valuedouble);
        if (cJSON_IsNumber(effect)) frame.effect = static_cast<uint8_t>(effect->valueint);
        if (cJSON_IsNumber(speed)) frame.speed = static_cast<float>(speed->valuedouble);
        const status_reducer::Result reduced = status_reducer::apply(
            task, frame, session.baseline());

        if (reduced.changed) {
            ++model::state.host_activity_serial;
            // Worth the whole screen. Only a real change raises it: the app
            // republishes the same lighting every couple of seconds, and a panel
            // that flashed on every poll would be unreadable.
            if (task.present) {
                model::queue_announcement(id->valueint, task.status,
                                          reduced.before,
                                          reduced.event_green,
                                          reduced.target_unseen,
                                          reduced.was_unseen,
                                          static_cast<uint32_t>(esp_timer_get_time() / 1000));
            }
        }
    }
    model::state.task_count = 6;
    bool any_thread_enabled = false;
    for (int i = 0; i < 6; ++i) any_thread_enabled = any_thread_enabled || model::state.tasks[i].present;
    if (model::state.host_threads_enabled != any_thread_enabled) {
        model::state.host_threads_enabled = any_thread_enabled;
        ++model::state.host_lighting_serial;
    }
    // Desktop deduplicates identical light payloads, so receiving thstatus is a
    // meaningful host event rather than a poll heartbeat.
    ++model::state.host_activity_serial;

    // Desktop encodes the selected chat as the only persistent breath effect
    // (e=4); ordinary slots are solid (e=1). Reflect that host selection back
    // into the hardware cursor instead of keeping a divergent local choice.
    int breath_slot = -1;
    int breath_count = 0;
    for (int i = 0; i < 6; ++i) {
        const auto& task = model::state.tasks[i];
        if (task.present && task.effect == 4) { breath_slot = i; ++breath_count; }
    }
    if (!selection_guarded() && breath_count == 1
        && model::state.selected != breath_slot) {
        model::state.selected = breath_slot;
        ++model::state.host_activity_serial;
        std::printf("CCP_NATIVE|host_select|%d\n", breath_slot);
    } else if (selection_guarded() && breath_count == 1) {
        std::printf("CCP_NATIVE|host_select_ignored|%d|preview\n", breath_slot);
    }
    return true;
}

void apply_lighting_config(cJSON* params)
{
    if (!cJSON_IsObject(params)) return;
    float brightest = 0.f;
    bool any_enabled = false;
    for (const char* name : {"ambient", "keys"}) {
        cJSON* side = cJSON_GetObjectItemCaseSensitive(params, name);
        if (!cJSON_IsObject(side)) continue;
        cJSON* brightness = cJSON_GetObjectItemCaseSensitive(side, "b");
        cJSON* effect = cJSON_GetObjectItemCaseSensitive(side, "e");
        const float value = cJSON_IsNumber(brightness)
            ? std::clamp(static_cast<float>(brightness->valuedouble), 0.f, 1.f) : 0.f;
        brightest = std::max(brightest, value);
        any_enabled = any_enabled || (value > 0.001f
            && (!cJSON_IsNumber(effect) || effect->valueint != 0));
    }
    model::state.host_lighting_seen = true;
    if (model::state.host_zones_enabled != any_enabled) {
        model::state.host_zones_enabled = any_enabled;
        ++model::state.host_lighting_serial;
    }
    // Keep the last non-zero slider value through an Auto-dim/off frame.
    if (brightest > 0.001f) {
        model::state.host_brightness = brightest;
    }
    std::printf("CCP_NATIVE|rgbcfg|brightness=%.3f|enabled=%d\n",
                static_cast<double>(brightest), any_enabled ? 1 : 0);
}

void handle(const char* json)
{
    cJSON* root = cJSON_Parse(json);
    if (!root) {
        const char* error = cJSON_GetErrorPtr();
        std::printf("CCP_NATIVE|rpc|parse_error|bytes=%u|offset=%u\n",
                    static_cast<unsigned>(std::strlen(json)),
                    error ? static_cast<unsigned>(error - json) : 0U);
        return;
    }
    cJSON* method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(method)) { cJSON_Delete(root); return; }
    // A mounted HID interface only proves that a cable or BLE link exists.
    // A valid Codex RPC proves that the companion session itself is alive.
    hostlink::note_host_activity();
    std::printf("CCP_NATIVE|rpc|%s\n", method->valuestring);

    cJSON* result = cJSON_CreateObject();
    if (std::strcmp(method->valuestring, "sys.version") == 0) {
        cJSON_AddStringToObject(result, "version", "1.0.0-cardputer-adv");
    } else if (std::strcmp(method->valuestring, "device.status") == 0) {
        cJSON_AddStringToObject(result, "version", "1.0.0-cardputer-adv");
        cJSON_AddNumberToObject(result, "profile_index", model::state.ble_profile);
        cJSON_AddNumberToObject(result, "layer_index", 0);
        cJSON_AddNumberToObject(result, "battery", std::max(0, model::state.battery));
        cJSON_AddBoolToObject(result, "is_charging", model::state.charging);
        session.note(session_sync::Method::DeviceStatus);
    } else if (std::strcmp(method->valuestring, "v.oai.thstatus") == 0) {
        if (apply_thread_lights(params))
            session.note(session_sync::Method::ThreadStatus);
        cJSON_AddBoolToObject(result, "ok", true);
    } else if (std::strcmp(method->valuestring, "v.oai.rgbcfg") == 0) {
        apply_lighting_config(params);
        session.note(session_sync::Method::LightingConfig);
        cJSON_AddBoolToObject(result, "ok", true);
    } else if (std::strcmp(method->valuestring, "lights.preview") == 0) {
        extend_selection_guard(2500);
        ui::note_composer_control_preview();
        cJSON_AddBoolToObject(result, "ok", true);
    } else if (std::strcmp(method->valuestring, "host.focused_app") == 0) {
        cJSON_AddBoolToObject(result, "ok", true);
    } else {
        cJSON_Delete(result);
        cJSON* error = cJSON_CreateObject();
        cJSON_AddNumberToObject(error, "code", -32601);
        cJSON_AddStringToObject(error, "message", "Method not found");
        cJSON* out = cJSON_CreateObject();
        cJSON_AddItemToObject(out, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
        cJSON_AddItemToObject(out, "error", error);
        char* text = cJSON_PrintUnformatted(out);
        if (text) { transmit(text); cJSON_free(text); }
        cJSON_Delete(out);
        cJSON_Delete(root);
        return;
    }
    response(id, result);
    cJSON_Delete(root);
}

void reset_session_sync()
{
    session.begin();
    model::state.announcement_count = 0;
    for (auto& task : model::state.tasks) task.completion_hold = false;
    ui::cancel_status_announcements();
    std::printf("CCP_NATIVE|session_sync|baseline\n");
}

}  // namespace

void init(SendReport sender) { send_report = sender; }

void begin_session_sync() { reset_session_sync(); }

void suppress_host_selection(uint32_t duration_ms)
{
    extend_selection_guard(duration_ms);
}

void receive_report(const uint8_t* body, size_t length)
{
    const rpc_framer::FeedResult result = input.feed(
        body, length, [](const std::string& message) { handle(message.c_str()); });
    if (result == rpc_framer::FeedResult::Invalid) {
        const unsigned count = body && length >= 2 ? body[1] : 0;
        std::printf("CCP_NATIVE|rx|invalid|length=%u|count=%u\n",
                    static_cast<unsigned>(length), count);
    } else if (result == rpc_framer::FeedResult::Oversize) {
        std::printf("CCP_NATIVE|rx|oversize|max=%u\n",
                    static_cast<unsigned>(rpc_framer::kMaxJson));
    }
}

bool send_key(const char* key, int action, int agent)
{
    char json[160];
    if (agent >= 0) {
        std::snprintf(json, sizeof(json),
            "{\"method\":\"v.oai.hid\",\"params\":{\"k\":\"%s\",\"act\":%d,\"ag\":%d}}",
            key, action, agent);
    } else {
        std::snprintf(json, sizeof(json),
            "{\"method\":\"v.oai.hid\",\"params\":{\"k\":\"%s\",\"act\":%d}}", key, action);
    }
    const bool sent = transmit(json);
    std::printf("CCP_NATIVE|key|%s|action=%d|agent=%d|sent=%d\n",
                key, action, agent, sent ? 1 : 0);
    return sent;
}

bool send_joystick(float angle, float distance)
{
    char json[128];
    std::snprintf(json, sizeof(json),
        "{\"method\":\"v.oai.rad\",\"params\":{\"a\":%.3f,\"d\":%.3f}}", angle, distance);
    return transmit(json);
}

}  // namespace codex_micro
