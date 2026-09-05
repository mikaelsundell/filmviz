// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <array>

// ISO Status-M transmission densitometry used by Kodak colour-negative
// sensitometric data. FilmViz uses this class only as a measurement system:
// it converts a spectral optical-density curve back into the R/G/B density
// coordinates used by the published characteristic curves.
class StatusMDensitometer
{
public:
    using Density = std::array<double, 3>;

    // Measure diffuse spectral density using the ISO Status-M spectral
    // products. Invalid input produces NaN components.
    Density measure(
        const SampledCurve& spectral_density) const;

    // Peak-normalized Status-M weighting function for diagnostics.
    // channel: 0=R, 1=G, 2=B.
    static double weight(
        int channel,
        double wavelength_nm);

private:
    static double log_product(
        int channel,
        double wavelength_nm);
};
