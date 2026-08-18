#pragma once

#include <cstddef>
#include <cstdint>

namespace display_fade {

inline uint16_t swap16(uint16_t value)
{
    return static_cast<uint16_t>((value << 8) | (value >> 8));
}

// M5Canvas stores 16-bit sprites as byte-swapped RGB565. The paper and other
// genuinely light surfaces must remain light; dimming drives coloured and dark
// marks toward black so typography and structure gain contrast at low power.
inline uint16_t scale_swapped_rgb565(uint16_t raw, uint8_t level)
{
    if (level == 255) return raw;
    const uint16_t colour = swap16(raw);
    const uint16_t red5 = (colour >> 11) & 31;
    const uint16_t green6 = (colour >> 5) & 63;
    const uint16_t blue5 = colour & 31;
    const uint16_t red8 = static_cast<uint16_t>((red5 * 255 + 15) / 31);
    const uint16_t green8 = static_cast<uint16_t>((green6 * 255 + 31) / 63);
    const uint16_t blue8 = static_cast<uint16_t>((blue5 * 255 + 15) / 31);
    const uint16_t luminance = static_cast<uint16_t>(
        (red8 * 54 + green8 * 183 + blue8 * 19) >> 8);
    if (luminance >= 210) return raw;

    const uint16_t red = static_cast<uint16_t>(red5 * level / 255);
    const uint16_t green = static_cast<uint16_t>(green6 * level / 255);
    const uint16_t blue = static_cast<uint16_t>(blue5 * level / 255);
    return swap16(static_cast<uint16_t>((red << 11) | (green << 5) | blue));
}

inline void apply(void* buffer, std::size_t pixels, uint8_t level)
{
    if (buffer == nullptr || level == 255) return;
    auto* data = static_cast<uint16_t*>(buffer);
    for (std::size_t i = 0; i < pixels; ++i) {
        data[i] = scale_swapped_rgb565(data[i], level);
    }
}

}  // namespace display_fade
