#pragma once

#include <cstdint>

namespace session_sync {

enum class Method : uint8_t {
    LightingConfig = 1 << 0,
    ThreadStatus   = 1 << 1,
    DeviceStatus   = 1 << 2,
};

class Tracker {
public:
    void begin()
    {
        mask_ = 0;
        baseline_ = true;
    }

    void note(Method method)
    {
        mask_ |= static_cast<uint8_t>(method);
        constexpr uint8_t complete = static_cast<uint8_t>(Method::LightingConfig)
                                   | static_cast<uint8_t>(Method::ThreadStatus)
                                   | static_cast<uint8_t>(Method::DeviceStatus);
        if ((mask_ & complete) == complete) baseline_ = false;
    }

    bool baseline() const { return baseline_; }

private:
    uint8_t mask_ = 0;
    bool baseline_ = true;
};

}  // namespace session_sync
