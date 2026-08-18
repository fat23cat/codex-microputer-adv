#include <cmath>
#include <cstdlib>
#include <iostream>

#include "status_animation.h"

model::State model::state;

namespace {
int failures = 0;
void check(bool condition, const char* name)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL status_animation: " << name << '\n';
}

void green_never_visits_viewed_fade()
{
    model::Task task;
    task.status = model::Status::Done;
    task.color = 0x00ff4c;
    for (int frame = 0; frame <= 200; ++frame) {
        const float age = status_timing::visual_life * frame / 200.f;
        check(status_animation::viewed_fade_progress(true, &task, age) == 0.f,
              "green lamp has zero gray fade on every frame");
    }
}

void white_fade_is_monotonic_and_finishes()
{
    model::Task task;
    task.status = model::Status::Done;
    task.color = 0xffffff;
    float previous = 0.f;
    for (int frame = 0; frame <= 200; ++frame) {
        const float age = status_timing::visual_life * frame / 200.f;
        const float value = status_animation::viewed_fade_progress(true, &task, age);
        check(value + 0.0001f >= previous, "white fade is monotonic");
        previous = value;
    }
    check(std::fabs(previous - 1.f) < 0.0001f, "white fade reaches viewed color");
}

void selection_rail_has_continuous_endpoints()
{
    check(std::fabs(status_animation::selection_visibility(true, 0.f, true) - 1.f)
              < 0.0001f,
          "rail begins fully visible");
    const float restore = status_timing::rail_out + status_timing::visual_life;
    check(status_animation::selection_visibility(true, restore, true) == 0.f,
          "rail restore begins below screen");
    const float end = status_timing::life;
    check(std::fabs(status_animation::selection_visibility(true, end, true) - 1.f)
              < 0.0001f,
          "rail reaches exact resting visibility");
    check(status_animation::selection_visibility(false, end, true) == 1.f,
          "first deck frame matches final rail visibility");
}
}  // namespace

int main()
{
    green_never_visits_viewed_fade();
    white_fade_is_monotonic_and_finishes();
    selection_rail_has_continuous_endpoints();
    if (failures) return EXIT_FAILURE;
    std::cout << "PASS status_animation (3 scenarios, 403 sampled frames)\n";
    return EXIT_SUCCESS;
}
