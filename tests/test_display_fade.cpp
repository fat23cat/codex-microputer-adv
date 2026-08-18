#include "display_fade.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main()
{
    const uint16_t red = display_fade::swap16(0xf800);
    const uint16_t paper = display_fade::swap16(0xf79d);
    assert(display_fade::scale_swapped_rgb565(red, 255) == red);
    assert(display_fade::scale_swapped_rgb565(red, 0) == 0);
    assert(display_fade::scale_swapped_rgb565(paper, 0) == paper);

    const uint16_t half_red = display_fade::swap16(
        display_fade::scale_swapped_rgb565(red, 128));
    assert((half_red & 0x07ff) == 0);
    assert(((half_red >> 11) & 31) == 15);

    uint16_t pixels[] = {
        display_fade::swap16(0xffff), display_fade::swap16(0x07e0),
        display_fade::swap16(0x001f), display_fade::swap16(0xf800),
    };
    display_fade::apply(pixels, 4, 0);
    assert(pixels[0] == display_fade::swap16(0xffff));
    assert(pixels[1] == 0);
    assert(pixels[2] == 0);
    assert(pixels[3] == 0);

    std::puts("PASS display_fade (5 scenarios)");
}
