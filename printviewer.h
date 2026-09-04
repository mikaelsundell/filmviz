// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <array>
#include <cstddef>
#include <string>

class PrintViewer
{
public:
    struct Settings
    {
        float wavelength_min_nm = 380.0f;
        float wavelength_max_nm = 700.0f;
        float wavelength_step_nm = 5.0f;
    };

    struct XYZ
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct xy
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Result
    {
        XYZ viewed_xyz;
        xy viewed_xy;
        XYZ adapted_xyz_d60;

        // Viewed print tristimulus represented in linear ACES2065-1/AP0.
        // This is a container colour space for the already-rendered print
        // simulation. No ACES RRT/ODT is applied here.
        std::array<float, 3> aces2065_1 = {{0.0f, 0.0f, 0.0f}};
    };

    bool load(
        const std::string& cie_observer_filename,
        const std::string& viewing_illuminant_filename,
        const Settings& settings);

    bool valid() const;

    Result view(
        const SampledCurve& print_transmittance) const;

    // Prototype 31 fast tone-coordinate path. Returns viewed Y under the
    // same normalized D55/CIE integration as view(), without computing X/Z,
    // chromaticity, Bradford adaptation, or AP0.
    float view_luminance(
        const SampledCurve& print_transmittance) const;

    Result view_flat_transmittance(
        float transmittance = 1.0f) const;

    SampledCurve viewed_spectrum(
        const SampledCurve& print_transmittance) const;

    const Settings& settings() const;

    const std::string& cie_observer_filename() const;
    const std::string& viewing_illuminant_filename() const;

    const SampledCurve& x_bar() const;
    const SampledCurve& y_bar() const;
    const SampledCurve& z_bar() const;
    const SampledCurve& viewing_illuminant() const;

    float normalization_k() const;
    XYZ viewing_white_xyz() const;
    xy viewing_white_xy() const;
    XYZ target_d60_xyz() const;
    xy target_d60_xy() const;

    bool observer_covers_integration_range() const;
    bool illuminant_covers_integration_range() const;

private:
    bool load_cie_observer(
        const std::string& filename);

    bool load_illuminant(
        const std::string& filename);

    bool initialize_colorimetry();

    XYZ integrate_xyz(
        const SampledCurve* transmittance) const;

    float integrate_y(
        const SampledCurve* transmittance) const;

    bool transmittance_matches_integration_grid(
        const SampledCurve& transmittance) const;

    XYZ adapt_to_d60(
        const XYZ& xyz) const;

    static xy xyz_to_xy(
        const XYZ& xyz);

    static std::array<float, 3> xyz_d60_to_ap0(
        const XYZ& xyz);

    Settings settings_;

    std::string cie_observer_filename_;
    std::string viewing_illuminant_filename_;

    SampledCurve x_bar_;
    SampledCurve y_bar_;
    SampledCurve z_bar_;
    SampledCurve viewing_illuminant_;

    float normalization_k_ = 0.0f;

    // Prototype 31 cached integration data on the active wavelength grid.
    std::vector<float> integration_wavelengths_;
    std::vector<double> integration_weight_x_;
    std::vector<double> integration_weight_y_;
    std::vector<double> integration_weight_z_;

    // Cached Bradford D55/viewing-white -> ACES D60 matrix.
    std::array<double, 9> adaptation_to_d60_ = {{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    }};

    XYZ viewing_white_xyz_;
    xy viewing_white_xy_;
    XYZ target_d60_xyz_;
    xy target_d60_xy_;

    bool observer_covers_integration_range_ = false;
    bool illuminant_covers_integration_range_ = false;
    bool valid_ = false;
};
