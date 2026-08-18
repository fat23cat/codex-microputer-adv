#include <cstdlib>
#include <iostream>

#include "adaptive_ble_power.h"

namespace {
void check(bool ok, const char* message)
{
    if (!ok) { std::cerr << "FAIL adaptive_ble_power: " << message << '\n'; std::exit(1); }
}
}

int main()
{
    check(adaptive_ble::next_tier(0, -45) == 0, "near link stays at 0 dBm");
    check(adaptive_ble::next_tier(0, -65) == 1, "weakening raises one tier");
    check(adaptive_ble::next_tier(3, -91) == 4, "very weak link reaches max tier gradually");
    check(adaptive_ble::next_tier(4, -65) == 3, "recovered link lowers one tier");
    check(adaptive_ble::next_tier(2, -65) == 2, "hysteresis prevents power chatter");
    check(adaptive_ble::wants_coded_phy(-86, false), "weak link requests coded PHY");
    check(adaptive_ble::wants_coded_phy(-80, true), "coded PHY stays through hysteresis");
    check(!adaptive_ble::wants_coded_phy(-70, true), "strong link returns to 1M PHY");
    check(adaptive_ble::weak_signal(-80, false), "weak indicator enters below threshold");
    check(adaptive_ble::weak_signal(-74, true), "weak indicator holds through hysteresis");
    check(!adaptive_ble::weak_signal(-68, true), "weak indicator clears after recovery");
    check(!adaptive_ble::tuning_ready(5999), "link tuning waits for controller settle");
    check(adaptive_ble::tuning_ready(6000), "link tuning starts on settle boundary");
    std::cout << "PASS adaptive_ble_power (13 scenarios)\n";
}
