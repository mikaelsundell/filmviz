// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <array>
#include <string>
#include <vector>

class SpectralIlluminant;

// Experimental FilmViz-owned AP0 -> sampled reflectance reconstruction.
//
// This is intentionally separate from SpectralReconstructor/rgb2spec so the
// two implementations can be compared side-by-side without changing the
// production rgb2spec path.
//
// The solver minimizes
//
//     || AP0(S) - target ||^2 + lambda * ||D2 S||^2
//
// subject to 0 <= S(lambda) <= 1.
//
// It is meant for validation and image experiments. It is not yet the final
// production reconstruction architecture.
class SampledSpectralReconstructor
{
public:
    struct Settings
    {
        float wavelength_min_nm = 360.0f;
        float wavelength_max_nm = 830.0f;
        float wavelength_step_nm = 5.0f;

        double smoothness = 1e-4;

        // Kept deliberately moderate for interactive LUT generation.
        // The diagnostic solver can use a much larger iteration count.
        int max_iterations = 512;
        double projected_gradient_tolerance = 1e-10;
    };

    SampledSpectralReconstructor() = default;

    bool initialize(
        const std::string& observer_filename,
        const SpectralIlluminant& illuminant);

    bool initialize(
        const std::string& observer_filename,
        const SpectralIlluminant& illuminant,
        const Settings& settings);

    bool valid() const;

    SampledCurve reconstruct(
        const std::array<float, 3>& ap0_linear) const;

    const Settings& settings() const;
    const std::string& error() const;

public:
    struct ObserverSample
    {
        double wavelength = 0.0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

private:
    bool load_observer(
        const std::string& filename);

    bool build_forward_matrix(
        const SpectralIlluminant& illuminant);

    std::array<double, 3> forward_ap0(
        const std::vector<double>& spectrum) const;

    Settings settings_;
    std::string error_;
    bool valid_ = false;

    std::vector<ObserverSample> observer_;
    std::vector<double> wavelengths_;
    std::array<std::vector<double>, 3> forward_matrix_;
};
