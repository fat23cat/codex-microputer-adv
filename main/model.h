// Shared application model. The host owns the task list; the device owns the
// selection, the settings it can change locally, and all presentation state.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace model {

constexpr int kMaxTasks = 10;
constexpr int kTitleMax = 96;   // bytes, UTF-8
constexpr int kIdMax    = 48;

enum class Status : uint8_t { Idle, Running, NeedsInput, Done, Error };

struct Task {
    char    title[kTitleMax] = {};
    char    id[kIdMax]       = {};
    Status  status           = Status::Idle;
    bool    unseen_done      = false;  // completed since the user last looked
    bool    completion_hold  = false;  // keep a fresh completion green through debounce/animation
    bool    locally_viewed_done = false; // selected/opened locally; stale green host frames must not revive it
    uint32_t color           = 0;
    float    brightness      = 0;
    float    effect_speed    = 0;
    uint8_t  effect          = 0;
    bool     present         = false;
    bool     lighting_interrupted = false; // off/on restoration is not a status event

    bool     seen        = false; // distinguishes initial sync from a transition
};

struct Announcement {
    int8_t slot = -1;
    Status status = Status::Idle;
    Status previous_status = Status::Idle;
    bool unseen = false;
    bool target_unseen = false;
    bool previous_unseen = false;
    uint32_t queued_at_ms = 0;
    uint32_t ready_at_ms = 0;
};

enum class Link : uint8_t { Offline, Usb, Ble };

// Setting wheels are supplied by the host, not hardcoded: the model catalogue
// and the efforts each model supports change over time, and a stale built-in
// list would show the user choices that do not exist (or hide the one they are
// actually using).
constexpr int kMaxOptions = 8;
constexpr int kLabelMax   = 10;
constexpr int kWireMax    = 40;

struct OptionList {
    char label[kMaxOptions][kLabelMax] = {};
    char wire[kMaxOptions][kWireMax]   = {};
    int  count = 0;
    int  index = 0;

    const char* current_label() const { return count > 0 ? label[valid()] : "--"; }
    const char* current_wire()  const { return count > 0 ? wire[valid()] : ""; }
    int valid() const { return (index >= 0 && index < count) ? index : 0; }

    void step(int delta)
    {
        if (count <= 0) return;
        index = (valid() + delta % count + count) % count;
    }
    // Point at the entry matching `target`, if we have it.
    void select_wire(const char* target)
    {
        for (int i = 0; i < count; ++i) {
            if (std::strcmp(wire[i], target) == 0) { index = i; return; }
        }
    }
};

struct State {
    Task tasks[kMaxTasks] = {};
    int  task_count       = 0;
    int  selected         = 0;

    char host[24] = "";
    Link link     = Link::Offline;
    uint8_t ble_profile = 0;  // one of three independently pairable host slots
    bool usb_hid_enabled = true;
    int8_t ble_rssi = 0;
    bool ble_signal_weak = false;

    OptionList models;
    OptionList efforts;
    OptionList speeds;
    uint8_t sound_volume = 60;
    uint8_t unmuted_volume = 60;
    bool startup_sound_on = true;
    uint8_t startup_chime = 3;  // CLOUD
    uint16_t status_debounce_ms = 100;
    int16_t status_audio_offset_ms = 200;

    int  battery  = -1;
    bool charging = false;

    // Codex owns brightness and Auto-dim while connected. Desktop briefly
    // publishes an all-off frame while recomposing selection lighting, so UI
    // debounces the off edge before treating it as host Auto-dim.
    bool  host_lighting_seen = false;
    bool  host_zones_enabled = true;
    bool  host_threads_enabled = true;
    uint32_t host_lighting_serial = 0;
    float host_brightness    = 1.f;
    uint32_t host_activity_serial = 0;

    // Pending status takeovers. One slot owns at most one pending event: a new
    // state for that slot replaces stale news, while different slots preserve
    // arrival order and are presented one by one. Because slots are unique in
    // the queue and only six slots exist, the count can never exceed six; the
    // append guard below is therefore unreachable for valid slots.
    Announcement announcements[6] = {};
    uint8_t announcement_count = 0;
};

extern State state;

inline void queue_announcement(int slot, Status status, Status previous,
                               bool unseen, bool target_unseen,
                               bool previous_unseen, uint32_t now_ms)
{
    if (slot < 0 || slot >= 6) return;
    Announcement event{static_cast<int8_t>(slot), status, previous, unseen,
                       target_unseen, previous_unseen, now_ms,
                       now_ms + state.status_debounce_ms};
    for (uint8_t i = 0; i < state.announcement_count; ++i) {
        if (state.announcements[i].slot == slot) {
            const uint32_t gap_ms = now_ms - state.announcements[i].queued_at_ms;
            std::printf("CCP_DEBOUNCE|slot=%d|replace=1|gap_ms=%u|window_ms=%u\n",
                        slot, static_cast<unsigned>(gap_ms),
                        static_cast<unsigned>(state.status_debounce_ms));
            // Keep the original visual starting point but replace the outcome.
            event.previous_status = state.announcements[i].previous_status;
            event.previous_unseen = state.announcements[i].previous_unseen;
            state.announcements[i] = event;
            return;
        }
    }
    if (state.announcement_count < 6) {
        std::printf("CCP_DEBOUNCE|slot=%d|replace=0|gap_ms=0|window_ms=%u\n",
                    slot, static_cast<unsigned>(state.status_debounce_ms));
        state.announcements[state.announcement_count++] = event;
    }
}

inline bool take_announcement_for_slot(int slot, uint32_t now_ms, Announcement& out)
{
    for (uint8_t i = 0; i < state.announcement_count; ++i) {
        if (state.announcements[i].slot != slot) continue;
        if (static_cast<int32_t>(now_ms - state.announcements[i].ready_at_ms) < 0)
            return false;
        out = state.announcements[i];
        for (uint8_t j = i + 1; j < state.announcement_count; ++j)
            state.announcements[j - 1] = state.announcements[j];
        --state.announcement_count;
        return true;
    }
    return false;
}

inline bool take_next_announcement(uint32_t now_ms, Announcement& out)
{
    if (state.announcement_count == 0) return false;
    if (static_cast<int32_t>(now_ms - state.announcements[0].ready_at_ms) < 0)
        return false;
    out = state.announcements[0];
    for (uint8_t i = 1; i < state.announcement_count; ++i)
        state.announcements[i - 1] = state.announcements[i];
    --state.announcement_count;
    return true;
}

inline bool has_announcement_for_slot(int slot)
{
    for (uint8_t i = 0; i < state.announcement_count; ++i) {
        if (state.announcements[i].slot == slot) return true;
    }
    return false;
}

inline bool settle_viewed_completion(Task& task, bool animation_owned)
{
    if (task.status != Status::Done || !task.locally_viewed_done
        || !task.completion_hold || animation_owned)
        return false;
    task.completion_hold = false;
    task.unseen_done = false;
    return true;
}

inline const Task* selected_task()
{
    if (state.task_count <= 0) return nullptr;
    int index = state.selected;
    if (index < 0) index = 0;
    if (index >= state.task_count) index = state.task_count - 1;
    return &state.tasks[index];
}

inline void mark_done_viewed(int slot)
{
    if (slot < 0 || slot >= state.task_count) return;
    Task& task = state.tasks[slot];
    if (task.status != Status::Done) return;
    task.locally_viewed_done = true;
    // A fresh completion remains green until its takeover has played. Existing
    // unread completions become viewed as soon as Codex selects the slot.
    if (!task.completion_hold) task.unseen_done = false;
}

}  // namespace model
