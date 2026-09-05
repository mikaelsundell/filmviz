// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <functional>
#include <vector>

// Spatial negative-stage halation approximation.
//
// The model derives a smooth highlight source from scene-linear AP0, scatters
// the corresponding negative exposure spatially, and adds the scattered light
// back into the negative exposure before development. It therefore remains an
// image-space effect, but its result is developed by the actual film model
// instead of being added as a display-space glow.
class HalationModel
{
public:
    using Cancel =
        std::function<bool()>;

    using Progress =
        std::function<void(
            int completed,
            int total)>;

    struct Settings
    {
        float strength = 0.0f;
        float radius_pixels = 12.0f;
        float threshold = 0.7f;

        // Relative backscatter into the negative records. The red-sensitive
        // record dominates, with smaller green and blue contributions. These
        // are phenomenological process parameters, not measured Verita data.
        FilmExposure record_scatter = {
            0.22f,
            0.06f,
            0.015f
        };
    };

    static bool valid_settings(
        const Settings& settings);

    // Modifies negative exposure in place. ap0_pixels contains width*height
    // scene-linear AP0 RGB triplets corresponding to the exposure samples.
    static bool apply(
        std::vector<FilmExposure>& negative_exposure,
        const std::vector<float>& ap0_pixels,
        int width,
        int height,
        const Settings& settings,
        const Cancel& cancel = Cancel(),
        const Progress& progress = Progress());

private:
    static float ap0_luminance(
        const float* ap0);

    static float smooth_highlight_weight(
        float luminance,
        float threshold);

    static std::vector<float> gaussian_blur_scalar(
        const std::vector<float>& source,
        int width,
        int height,
        float radius_pixels,
        const Cancel& cancel,
        const std::function<void()>& work_completed);
};
