#include <cstdlib>
#include <iostream>

#include "model.h"

model::State model::state;

namespace {
int failures = 0;
void check(bool condition, const char* name)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL announcement_queue: " << name << '\n';
}

void same_slot_replaces_and_restarts_debounce()
{
    model::state = model::State{};
    model::state.status_debounce_ms = 100;
    model::queue_announcement(2, model::Status::Done, model::Status::Running,
                              true, true, false, 1000);
    model::queue_announcement(2, model::Status::Running, model::Status::Running,
                              false, false, false, 1060);
    check(model::state.announcement_count == 1, "same slot has one pending event");
    const auto& event = model::state.announcements[0];
    check(event.status == model::Status::Running, "latest status replaces prior status");
    check(event.previous_status == model::Status::Running,
          "replacement preserves original visual start");
    check(event.ready_at_ms == 1160, "replacement restarts debounce window");
    model::Announcement out;
    check(!model::take_next_announcement(1159, out), "event is unavailable before debounce");
    check(model::take_next_announcement(1160, out), "event becomes available on boundary");
    check(out.status == model::Status::Running, "taken event is latest state");
}

void different_slots_remain_fifo()
{
    model::state = model::State{};
    model::state.status_debounce_ms = 100;
    model::queue_announcement(4, model::Status::Done, model::Status::Running,
                              true, true, false, 1000);
    model::queue_announcement(1, model::Status::NeedsInput, model::Status::Running,
                              false, false, false, 1010);
    model::Announcement out;
    check(model::take_next_announcement(1110, out) && out.slot == 4,
          "different slots keep arrival order");
    check(model::take_next_announcement(1110, out) && out.slot == 1,
          "second slot follows first");
}
}  // namespace

int main()
{
    same_slot_replaces_and_restarts_debounce();
    different_slots_remain_fifo();
    if (failures) return EXIT_FAILURE;
    std::cout << "PASS announcement_queue (2 scenarios)\n";
    return EXIT_SUCCESS;
}
