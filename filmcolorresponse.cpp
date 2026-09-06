// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmcolorresponse.h"

#include <algorithm>
#include <cmath>

namespace {

float
smoothstep(
    float edge0,
    float edge1,
    float value)
{
    const float t =
        std::clamp(
            (value - edge0) / (edge1 - edge0),
            0.0f,
            1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

FilmColorResponse::FilmColorResponse(
    const FilmDensity& minimum_coordinate,
    const FilmDensity& neutral_reference_coordinate)
    : minimum_(minimum_coordinate)
{
    increment_.red =
        neutral_reference_coordinate.red - minimum_.red;
    increment_.green =
        neutral_reference_coordinate.green - minimum_.green;
    increment_.blue =
        neutral_reference_coordinate.blue - minimum_.blue;

    valid_ =
        std::isfinite(increment_.red)
        && std::isfinite(increment_.green)
        && std::isfinite(increment_.blue)
        && increment_.red > 1e-6f
        && increment_.green > 1e-6f
        && increment_.blue > 1e-6f;
}

bool
FilmColorResponse::valid() const
{
    return valid_;
}

float
FilmColorResponse::amount_from_trim(float trim)
{
    const float bounded =
        std::clamp(
            trim,
            minimum_trim,
            maximum_trim);
    return
        standard_amount
        * (1.0f + bounded / 4.0f);
}

FilmDensity
FilmColorResponse::apply(
    const FilmDensity& coordinate,
    const Settings& settings) const
{
    if (!valid_
        || !std::isfinite(settings.amount)
        || settings.amount <= 0.0f
        || !std::isfinite(settings.chroma_compression)
        || !std::isfinite(settings.density_depth)
        || !std::isfinite(settings.chroma_knee)
        || settings.chroma_knee <= 1e-6f) {
        return coordinate;
    }

    const float normalized[3] = {
        (coordinate.red - minimum_.red) / increment_.red,
        (coordinate.green - minimum_.green) / increment_.green,
        (coordinate.blue - minimum_.blue) / increment_.blue
    };
    const float neutral =
        (normalized[0] + normalized[1] + normalized[2]) / 3.0f;
    const float chroma[3] = {
        normalized[0] - neutral,
        normalized[1] - neutral,
        normalized[2] - neutral
    };
    const float magnitude =
        std::sqrt(
            (chroma[0] * chroma[0]
             + chroma[1] * chroma[1]
             + chroma[2] * chroma[2])
            / 3.0f);

    // Compress channel differences continuously from moderate through extreme
    // colour. A small common-coordinate reduction proportional to chroma sends
    // more light to the print stage, producing denser viewed colour rather than
    // merely moving saturated colours toward grey. Hue direction is unchanged
    // in normalized dye-coordinate space.
    const float density_envelope =
        smoothstep(0.0f, 0.20f, neutral)
        * (1.0f - smoothstep(1.75f, 2.50f, neutral));
    const float compression =
        std::max(0.0f, settings.chroma_compression)
        * settings.amount
        * density_envelope;
    const float knee_position =
        std::max(1e-6f, settings.chroma_knee);
    const float knee_ratio = magnitude / knee_position;
    const float scale =
        1.0f
        / (1.0f + compression * knee_ratio);
    const float depth =
        std::max(0.0f, settings.density_depth)
        * settings.amount
        * density_envelope
        * magnitude
        / (magnitude + knee_position);
    const float shaped_neutral = neutral - depth;

    FilmDensity result;
    result.red =
        std::max(
            minimum_.red,
            minimum_.red
                + (shaped_neutral + scale * chroma[0]) * increment_.red);
    result.green =
        std::max(
            minimum_.green,
            minimum_.green
                + (shaped_neutral + scale * chroma[1]) * increment_.green);
    result.blue =
        std::max(
            minimum_.blue,
            minimum_.blue
                + (shaped_neutral + scale * chroma[2]) * increment_.blue);
    return result;
}
