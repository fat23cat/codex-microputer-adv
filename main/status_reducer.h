#pragma once

#include <cstdint>

#include "model.h"

namespace status_reducer {

struct LampFrame {
    uint32_t color = 0;
    float brightness = 0.f;
    float speed = 0.f;
    uint8_t effect = 0;
};

struct Result {
    model::Status before = model::Status::Idle;
    bool was_unseen = false;
    bool semantic = false;
    bool restoration = false;
    bool initial_sync = false;
    bool changed = false;
    bool target_unseen = false;
    bool event_green = false;
};

inline Result apply(model::Task& task, const LampFrame& frame, bool baseline = false)
{
    Result result;
    result.before = task.status;
    result.was_unseen = task.unseen_done;

    task.color = frame.color & 0xffffff;
    task.brightness = frame.brightness;
    task.effect = frame.effect;
    task.effect_speed = frame.speed;
    task.present = task.effect != 0 && task.brightness > 0.001f;
    if (!task.present) task.lighting_interrupted = true;

    model::Status incoming = result.before;
    result.semantic = task.present;
    if (task.color == 0x304ffe) incoming = model::Status::Running;
    else if (task.color == 0x00ff4c || task.color == 0xffffff)
        incoming = model::Status::Done;
    else if (task.color == 0xff6d00) incoming = model::Status::NeedsInput;
    else if (task.color == 0xff0033) incoming = model::Status::Error;
    else result.semantic = false;

    result.restoration = result.semantic && task.lighting_interrupted;
    if (result.semantic) {
        task.status = incoming;
        task.lighting_interrupted = false;
    }

    const bool fresh_completion = !baseline && task.seen
                               && result.before != model::Status::Done;
    if (task.status != model::Status::Done) {
        task.unseen_done = false;
        task.completion_hold = false;
        task.locally_viewed_done = false;
    } else {
        if (fresh_completion) {
            task.completion_hold = true;
            task.locally_viewed_done = false;
        }
        const bool host_unseen = task.color == 0x00ff4c;
        result.target_unseen = host_unseen && !task.locally_viewed_done;
        task.unseen_done = task.completion_hold ? true : result.target_unseen;
    }

    result.initial_sync = baseline || !task.seen;
    if (result.initial_sync) {
        task.seen = true;
    } else {
        result.changed = result.semantic && !result.restoration
                      && task.status != result.before;
    }
    result.event_green = task.status == model::Status::Done
                       ? true : task.unseen_done;
    return result;
}

}  // namespace status_reducer
