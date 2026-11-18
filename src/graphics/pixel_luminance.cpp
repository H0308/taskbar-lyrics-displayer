#include "graphics/pixel_luminance.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace graphics {
namespace {
float srgbChannelToLinear(uint8_t value) {
    const float channel = static_cast<float>(value) / 255.0f;
    return channel <= 0.04045f ? channel / 12.92f
                              : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}
} // namespace

float backdropLuminance(const void* pixels, int width, int height) {
    return backdropLuminance(pixels, width, height, width);
}

float backdropLuminance(const void* pixels, int width, int height, int rowStridePixels) {
    if (!pixels || width <= 0 || height <= 0 || rowStridePixels < width)
        return 0.0f;
    const auto* bytes = static_cast<const uint8_t*>(pixels);
    const int step = std::max(1, std::min(width, height) / 96);
    double sumRoot = 0.0;
    int samples = 0;
    int darkPixels = 0;
    for (int y = 0; y < height; y += step) {
        for (int x = 0; x < width; x += step) {
            const auto* pixel = bytes + (static_cast<size_t>(y) * rowStridePixels + x) * 4;
            const float blue = srgbChannelToLinear(pixel[0]);
            const float green = srgbChannelToLinear(pixel[1]);
            const float red = srgbChannelToLinear(pixel[2]);
            const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            sumRoot += std::sqrt(std::max(0.0, luminance));
            if (luminance < 0.12)
                ++darkPixels;
            ++samples;
        }
    }
    if (samples <= 0)
        return 0.0f;
    const double count = static_cast<double>(samples);
    const double meanRoot = sumRoot / count;
    double effectiveRoot = meanRoot;
    if (darkPixels > 0 && darkPixels < samples) {
        const double darkRatio = static_cast<double>(darkPixels) / count;
        const double darkPenalty = std::clamp((darkRatio - 0.20) / 0.35, 0.0, 1.0);
        effectiveRoot = meanRoot * (1.0 - darkPenalty * 0.55);
    }
    return static_cast<float>(effectiveRoot * effectiveRoot);
}
} // namespace graphics
