// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

// Flattened, renderer-neutral representation of an initialized FilmPipeline.
// Measured/profile data lives here while image controls remain live parameters.
struct FilmDirectData {
    struct Curve {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };

    std::uint32_t spectral_count = 0;
    float wavelength_min_nm = 380.0f;
    float wavelength_step_nm = 5.0f;

    std::uint32_t rgb2spec_resolution = 0;
    std::uint32_t rgb2spec_forward_count = 0;
    std::vector<float> rgb2spec_scale;
    std::vector<float> rgb2spec_data;
    std::vector<float> rgb2spec_forward;

    std::vector<float> negative_exposure_samples;
    std::vector<float> negative_density_samples;
    std::vector<float> status_m_samples;
    std::vector<float> print_exposure_samples;
    std::vector<float> print_density_samples;
    std::vector<float> viewer_ap0_samples;
    std::vector<float> negative_granularity_samples;
    std::vector<float> print_granularity_samples;
    float granularity_density_min = 0.0f;
    float granularity_density_max = 4.0f;

    // Packed float2 x/y knots used by the six characteristic curves.
    std::vector<float> characteristic_points;
    std::array<Curve, 3> negative_characteristic;
    std::array<Curve, 3> print_characteristic;

    std::array<float, 3> reference_negative_exposure = { { 0.0f, 0.0f, 0.0f } };
    std::array<float, 3> reference_negative_density = { { 0.0f, 0.0f, 0.0f } };
    std::array<float, 3> minimum_negative_coordinate = { { 0.0f, 0.0f, 0.0f } };
    std::array<float, 3> neutral_negative_increment = { { 0.0f, 0.0f, 0.0f } };
    std::array<float, 3> calibration_zero_target = { { 0.0f, 0.0f, 0.0f } };
    std::array<float, 3> calibration_zero_measured = { { 0.0f, 0.0f, 0.0f } };
    std::array<float, 3> calibration_minimum_status_m = { { 0.0f, 0.0f, 0.0f } };
    std::array<float, 9> calibration_zero_jacobian = { { 0.0f } };
    std::array<float, 3> print_target_log_exposure = { { 0.0f, 0.0f, 0.0f } };

    bool valid() const;
};
