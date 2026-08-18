#include <cstdlib>
#include <iostream>

#include "session_sync.h"

namespace {
int failures = 0;
void check(bool condition, const char* name)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL session_sync: " << name << '\n';
}
}

int main()
{
    session_sync::Tracker sync;
    check(sync.baseline(), "cold boot starts as baseline");
    sync.note(session_sync::Method::ThreadStatus);
    sync.note(session_sync::Method::ThreadStatus);
    check(sync.baseline(), "intermediate thread snapshots stay baseline");
    sync.note(session_sync::Method::DeviceStatus);
    check(sync.baseline(), "partial handshake stays baseline");
    sync.note(session_sync::Method::LightingConfig);
    check(!sync.baseline(), "complete handshake enables live events");
    sync.begin();
    check(sync.baseline(), "real session loss starts a new baseline");
    if (failures) return EXIT_FAILURE;
    std::cout << "PASS session_sync (5 scenarios)\n";
    return EXIT_SUCCESS;
}
