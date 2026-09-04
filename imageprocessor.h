// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "inputtransform.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

class FilmPipeline;

class ImageProcessor
{
public:
    enum class Output
    {
        AP0Linear,
        Rec709Gamma24
    };

    using Progress =
        std::function<void(
            const char* stage,
            int completed,
            int total)>;

    struct Settings
    {
        int lut_size = 33;
        Output output = Output::Rec709Gamma24;

        float negative_grain_strength = 0.0f;
        float print_grain_strength = 0.0f;
        float grain_size_pixels = 1.0f;
        float grain_chroma = 1.0f;
        std::uint32_t grain_seed = 1u;
    };

    // Keeps Rec.709-weighted grain luminance fixed while scaling only the
    // differences between channel noise. Zero is neutral grain; one preserves
    // the measured independent-channel result.
    static std::array<float, 3> mix_grain_chroma(
        const std::array<float, 3>& density_noise,
        float chroma);

    bool process(
        const std::string& input_filename,
        const std::string& output_filename,
        const FilmPipeline& pipeline,
        const InputTransform& input_transform,
        const Settings& settings,
        const Progress& progress = Progress());

    const std::string& error() const;

private:
    std::string error_;
};
