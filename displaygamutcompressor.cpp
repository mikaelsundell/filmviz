// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "displaygamutcompressor.h"

#include <algorithm>
#include <cmath>

std::array<float, 3>
DisplayGamutCompressor::compress_rec709(const std::array<float, 3>& linear_rgb)
{
    constexpr float knee = 0.1f;

    const float luminance = 0.2126390059f * linear_rgb[0] + 0.7151686788f * linear_rgb[1]
                            + 0.0721923154f * linear_rgb[2];

    if (!std::isfinite(luminance) || luminance <= 1e-6f || luminance >= 1.0f - 1e-6f) {
        return linear_rgb;
    }

    float scale = 1.0f;

    const float minimum = std::min({ linear_rgb[0], linear_rgb[1], linear_rgb[2] });
    const float lower_margin = minimum / luminance;

    if (lower_margin < knee) {
        const float mapped_margin = knee * std::exp((lower_margin - knee) / knee);
        scale = std::min(scale, (1.0f - mapped_margin) / (1.0f - lower_margin));
    }

    const float maximum = std::max({ linear_rgb[0], linear_rgb[1], linear_rgb[2] });
    const float upper_margin = (1.0f - maximum) / (1.0f - luminance);

    if (upper_margin < knee) {
        const float mapped_margin = knee * std::exp((upper_margin - knee) / knee);
        scale = std::min(scale, (1.0f - mapped_margin) / (1.0f - upper_margin));
    }

    std::array<float, 3> result = linear_rgb;
    for (int channel = 0; channel < 3; ++channel) {
        result[channel] = luminance + scale * (linear_rgb[channel] - luminance);
    }
    return result;
}
