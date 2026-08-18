#include <cstdlib>
#include <iostream>

#include "status_reducer.h"

model::State model::state;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL status_reducer: " << name << '\n';
}

status_reducer::LampFrame lamp(uint32_t color, uint8_t effect = 1,
                               float brightness = 1.f)
{
    return {color, brightness, 0.f, effect};
}

void initial_sync_uses_lamp_color_without_event()
{
    model::Task viewed;
    auto result = status_reducer::apply(viewed, lamp(0xffffff));
    check(result.initial_sync, "viewed initial frame is sync");
    check(!result.changed, "viewed initial frame has no event");
    check(viewed.status == model::Status::Done, "viewed initial status is done");
    check(!viewed.unseen_done, "white initial lamp is gray/viewed");
    check(!viewed.completion_hold, "initial done has no permanent hold");

    model::Task unread;
    result = status_reducer::apply(unread, lamp(0x00ff4c));
    check(unread.unseen_done, "green initial lamp is unread");
    check(!result.changed, "green initial frame has no event");
}

void multi_frame_session_baseline_never_becomes_an_event()
{
    model::Task task;
    status_reducer::apply(task, lamp(0x304ffe), true);
    auto result = status_reducer::apply(task, lamp(0xff6d00), true);
    check(result.initial_sync, "second baseline frame is still sync");
    check(!result.changed, "second baseline frame has no event");
    result = status_reducer::apply(task, lamp(0xffffff), true);
    check(!result.changed, "final baseline frame has no event");
    check(!task.completion_hold, "baseline completion has no local hold");
    check(!task.unseen_done, "white baseline completion is viewed");
}

void fresh_completion_holds_green_until_animation_finishes()
{
    model::Task task;
    status_reducer::apply(task, lamp(0x304ffe));
    auto result = status_reducer::apply(task, lamp(0xffffff));
    check(result.changed, "running to done is an event");
    check(result.target_unseen == false, "white lamp target is viewed");
    check(task.unseen_done, "fresh completion is green during animation");
    check(task.completion_hold, "fresh completion owns temporary hold");
}

void repeated_and_restored_frames_are_silent()
{
    model::Task task;
    status_reducer::apply(task, lamp(0x304ffe));
    auto same = status_reducer::apply(task, lamp(0x304ffe));
    check(!same.changed, "identical running frame is silent");

    auto off = status_reducer::apply(task, lamp(0x304ffe, 0, 0.f));
    check(!off.changed, "off presentation frame is silent");
    check(task.status == model::Status::Running, "off frame preserves semantic status");

    auto restored = status_reducer::apply(task, lamp(0x304ffe));
    check(restored.restoration, "on frame is identified as restoration");
    check(!restored.changed, "restoration does not replay event");
}

void latest_lamp_color_remains_authoritative()
{
    model::Task task;
    status_reducer::apply(task, lamp(0x304ffe));
    status_reducer::apply(task, lamp(0x00ff4c));
    task.completion_hold = false;  // animation finalizer
    status_reducer::apply(task, lamp(0x00ff4c));
    check(task.unseen_done, "green remains green after animation");
    status_reducer::apply(task, lamp(0xffffff));
    check(!task.unseen_done, "white becomes viewed after hold release");
}

}  // namespace

int main()
{
    initial_sync_uses_lamp_color_without_event();
    multi_frame_session_baseline_never_becomes_an_event();
    fresh_completion_holds_green_until_animation_finishes();
    repeated_and_restored_frames_are_silent();
    latest_lamp_color_remains_authoritative();
    if (failures) return EXIT_FAILURE;
    std::cout << "PASS status_reducer (5 scenarios)\n";
    return EXIT_SUCCESS;
}
