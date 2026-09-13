// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "displaygamutcompressor.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

double
luminance(
    const std::array<float, 3>& rgb)
{
    return
        0.2126390059 * rgb[0]
        + 0.7151686788 * rgb[1]
        + 0.0721923154 * rgb[2];
}

} // namespace

int
main()
{
    bool passed = true;

    const std::array<float, 3> neutral = {{0.25f, 0.25f, 0.25f}};
    const std::array<float, 3> neutral_result =
        DisplayGamutCompressor::compress_rec709(
            neutral);

    for (int channel = 0; channel < 3; ++channel) {
        passed &= test::near(
            neutral_result[channel],
            neutral[channel],
            1e-7,
            "display gamut compression preserves neutrals");
    }

    const std::array<float, 3> in_gamut = {{0.2f, 0.3f, 0.4f}};
    const std::array<float, 3> in_gamut_result =
        DisplayGamutCompressor::compress_rec709(
            in_gamut);

    for (int channel = 0; channel < 3; ++channel) {
        passed &= test::near(
            in_gamut_result[channel],
            in_gamut[channel],
            1e-7,
            "display gamut compression leaves interior colours unchanged");
    }

    const std::array<float, 3> out_of_gamut = {{0.48f, 0.19f, -0.01f}};
    const std::array<float, 3> compressed =
        DisplayGamutCompressor::compress_rec709(
            out_of_gamut);

    passed &= test::near(
        luminance(compressed),
        luminance(out_of_gamut),
        1e-7,
        "display gamut compression preserves Rec.709 luminance");

    passed &= test::check(
        *std::min_element(compressed.begin(), compressed.end()) > 0.0f,
        "display gamut compression moves a negative channel inside gamut");

    const double original_luminance =
        luminance(out_of_gamut);
    const double red_scale =
        (compressed[0] - original_luminance)
        / (out_of_gamut[0] - original_luminance);
    const double blue_scale =
        (compressed[2] - original_luminance)
        / (out_of_gamut[2] - original_luminance);

    passed &= test::near(
        red_scale,
        blue_scale,
        1e-6,
        "display gamut compression preserves the linear-RGB hue direction");

    const std::array<float, 3> above_gamut = {{1.05f, 0.7f, 0.4f}};
    const std::array<float, 3> upper_compressed =
        DisplayGamutCompressor::compress_rec709(
            above_gamut);

    passed &= test::near(
        luminance(upper_compressed),
        luminance(above_gamut),
        1e-7,
        "upper display gamut compression preserves Rec.709 luminance");

    passed &= test::check(
        *std::max_element(upper_compressed.begin(), upper_compressed.end()) < 1.0f,
        "display gamut compression moves an over-range channel inside gamut");

    return
        test::finish(
            passed,
            "display gamut compression");
}
