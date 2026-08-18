#pragma once

#include <algorithm>
#include <cstdint>

namespace adaptive_ble {

// Five hardware levels from quiet nearby operation to maximum ESP32-S3 range.
constexpr int kDbm[5] = {0, 3, 6, 12, 20};
constexpr int kRaiseBelow[4] = {-58, -68, -78, -86};
constexpr int kLowerAbove[4] = {-52, -62, -72, -80};
constexpr uint32_t kLinkSettleMs = 6000;

inline bool tuning_ready(uint32_t connected_ms)
{
    return connected_ms >= kLinkSettleMs;
}

inline uint8_t next_tier(uint8_t current, int rssi)
{
    current = std::min<uint8_t>(current, 4);
    if (current < 4 && rssi < kRaiseBelow[current]) return current + 1;
    if (current > 0 && rssi > kLowerAbove[current - 1]) return current - 1;
    return current;
}

inline bool wants_coded_phy(int rssi, bool coded_now)
{
    // Separate enter/exit thresholds prevent repeated PHY negotiations at the
    // edge of a room. S2 retains useful throughput for Codex RPC.
    return coded_now ? rssi < -76 : rssi < -84;
}

inline bool weak_signal(int rssi, bool weak_now)
{
    return weak_now ? rssi < -72 : rssi < -78;
}

}  // namespace adaptive_ble
