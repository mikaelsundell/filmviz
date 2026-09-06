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
        || !std::isfinite(settings.warm_tone_separation)
        || settings.warm_tone_separation
            < minimum_warm_tone_separation
        || settings.warm_tone_separation
            > maximum_warm_tone_separation
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
    // merely moving saturated colours toward grey. This general stage is
    // radial in normalized dye-coordinate space; the narrowly gated warm
    // guidance is applied separately below.
    const float density_envelope =
        smoothstep(0.0f, 0.20f, neutral)
        * (1.0f - smoothstep(1.75f, 2.50f, neutral));
    float compression =
        std::max(0.0f, settings.chroma_compression)
        * settings.amount
        * density_envelope;

    // Warm skin-like records occupy the broad yellow/red direction in the
    // normalized dye-coordinate plane: red and green rise together relative
    // to blue. Protect that direction through ordinary midscale densities,
    // then taper the protection at neutral and extreme chroma. This retains a
    // continuous warm-tone branch without exempting saturated reds from the
    // outer colour-density roll-off. It is an empirical colour-separation
    // control, not a face detector or a claim of measured interimage chemistry.
    const float chroma_length =
        std::sqrt(
            chroma[0] * chroma[0]
            + chroma[1] * chroma[1]
            + chroma[2] * chroma[2]);
    float warm_direction = 0.0f;
    if (chroma_length > 1e-6f) {
        constexpr float warm_red_green = 0.40824829f;
        constexpr float warm_blue = -0.81649658f;
        warm_direction =
            (warm_red_green * chroma[0]
             + warm_red_green * chroma[1]
             + warm_blue * chroma[2])
            / chroma_length;
    }
    const float warm_hue =
        smoothstep(0.15f, 0.90f, warm_direction);
    const float warm_density =
        smoothstep(0.30f, 0.60f, neutral)
        * (1.0f - smoothstep(1.40f, 2.00f, neutral));
    const float warm_chroma =
        smoothstep(0.02f, 0.08f, magnitude)
        * (1.0f - smoothstep(0.35f, 0.75f, magnitude));
    const float warm_protection =
        std::clamp(
            0.5f * settings.warm_tone_separation,
            0.0f,
            1.0f)
        * warm_hue
        * warm_density
        * warm_chroma;
    compression *= 1.0f - warm_protection;

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

    // Guide only the protected warm lobe gently toward the yellow/orange
    // axis. Renormalizing the mixed direction keeps this separate from the
    // radial compression above: it prevents a near-skin trajectory from
    // curling toward magenta without turning true magenta objects into skin.
    const float warm_guidance =
        std::clamp(
            0.15f * settings.warm_tone_separation,
            0.0f,
            0.30f)
        * warm_hue
        * warm_density
        * warm_chroma;
    float guided_chroma[3] = {
        chroma[0],
        chroma[1],
        chroma[2]
    };
    if (warm_guidance > 0.0f && chroma_length > 1e-6f) {
        const float target[3] = {
            0.40824829f * chroma_length,
            0.40824829f * chroma_length,
            -0.81649658f * chroma_length
        };
        float mixed[3];
        float mixed_length_squared = 0.0f;
        for (int channel = 0; channel < 3; ++channel) {
            mixed[channel] =
                (1.0f - warm_guidance) * chroma[channel]
                + warm_guidance * target[channel];
            mixed_length_squared += mixed[channel] * mixed[channel];
        }
        const float mixed_length = std::sqrt(mixed_length_squared);
        if (mixed_length > 1e-6f) {
            const float normalization = chroma_length / mixed_length;
            for (int channel = 0; channel < 3; ++channel) {
                guided_chroma[channel] =
                    mixed[channel] * normalization;
            }
        }
    }

    FilmDensity result;
    result.red =
        std::max(
            minimum_.red,
            minimum_.red
                + (shaped_neutral + scale * guided_chroma[0]) * increment_.red);
    result.green =
        std::max(
            minimum_.green,
            minimum_.green
                + (shaped_neutral + scale * guided_chroma[1]) * increment_.green);
    result.blue =
        std::max(
            minimum_.blue,
            minimum_.blue
                + (shaped_neutral + scale * guided_chroma[2]) * increment_.blue);
    return result;
}
