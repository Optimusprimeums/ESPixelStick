#pragma once
#include <stdint.h>

// Production matrix coordinate transform, shared by firmware and host tests.
// Configuration validation is performed by c_OutputPixel::validate().
namespace MatrixMapping {
inline uint32_t map(uint32_t id, uint16_t width, uint16_t height,
                    bool enabled, bool serpentine, bool vertical,
                    bool reverseX, bool reverseY, uint16_t rotation)
{
    if (!enabled || !width || !height) return id;
    uint32_t x = id % width, y = id / width;
    if ((rotation == 90 || rotation == 270) && width == height) {
        uint32_t ox = x;
        if (rotation == 90) { x = width - 1 - y; y = ox; }
        else { x = y; y = height - 1 - ox; }
    } else if (rotation == 180) {
        x = width - 1 - x; y = height - 1 - y;
    }
    if (reverseX) x = width - 1 - x;
    if (reverseY) y = height - 1 - y;
    if (vertical) {
        if (serpentine && (x & 1U)) y = height - 1 - y;
        return x * height + y;
    }
    if (serpentine && (y & 1U)) x = width - 1 - x;
    return y * width + x;
}
}
