#pragma once
#include <stdint.h>
#include "MatrixMapping.hpp"

namespace PixelChannelMapping {
inline uint32_t intensityOffset(uint32_t channelId, uint8_t bytesPerPixel,
                                uint32_t groupSize, const uint8_t colorOffsets[4],
                                uint16_t matrixWidth, uint16_t matrixHeight,
                                bool matrixEnabled, bool serpentine, bool vertical,
                                bool reverseX, bool reverseY, uint16_t rotation)
{
    if (!bytesPerPixel) return 0;
    uint32_t pixelId = channelId / bytesPerPixel;
    pixelId = MatrixMapping::map(pixelId, matrixWidth, matrixHeight, matrixEnabled,
                                 serpentine, vertical, reverseX, reverseY, rotation);
    const uint32_t colorIndex = channelId % bytesPerPixel;
    return pixelId * groupSize * bytesPerPixel + colorOffsets[colorIndex];
}
}
