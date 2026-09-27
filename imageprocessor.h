// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmformat.h"
#include "granularitymodel.h"
#include "inputtransform.h"
#include "outputtransform.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

class FilmPipeline;

class ImageProcessor {
public:
    using Output = OutputTransform::Encoding;

    using Progress = std::function<void(const char* stage, int completed, int total)>;

    using Cancel = std::function<bool()>;

    struct Settings {
        int lut_size = 33;
        bool use_lut_acceleration = true;
        Output output = Output::Rec709Gamma24;

        float negative_grain_strength = 0.0f;
        float print_grain_strength = 0.0f;
        float grain_size_pixels = 1.0f; // Size multiplier: 1px at 2048px-wide Super 35.
        float grain_chroma = 1.0f;
        std::uint32_t grain_seed = 1u;

        std::string film_format = FilmFormatCatalog::default_format().identifier;
        float image_width_mm = FilmFormatCatalog::default_format().image_width_mm;
        float negative_mtf_amount = 0.0f;
        float print_mtf_amount = 0.0f;

        // Spatial negative-stage halation. It is deliberately excluded from
        // LUT generation because it depends on neighbouring pixels and is
        // applied to negative exposure before development.
        float halation_strength = 0.0f;
        float halation_radius_pixels = 12.0f;
        float halation_threshold = 0.7f;
    };

    // Keeps Rec.709-weighted grain luminance fixed while scaling only the
    // differences between channel noise. Zero is neutral grain; one preserves
    // the correlated per-channel result.
    static std::array<float, 3> mix_grain_chroma(const std::array<float, 3>& density_noise, float chroma);

    static std::array<float, 6> grain_residuals(const std::array<float, 24>& response,
        const std::array<float, 3>& linear, const GranularityModel::Texture& texture,
        std::uint32_t seed, int x, int y, float negative_strength, float print_strength);
    static std::array<float, 3> composite_grain(const std::array<float, 3>& linear,
        const std::array<float, 3>& residual, float chroma);
    // Empirical output look trim; does not modify measured density RMS.
    static float grain_visibility(const std::array<float, 3>& linear);

    bool process(const std::string& input_filename, const std::string& output_filename, const FilmPipeline& pipeline,
                 const InputTransform& input_transform, const Settings& settings, const Progress& progress = Progress(),
                 const Cancel& cancel = Cancel());

    const std::string& error() const;

private:
    std::string error_;
};
