// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "spectralilluminant.h"

#include "mitsuba/details/cie1931.h"

SpectralIlluminant::SpectralIlluminant(
    Standard standard)
{
    if (standard != Standard::D60) {
        return;
    }

    m_curve.x.reserve(CIE_SAMPLES);
    m_curve.y.reserve(CIE_SAMPLES);

    for (int i = 0; i < CIE_SAMPLES; ++i) {
        const float wavelength =
            static_cast<float>(
                CIE_LAMBDA_MIN
                + 5.0 * static_cast<double>(i));

        m_curve.x.push_back(
            wavelength);

        m_curve.y.push_back(
            static_cast<float>(
                cie_d60[i]));
    }
}

bool
SpectralIlluminant::valid() const
{
    return m_curve.valid();
}

const SampledCurve&
SpectralIlluminant::curve() const
{
    return m_curve;
}

SampledCurve
SpectralIlluminant::illuminate(
    const SampledCurve& spectral_factor) const
{
    SampledCurve result;

    if (!valid()
        || !spectral_factor.valid()) {
        return result;
    }

    result.x.reserve(
        spectral_factor.x.size());

    result.y.reserve(
        spectral_factor.y.size());

    for (std::size_t i = 0;
         i < spectral_factor.x.size();
         ++i) {

        const float wavelength =
            spectral_factor.x[i];

        result.x.push_back(
            wavelength);

        result.y.push_back(
            spectral_factor.y[i]
            * m_curve.sample(
                wavelength));
    }

    return result;
}
