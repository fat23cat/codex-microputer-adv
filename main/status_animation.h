#pragma once

#include "lamp.h"
#include "model.h"
#include "motion.h"
#include "status_timing.h"

namespace status_animation {

inline bool lamp_is_viewed(const model::Task* task)
{
    return task && task->status == model::Status::Done
        && (task->color == lamp::kDoneSeen || task->locally_viewed_done);
}

inline float viewed_fade_progress(bool requested, const model::Task* task,
                                  float visual_age)
{
    const float settle_start = status_timing::return_start
                             + status_timing::returning
                             + status_timing::collapse;
    if (!requested || !lamp_is_viewed(task) || visual_age < settle_start) return 0.f;
    return motion::ease_in_out_cubic(
        (visual_age - settle_start) / status_timing::settle);
}

inline float selection_visibility(bool active, float age, bool selected)
{
    if (!active) return 1.f;
    if (age < status_timing::rail_out)
        return 1.f - motion::ease_in_out_cubic(age / status_timing::rail_out);
    const float restore_start = status_timing::rail_out + status_timing::visual_life;
    if (age < restore_start || !selected) return 0.f;
    return motion::ease_in_out_cubic(
        (age - restore_start) / status_timing::rail_in);
}

}  // namespace status_animation
