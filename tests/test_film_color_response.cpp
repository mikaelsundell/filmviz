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

    FilmColorResponse::Settings separation_only = enabled;
    separation_only.color_depth = 0.0f;
    const FilmDensity separated = response.apply(colour, separation_only);
    const double separated_neutral_value =
        ((separated.red - minimum.red)
         + (separated.green - minimum.green)
         + (separated.blue - minimum.blue))
        / 3.0;
    passed &= test::near(
        separated_neutral_value,
        input_neutral,
        1e-7,
        "zero color depth preserves the neutral coordinate");
    passed &= test::check(
        normalized_chroma(separated, minimum)
            < normalized_chroma(colour, minimum),
        "zero color depth retains chroma separation shaping");

    FilmColorResponse::Settings lifted = enabled;
    lifted.color_depth = -1.0f;
    const FilmDensity lifted_colour = response.apply(colour, lifted);
    const double lifted_neutral_value =
        ((lifted_colour.red - minimum.red)
         + (lifted_colour.green - minimum.green)
         + (lifted_colour.blue - minimum.blue))
        / 3.0;
    passed &= test::check(
        lifted_neutral_value > input_neutral,
        "negative color depth lifts the neutral coordinate of chromatic colour");

    const FilmDensity extreme = {3.1f, 0.2f, 0.3f};
    const FilmDensity shaped_extreme = response.apply(extreme, enabled);
    passed &= test::check(
        normalized_chroma(shaped_extreme, minimum)
            < normalized_chroma(extreme, minimum),
        "extreme colour is compressed");

    const FilmDensity warm = {1.15f, 1.25f, 0.95f};
    const FilmDensity protected_warm = response.apply(warm, enabled);
    passed &= test::check(
        normalized_chroma(protected_warm, minimum)
            < normalized_chroma(warm, minimum),
        "built-in warm shaping remains inside calibrated separation");

    const FilmDensity bent_warm = {1.30f, 1.00f, 0.90f};
    const FilmDensity guided_bent_warm =
        response.apply(bent_warm, enabled);
    passed &= test::check(
        warm_alignment(guided_bent_warm, minimum)
            > warm_alignment(bent_warm, minimum),
        "built-in warm shaping guides a near-warm hue toward yellow");

    const FilmDensity extreme_warm = {2.5f, 2.6f, 0.3f};
    const FilmDensity protected_extreme_warm =
        response.apply(extreme_warm, enabled);
    passed &= test::check(
        normalized_chroma(protected_extreme_warm, minimum)
            < normalized_chroma(extreme_warm, minimum),
        "extreme warm colour rejoins outer compression");

    // The new master amount includes hue guidance, unlike the legacy trim.
    auto tuning = enabled;
    tuning.tuning.response_amount = 0.0f;
    const auto disabled = response.apply(bent_warm, tuning);
    passed &= test::near(disabled.red, bent_warm.red, 0.0, "master bypass preserves red exactly");
    passed &= test::near(disabled.green, bent_warm.green, 0.0, "master bypass preserves green exactly");
    passed &= test::near(disabled.blue, bent_warm.blue, 0.0, "master bypass preserves blue exactly");
    tuning.tuning.response_amount = 0.5f;
    const auto half = response.apply(bent_warm, tuning);
    passed &= test::near(half.red, (bent_warm.red + guided_bent_warm.red) * 0.5f, 1e-6,
        "master amount interpolates complete response");

    tuning = enabled;
    tuning.color_depth = 0.0f;
    tuning.tuning.chroma_compression = 0.0f;
    const auto unrotated = response.apply(bent_warm, tuning);
    tuning.tuning.warm_hue_shift = 30.0f;
    const auto rotated = response.apply(bent_warm, tuning);
    passed &= test::check(rotated.red > unrotated.red && rotated.green < unrotated.green,
        "positive warm rotation moves toward the red dye direction");
    passed &= test::near(normalized_chroma(rotated, minimum), normalized_chroma(unrotated, minimum), 1e-6,
        "hue rotation preserves chroma magnitude");
    passed &= test::near(rotated.red + rotated.green + rotated.blue,
        unrotated.red + unrotated.green + unrotated.blue, 1e-6, "hue rotation preserves common density");
    tuning.tuning.warm_hue_center = 45.0f;
    tuning.tuning.density_center = 0.8f;
    tuning.tuning.density_width = 1.5f;
    const auto tuned_neutral = response.apply(reference, tuning);
    passed &= test::near(tuned_neutral.red, reference.red, 1e-6, "tuning preserves neutral red");
    passed &= test::near(tuned_neutral.green, reference.green, 1e-6, "tuning preserves neutral green");
    passed &= test::near(tuned_neutral.blue, reference.blue, 1e-6, "tuning preserves neutral blue");
    tuning.tuning.chroma_knee = 0.0f;
    passed &= test::check(!FilmColorResponse::valid_tuning(tuning.tuning), "zero knee is rejected");

    return test::finish(passed, "film colour response");
}
