#include <cstdlib>
#include <iostream>
#include "input_event_queue.h"
namespace {
struct Event {
    int id = 0;
    bool down = true;
    bool repeatable = false;
};
int failures = 0;
void check(bool ok, const char* name)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL input_event_queue: " << name << '\n';
    }
}
auto is_release = [](const Event& e) { return !e.down; };
auto droppable = [](const Event& e) { return e.down && e.repeatable; };
void release_survives_full_queue()
{
    input_event_queue::Queue<Event, 3> q;
    q.push({1, true, false}, false, is_release, droppable);
    q.push({2, true, true}, false, is_release, droppable);
    q.push({3, true, false}, false, is_release, droppable);
    check(q.push({2, false, false}, false, is_release, droppable), "release accepted when full");
    Event e;
    bool saw = false;
    while (q.pop(e))
        saw |= e.id == 2 && !e.down;
    check(saw, "release retained");
}
void repeat_drops_first()
{
    input_event_queue::Queue<Event, 2> q;
    q.push({1, true, false}, false, is_release, droppable);
    q.push({2, true, false}, false, is_release, droppable);
    check(!q.push({3, true, true}, true, is_release, droppable), "repeat dropped");
    check(q.size() == 2, "depth unchanged");
}
void fifo()
{
    input_event_queue::Queue<Event, 4> q;
    q.push({1}, false, is_release, droppable);
    q.push({2}, false, is_release, droppable);
    Event a, b;
    check(q.pop(a) && q.pop(b) && a.id == 1 && b.id == 2, "fifo order");
}
} // namespace
int main()
{
    release_survives_full_queue();
    repeat_drops_first();
    fifo();
    if (failures)
        return EXIT_FAILURE;
    std::cout << "PASS input_event_queue (3 scenarios)\n";
}
