// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmcolorresponse.h"
#include "test_common.h"

#include <cmath>

namespace {

double
normalized_chroma(
    const FilmDensity& density,
    const FilmDensity& minimum)
{
    const double values[3] = {
        density.red - minimum.red,
        density.green - minimum.green,
        density.blue - minimum.blue
    };
    const double neutral =
        (values[0] + values[1] + values[2]) / 3.0;
    return std::sqrt(
        ((values[0] - neutral) * (values[0] - neutral)
         + (values[1] - neutral) * (values[1] - neutral)
         + (values[2] - neutral) * (values[2] - neutral))
        / 3.0);
}

double
warm_alignment(
    const FilmDensity& density,
    const FilmDensity& minimum)
{
    const double values[3] = {
        density.red - minimum.red,
        density.green - minimum.green,
        density.blue - minimum.blue
    };
    const double neutral =
        (values[0] + values[1] + values[2]) / 3.0;
    const double chroma[3] = {
        values[0] - neutral,
        values[1] - neutral,
        values[2] - neutral
    };
    const double length =
        std::sqrt(
            chroma[0] * chroma[0]
            + chroma[1] * chroma[1]
            + chroma[2] * chroma[2]);
    return length > 1e-12
        ? (0.40824829 * chroma[0]
           + 0.40824829 * chroma[1]
           - 0.81649658 * chroma[2]) / length
        : 0.0;
}

} // namespace

int
main()
{
    const FilmDensity minimum = {0.1f, 0.2f, 0.3f};
    const FilmDensity reference = {1.1f, 1.2f, 1.3f};
    FilmColorResponse response(minimum, reference);
    bool passed = test::check(response.valid(), "model is valid");
    passed &= test::near(
        FilmColorResponse::amount_from_trim(-4.0f),
        0.0,
        1e-7,
        "-4 trim maps to calibrated bypass");
    passed &= test::near(
        FilmColorResponse::amount_from_trim(0.0f),
        1.5,
        1e-7,
        "zero trim maps to the standard response");
    passed &= test::near(
        FilmColorResponse::amount_from_trim(4.0f),
        3.0,
        1e-7,
        "+4 trim maps to twice the standard response");

    FilmColorResponse::Settings bypass;
    bypass.amount = 0.0f;
    const FilmDensity colour = {1.3f, 1.1f, 1.2f};
    const FilmDensity bypassed = response.apply(colour, bypass);
    passed &= test::check(
        bypassed.red == colour.red
            && bypassed.green == colour.green
            && bypassed.blue == colour.blue,
        "zero amount is an exact bypass");

    FilmColorResponse::Settings enabled;
    enabled.amount = 1.0f;
    const FilmDensity neutral = {0.8f, 0.9f, 1.0f};
    const FilmDensity shaped_neutral = response.apply(neutral, enabled);
    passed &= test::near(shaped_neutral.red, neutral.red, 1e-7, "neutral red");
    passed &= test::near(shaped_neutral.green, neutral.green, 1e-7, "neutral green");
    passed &= test::near(shaped_neutral.blue, neutral.blue, 1e-7, "neutral blue");

    const FilmDensity moderate = response.apply(colour, enabled);
    passed &= test::check(
        normalized_chroma(moderate, minimum)
            < normalized_chroma(colour, minimum),
        "moderate colour is compressed");
    const double input_neutral =
        ((colour.red - minimum.red)
         + (colour.green - minimum.green)
         + (colour.blue - minimum.blue))
        / 3.0;
    const double shaped_neutral_value =
        ((moderate.red - minimum.red)
         + (moderate.green - minimum.green)
         + (moderate.blue - minimum.blue))
        / 3.0;
    passed &= test::check(
        shaped_neutral_value < input_neutral,
        "chromatic colour receives negative-density depth compensation");

    const FilmDensity extreme = {3.1f, 0.2f, 0.3f};
    const FilmDensity shaped_extreme = response.apply(extreme, enabled);
    passed &= test::check(
        normalized_chroma(shaped_extreme, minimum)
            < normalized_chroma(extreme, minimum),
        "extreme colour is compressed");

    FilmColorResponse::Settings uniform = enabled;
    uniform.warm_tone_separation = 0.0f;
    const FilmDensity warm = {1.15f, 1.25f, 0.95f};
    const FilmDensity uniform_warm = response.apply(warm, uniform);
    const FilmDensity protected_warm = response.apply(warm, enabled);
    passed &= test::check(
        normalized_chroma(protected_warm, minimum)
            > normalized_chroma(uniform_warm, minimum),
        "standard warm separation retains more mid-density chroma");
    passed &= test::check(
        normalized_chroma(protected_warm, minimum)
            < normalized_chroma(warm, minimum),
        "standard warm separation remains inside calibrated separation");

    const FilmDensity bent_warm = {1.30f, 1.00f, 0.90f};
    const FilmDensity uniform_bent_warm =
        response.apply(bent_warm, uniform);
    const FilmDensity guided_bent_warm =
        response.apply(bent_warm, enabled);
    passed &= test::check(
        warm_alignment(guided_bent_warm, minimum)
            > warm_alignment(uniform_bent_warm, minimum),
        "standard warm separation guides a near-warm hue toward yellow");

    const FilmDensity cool = {0.5f, 0.6f, 1.7f};
    const FilmDensity uniform_cool = response.apply(cool, uniform);
    const FilmDensity protected_cool = response.apply(cool, enabled);
    passed &= test::near(
        protected_cool.red,
        uniform_cool.red,
        1e-7,
        "cool red coordinate is not protected");
    passed &= test::near(
        protected_cool.green,
        uniform_cool.green,
        1e-7,
        "cool green coordinate is not protected");
    passed &= test::near(
        protected_cool.blue,
        uniform_cool.blue,
        1e-7,
        "cool blue coordinate is not protected");

    FilmColorResponse::Settings maximum_warm = enabled;
    maximum_warm.warm_tone_separation = 2.0f;
    const FilmDensity fully_protected_warm =
        response.apply(warm, maximum_warm);
    passed &= test::check(
        normalized_chroma(fully_protected_warm, minimum)
            > normalized_chroma(protected_warm, minimum),
        "maximum warm separation retains more than standard");

    const FilmDensity extreme_warm = {2.5f, 2.6f, 0.3f};
    const FilmDensity uniform_extreme_warm =
        response.apply(extreme_warm, uniform);
    const FilmDensity protected_extreme_warm =
        response.apply(extreme_warm, maximum_warm);
    passed &= test::near(
        protected_extreme_warm.red,
        uniform_extreme_warm.red,
        1e-7,
        "extreme warm red rejoins outer compression");
    passed &= test::near(
        protected_extreme_warm.green,
        uniform_extreme_warm.green,
        1e-7,
        "extreme warm green rejoins outer compression");
    passed &= test::near(
        protected_extreme_warm.blue,
        uniform_extreme_warm.blue,
        1e-7,
        "extreme warm blue rejoins outer compression");

    return test::finish(passed, "film colour response");
}
