// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "cieobserver.h"
#include "colorimetry.h"
#include "filmdata.h"
#include "spectralilluminant.h"
#include "spectralreconstructor.h"

class SpectralColorimetry
{
public:
    // Emission-style sampled integration: spectrum * CMFs.
    static Colorimetry::XYZ integrate_xyz(
        const SampledCurve& spectrum,
        const CIEObserver& observer);

    // Reflectance-style sampled integration. Useful for generic sampled data,
    // but it necessarily interpolates the input SampledCurve.
    static Colorimetry::XYZ integrate_reflectance_xyz(
        const SampledCurve& reflectance,
        const CIEObserver& observer,
        const SpectralIlluminant& illuminant,
        float wavelength_min_nm = 360.0f,
        float wavelength_max_nm = 830.0f,
        float step_nm = 5.0f / 3.0f);

    // Preferred rgb2spec validation path. The polynomial is evaluated directly
    // at the quadrature points, avoiding a 5 nm SampledCurve round-trip.
    //
    // Defaults reproduce rgb2spec_opt's CIE integration grid:
    //   360..830 nm, three equal subintervals per 5 nm segment,
    //   Simpson's 3/8 quadrature.
    static Colorimetry::XYZ integrate_reconstructed_reflectance_xyz(
        const SpectralReconstructor& reconstructor,
        const SpectralReconstructor::Spectrum& reflectance,
        const CIEObserver& observer,
        const SpectralIlluminant& illuminant,
        float wavelength_min_nm = 360.0f,
        float wavelength_max_nm = 830.0f);
};
