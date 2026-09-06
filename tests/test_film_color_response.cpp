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

    return test::finish(passed, "film colour response");
}
