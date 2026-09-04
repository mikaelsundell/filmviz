// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "spectralcolorimetry.h"

#include <cmath>
#include <cstddef>

namespace {

double
simpson38_weight(
    std::size_t i,
    std::size_t last,
    double h)
{
    double weight =
        3.0 / 8.0 * h;

    if (i == 0 || i == last) {
        return weight;
    }

    if ((i - 1) % 3 == 2) {
        return weight * 2.0;
    }

    return weight * 3.0;
}

std::size_t
rgb2spec_interval_count(
    float wavelength_min_nm,
    float wavelength_max_nm)
{
    // rgb2spec uses 5 nm reference samples with three subintervals per
    // reference segment. 360..830 contains 94 five-nanometre segments.
    const double segments =
        static_cast<double>(
            wavelength_max_nm - wavelength_min_nm)
        / 5.0;

    return
        static_cast<std::size_t>(
            std::llround(segments))
        * 3u;
}

} // namespace

Colorimetry::XYZ
SpectralColorimetry::integrate_xyz(
    const SampledCurve& spectrum,
    const CIEObserver& observer)
{
    Colorimetry::XYZ result;

    if (!spectrum.valid() || !observer.valid()) {
        return result;
    }

    for (std::size_t i = 1; i < spectrum.x.size(); ++i) {
        const float lambda0 = spectrum.x[i - 1];
        const float lambda1 = spectrum.x[i];
        const double dlambda =
            static_cast<double>(lambda1 - lambda0);

        const double s0 = spectrum.y[i - 1];
        const double s1 = spectrum.y[i];

        result.x +=
            0.5
            * (
                s0 * observer.xbar().sample(lambda0)
                + s1 * observer.xbar().sample(lambda1)
            )
            * dlambda;

        result.y +=
            0.5
            * (
                s0 * observer.ybar().sample(lambda0)
                + s1 * observer.ybar().sample(lambda1)
            )
            * dlambda;

        result.z +=
            0.5
            * (
                s0 * observer.zbar().sample(lambda0)
                + s1 * observer.zbar().sample(lambda1)
            )
            * dlambda;
    }

    return result;
}

Colorimetry::XYZ
SpectralColorimetry::integrate_reflectance_xyz(
    const SampledCurve& reflectance,
    const CIEObserver& observer,
    const SpectralIlluminant& illuminant,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float step_nm)
{
    Colorimetry::XYZ result;

    if (!reflectance.valid()
        || !observer.valid()
        || !illuminant.valid()
        || step_nm <= 0.0f
        || wavelength_max_nm <= wavelength_min_nm) {
        return result;
    }

    const double span =
        static_cast<double>(
            wavelength_max_nm - wavelength_min_nm);

    std::size_t intervals =
        static_cast<std::size_t>(
            std::llround(
                span
                / static_cast<double>(step_nm)));

    if (intervals < 3) {
        intervals = 3;
    }

    intervals -= intervals % 3;

    const double h =
        span / static_cast<double>(intervals);

    for (std::size_t i = 0; i <= intervals; ++i) {
        const float lambda =
            static_cast<float>(
                static_cast<double>(wavelength_min_nm)
                + h * static_cast<double>(i));

        const double weight =
            simpson38_weight(
                i,
                intervals,
                h);

        const double r =
            reflectance.sample(lambda);

        const double e =
            illuminant.curve().sample(lambda);

        result.x +=
            r * e
            * observer.xbar().sample(lambda)
            * weight;

        result.y +=
            r * e
            * observer.ybar().sample(lambda)
            * weight;

        result.z +=
            r * e
            * observer.zbar().sample(lambda)
            * weight;
    }

    // SpectralIlluminant::D60 uses the same already-normalized D60 values as
    // rgb2spec's reference tables, so no post-hoc Y normalization is applied.
    return result;
}

Colorimetry::XYZ
SpectralColorimetry::integrate_reconstructed_reflectance_xyz(
    const SpectralReconstructor& reconstructor,
    const SpectralReconstructor::Spectrum& reflectance,
    const CIEObserver& observer,
    const SpectralIlluminant& illuminant,
    float wavelength_min_nm,
    float wavelength_max_nm)
{
    Colorimetry::XYZ result;

    if (!reconstructor.valid()
        || !observer.valid()
        || !illuminant.valid()
        || wavelength_max_nm <= wavelength_min_nm) {
        return result;
    }

    const std::size_t intervals =
        rgb2spec_interval_count(
            wavelength_min_nm,
            wavelength_max_nm);

    if (intervals == 0) {
        return result;
    }

    const double h =
        static_cast<double>(
            wavelength_max_nm - wavelength_min_nm)
        / static_cast<double>(intervals);

    for (std::size_t i = 0; i <= intervals; ++i) {
        const float lambda =
            static_cast<float>(
                static_cast<double>(wavelength_min_nm)
                + h * static_cast<double>(i));

        const double weight =
            simpson38_weight(
                i,
                intervals,
                h);

        const double r =
            static_cast<double>(
                reconstructor.evaluate(
                    reflectance,
                    lambda));

        const double e =
            static_cast<double>(
                illuminant.curve().sample(lambda));

        result.x +=
            r * e
            * observer.xbar().sample(lambda)
            * weight;

        result.y +=
            r * e
            * observer.ybar().sample(lambda)
            * weight;

        result.z +=
            r * e
            * observer.zbar().sample(lambda)
            * weight;
    }

    return result;
}
