// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

class SpectralIlluminant
{
public:
    enum class Standard
    {
        D60
    };

    SpectralIlluminant() = default;
    explicit SpectralIlluminant(Standard standard);

    bool valid() const;

    const SampledCurve&
    curve() const;

    // Apply this illuminant to a sampled spectral scene factor.
    //
    // The returned curve is sampled at the wavelengths of `spectral_factor`
    // and contains:
    //
    //     illuminated(lambda)
    //       = spectral_factor(lambda) * illuminant(lambda)
    //
    // For the rgb2spec path, spectral_factor is reflectance-like spectral
    // shape with the scene-linear magnitude restored by SpectralReconstructor.
    SampledCurve illuminate(
        const SampledCurve& spectral_factor) const;

private:
    SampledCurve m_curve;
};
