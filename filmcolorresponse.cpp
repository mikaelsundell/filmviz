// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmcolorresponse.h"

#include <algorithm>
#include <cmath>

namespace {

float
smoothstep(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

bool
FilmColorResponse::Tuning::operator==(const Tuning& other) const
{
    return response_amount == other.response_amount
        && chroma_compression == other.chroma_compression
        && chroma_knee == other.chroma_knee
        && density_center == other.density_center
        && density_width == other.density_width
        && warm_protection == other.warm_protection
        && warm_hue_center == other.warm_hue_center
        && warm_hue_width == other.warm_hue_width
        && warm_hue_shift == other.warm_hue_shift;
}

bool
FilmColorResponse::valid_tuning(const Tuning& t)
{
    return (std::isfinite(t.response_amount) && t.response_amount >= 0.0f && t.response_amount <= 1.0f)
        && (std::isfinite(t.chroma_compression) && t.chroma_compression >= 0.0f && t.chroma_compression <= 1.0f)
        && (std::isfinite(t.chroma_knee) && t.chroma_knee >= 0.05f && t.chroma_knee <= 2.0f)
        && (std::isfinite(t.density_center) && t.density_center >= 0.0f && t.density_center <= 3.0f)
        && (std::isfinite(t.density_width) && t.density_width >= 0.25f && t.density_width <= 3.0f)
        && (std::isfinite(t.warm_protection) && t.warm_protection >= 0.0f && t.warm_protection <= 1.0f)
        && (std::isfinite(t.warm_hue_center) && t.warm_hue_center >= -180.0f && t.warm_hue_center <= 180.0f)
        && (std::isfinite(t.warm_hue_width) && t.warm_hue_width >= 0.25f && t.warm_hue_width <= 3.0f)
        && (std::isfinite(t.warm_hue_shift) && t.warm_hue_shift >= -45.0f && t.warm_hue_shift <= 45.0f);
}

FilmColorResponse::FilmColorResponse(const FilmDensity& minimum_coordinate,
                                     const FilmDensity& neutral_reference_coordinate)
    : minimum_(minimum_coordinate)
{
    increment_.red = neutral_reference_coordinate.red - minimum_.red;
    increment_.green = neutral_reference_coordinate.green - minimum_.green;
    increment_.blue = neutral_reference_coordinate.blue - minimum_.blue;

    valid_ = std::isfinite(increment_.red) && std::isfinite(increment_.green) && std::isfinite(increment_.blue)
             && increment_.red > 1e-6f && increment_.green > 1e-6f && increment_.blue > 1e-6f;
}

bool
FilmColorResponse::valid() const
{
    return valid_;
}

float
FilmColorResponse::amount_from_trim(float trim)
{
    const float bounded = std::clamp(trim, minimum_trim, maximum_trim);
    return standard_amount * (1.0f + bounded / 4.0f);
}

FilmDensity
FilmColorResponse::apply(const FilmDensity& coordinate, const Settings& settings) const
{
    if (!valid_tuning(settings.tuning) || settings.tuning.response_amount == 0.0f
        || !valid_ || !std::isfinite(settings.amount) || settings.amount <= 0.0f
        || !std::isfinite(settings.density_depth) || !std::isfinite(settings.color_depth)
        || settings.color_depth < minimum_color_depth || settings.color_depth > maximum_color_depth) {
        return coordinate;
    }

    const float normalized[3] = { (coordinate.red - minimum_.red) / increment_.red,
                                  (coordinate.green - minimum_.green) / increment_.green,
                                  (coordinate.blue - minimum_.blue) / increment_.blue };
    const float neutral = (normalized[0] + normalized[1] + normalized[2]) / 3.0f;
    const float chroma[3] = { normalized[0] - neutral, normalized[1] - neutral, normalized[2] - neutral };
    const float magnitude = std::sqrt((chroma[0] * chroma[0] + chroma[1] * chroma[1] + chroma[2] * chroma[2]) / 3.0f);

    // Compress channel differences continuously from moderate through extreme
    // colour. A small common-coordinate reduction proportional to chroma sends
    // more light to the print stage, producing denser viewed colour rather than
    // merely moving saturated colours toward grey. This general stage is
    // radial in normalized dye-coordinate space; the narrowly gated warm
    // guidance is applied separately below.
    const auto& tuning = settings.tuning;
    const float density_position = tuning.density_center == 1.25f && tuning.density_width == 1.0f
        ? neutral : (neutral - tuning.density_center) / tuning.density_width + 1.25f;
    const float density_envelope = smoothstep(0.0f, 0.20f, density_position)
        * (1.0f - smoothstep(1.75f, 2.50f, density_position));
    constexpr float radians = 0.017453292519943295f;
    const float angle = tuning.warm_hue_center * radians;
    const float axis[3] = {
        0.40824829f * std::cos(angle) + 0.70710678f * std::sin(angle),
        0.40824829f * std::cos(angle) - 0.70710678f * std::sin(angle),
        -0.81649658f * std::cos(angle)
    };
    float compression = std::max(0.0f, settings.tuning.chroma_compression) * settings.amount * density_envelope;

    // Warm skin-like records occupy the broad yellow/red direction in the
    // normalized dye-coordinate plane: red and green rise together relative
    // to blue. Protect that direction through ordinary midscale densities,
    // then taper the protection at neutral and extreme chroma. This retains a
    // continuous warm-colour branch without exempting saturated reds from the
    // outer colour-density roll-off. It is an empirical colour-separation
    // control, not a face detector or a claim of measured interimage chemistry.
    const float chroma_length = std::sqrt(chroma[0] * chroma[0] + chroma[1] * chroma[1] + chroma[2] * chroma[2]);
    float warm_direction = 0.0f;
    if (chroma_length > 1e-6f) {
        warm_direction = (axis[0] * chroma[0] + axis[1] * chroma[1] + axis[2] * chroma[2]) / chroma_length;
    }
    if (tuning.warm_hue_width != 1.0f) {
        const float distance = std::acos(std::clamp(warm_direction, -1.0f, 1.0f));
        warm_direction = std::cos(std::min(3.14159265f, distance / tuning.warm_hue_width));
    }
    const float warm_hue = smoothstep(0.15f, 0.90f, warm_direction);
    const float warm_density = smoothstep(0.30f, 0.60f, density_position) * (1.0f - smoothstep(1.40f, 2.00f, density_position));
    const float warm_chroma = smoothstep(0.02f, 0.08f, magnitude) * (1.0f - smoothstep(0.35f, 0.75f, magnitude));
    const float warm_protection = tuning.warm_protection * warm_hue * warm_density * warm_chroma;
    compression *= 1.0f - warm_protection;

    const float knee_position = std::max(1e-6f, settings.tuning.chroma_knee);
    const float knee_ratio = magnitude / knee_position;
    const float scale = 1.0f / (1.0f + compression * knee_ratio);
    const float depth = std::max(0.0f, settings.density_depth) * settings.color_depth * settings.amount
                        * density_envelope * magnitude / (magnitude + knee_position);
    const float shaped_neutral = neutral - depth;

    // Guide only the protected warm lobe gently toward the yellow/orange
    // axis. Renormalizing the mixed direction keeps this separate from the
    // radial compression above: it prevents a near-skin trajectory from
    // curling toward magenta without turning true magenta objects into skin.
    const float warm_guidance = 0.15f * warm_hue * warm_density * warm_chroma;
    float guided_chroma[3] = { chroma[0], chroma[1], chroma[2] };
    if (warm_guidance > 0.0f && chroma_length > 1e-6f) {
        const float target[3] = { axis[0] * chroma_length, axis[1] * chroma_length, axis[2] * chroma_length };
        float mixed[3];
        float mixed_length_squared = 0.0f;
        for (int channel = 0; channel < 3; ++channel) {
            mixed[channel] = (1.0f - warm_guidance) * chroma[channel] + warm_guidance * target[channel];
            mixed_length_squared += mixed[channel] * mixed[channel];
        }
        const float mixed_length = std::sqrt(mixed_length_squared);
        if (mixed_length > 1e-6f) {
            const float normalization = chroma_length / mixed_length;
            for (int channel = 0; channel < 3; ++channel) {
                guided_chroma[channel] = mixed[channel] * normalization;
            }
        }
    }

    // Rotate locally in the plane perpendicular to the neutral axis.
    // Positive angles move from yellow toward red; negative angles toward green.
    const float rotation = tuning.warm_hue_shift * radians * warm_hue * warm_density * warm_chroma;
    if (rotation != 0.0f && chroma_length > 1e-6f) {
        const float tangent[3] = {
            (guided_chroma[1] - guided_chroma[2]) * 0.577350269f,
            (guided_chroma[2] - guided_chroma[0]) * 0.577350269f,
            (guided_chroma[0] - guided_chroma[1]) * 0.577350269f
        };
        for (int c = 0; c < 3; ++c)
            guided_chroma[c] = std::cos(rotation) * guided_chroma[c] + std::sin(rotation) * tangent[c];
    }

    FilmDensity result;
    result.red = std::max(minimum_.red, minimum_.red + (shaped_neutral + scale * guided_chroma[0]) * increment_.red);
    result.green = std::max(minimum_.green,
                            minimum_.green + (shaped_neutral + scale * guided_chroma[1]) * increment_.green);
    result.blue = std::max(minimum_.blue,
                           minimum_.blue + (shaped_neutral + scale * guided_chroma[2]) * increment_.blue);
    if (tuning.response_amount != 1.0f) {
        result.red = coordinate.red + tuning.response_amount * (result.red - coordinate.red);
        result.green = coordinate.green + tuning.response_amount * (result.green - coordinate.green);
        result.blue = coordinate.blue + tuning.response_amount * (result.blue - coordinate.blue);
    }
    return result;
}
