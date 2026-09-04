// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <cstdint>
#include <string>

// Interpolates Kodak diffuse-RMS-granularity measurements in their native
// density coordinates. Spatial synthesis is deliberately kept separate.
class GranularityModel
{
public:
    bool load(
        const std::string& negative_filename,
        const std::string& print_filename);

    bool valid() const;

    FilmDensity negative_sigma(
        const FilmDensity& status_m_density) const;

    FilmDensity print_sigma(
        const FilmDensity& status_a_density) const;

    // Reproducible unit-normal sample for image-domain grain synthesis.
    static float normal_sample(
        std::uint32_t seed,
        int x,
        int y,
        int stage,
        int channel);

private:
    struct Curves
    {
        SampledCurve red_density;
        SampledCurve green_density;
        SampledCurve blue_density;
        SampledCurve red_sigma;
        SampledCurve green_sigma;
        SampledCurve blue_sigma;
    };

    static bool load_curves(
        const std::string& filename,
        bool negative_order,
        Curves& curves);

    static float sigma_for_density(
        const SampledCurve& density,
        const SampledCurve& sigma,
        float target_density);

    static FilmDensity sample(
        const Curves& curves,
        const FilmDensity& density);

    Curves negative_;
    Curves print_;
    bool valid_ = false;
};
