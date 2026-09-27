// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

struct alignas(16) FilmVizDirectParams
{
    std::uint32_t spectral_count = 0;
    std::uint32_t rgb2spec_resolution = 0;
    std::uint32_t rgb2spec_forward_count = 0;
    std::uint32_t input_profile = 0;
    std::uint32_t output_profile = 1;
    std::uint32_t reserved_header[3] = {};

    std::int32_t source_x1 = 0;
    std::int32_t source_y1 = 0;
    std::int32_t source_x2 = 0;
    std::int32_t source_y2 = 0;
    std::int32_t destination_x1 = 0;
    std::int32_t destination_y1 = 0;
    std::int32_t destination_x2 = 0;
    std::int32_t destination_y2 = 0;
    std::int32_t render_x1 = 0;
    std::int32_t render_y1 = 0;
    std::int32_t render_x2 = 0;
    std::int32_t render_y2 = 0;
    std::uint32_t source_row_bytes = 0;
    std::uint32_t destination_row_bytes = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;

    float exposure_stops = 0.0f;
    float negative_flash_percent = 0.0f;
    float print_flash_percent = 0.0f;
    float push_pull_stops = 0.0f;
    float color_density = 0.0f;
    float color_depth = 1.0f;
    float negative_bleach_bypass = 0.0f;
    float print_bleach_bypass = 0.0f;
    float printer_light_red = 25.0f;
    float printer_light_green = 25.0f;
    float printer_light_blue = 25.0f;
    float printer_light_master = 0.0f;
    float middle_gray = 0.18f;
    float printer_temperature = 3200.0f;
    float wavelength_min_nm = 380.0f;
    float wavelength_step_nm = 5.0f;
    std::uint32_t granularity_count = 0;
    std::uint32_t frame_seed = 0;
    std::uint32_t grain_enabled = 0;
    std::uint32_t reserved_grain = 0;
    float negative_grain = 0.0f;
    float print_grain = 0.0f;
    float grain_size = 1.0f;
    float grain_chroma = 1.0f;
    std::uint32_t grain_tonal_enabled = 1;
    float grain_shadows = 1.0f;
    float grain_midtones = 1.0f;
    float grain_highlights = 1.0f;
    float granularity_density_min = 0.0f;
    float granularity_density_max = 4.0f;
    float reserved_grain_float[2] = {}; // Reference aperture in pixels, then reserved.

    float response_response_amount = 1.0f;
    float response_chroma_compression = 0.22f;
    float response_chroma_knee = 0.5f;
    float response_density_center = 1.25f;
    float response_density_width = 1.0f;
    float response_warm_protection = 0.5f;
    float response_warm_hue_center = 0.0f;
    float response_warm_hue_width = 1.0f;
    float response_warm_hue_shift = 0.0f;
    float reserved_response[3] = {};

    float reference_negative_exposure[4] = {};
    float reference_negative_density[4] = {};
    float minimum_negative_coordinate[4] = {};
    float neutral_negative_increment[4] = {};
    float calibration_zero_target[4] = {};
    float calibration_zero_measured[4] = {};
    float calibration_minimum_status_m[4] = {};
    float print_target_log_exposure[4] = {};
    float calibration_jacobian_0[4] = {};
    float calibration_jacobian_1[4] = {};
    float calibration_jacobian_2[4] = {};

    std::uint32_t curve_negative_01[4] = {};
    std::uint32_t curve_negative_2_print_0[4] = {};
    std::uint32_t curve_print_12[4] = {};
};

static_assert(sizeof(FilmVizDirectParams) == 496,
    "FilmVizDirectParams must match the GPU constant-buffer layout");

struct alignas(16) FilmVizDirectSpatialParams
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t radius = 0;
    std::uint32_t horizontal = 0;
    std::uint32_t gamma24 = 0;
    std::uint32_t reserved[3] = {}; // [0]: preserve signed/unclipped linear residuals.
    float strength = 0.0f;
    float threshold = 0.0f;
    float reserved_float[2] = {};
    float scatter[4] = {0.22f, 0.06f, 0.015f, 0.0f};
};

static_assert(sizeof(FilmVizDirectSpatialParams) == 64,
    "FilmVizDirectSpatialParams must match the GPU constant-buffer layout");

inline int filmviz_halation_box_radius(float radius)
{
    const float sigma = std::max(0.5f, radius / 3.0f);
    return std::max(1, static_cast<int>(std::round(
        0.5f * (-1.0f + std::sqrt(1.0f + 4.0f * sigma * sigma)))));
}
