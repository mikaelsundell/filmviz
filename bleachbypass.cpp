// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "bleachbypass.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr float kNegativeCoolDensitySwing = 0.14f;
constexpr float kPrintChromaReductionAtFullBypass = 0.80f;

float
clamped_amount(
    float amount)
{
    return
        std::isfinite(amount)
            ? std::clamp(amount, 0.0f, 1.0f)
            : 0.0f;
}

bool
finite_curve(
    const SampledCurve& curve)
{
    if (!curve.valid()) {
        return false;
    }

    for (float value : curve.y) {
        if (!std::isfinite(value)) {
            return false;
        }
    }

    return true;
}

} // namespace

BleachBypass::Result
BleachBypass::apply_negative(
    const SampledCurve& spectral_density,
    float amount)
{
    Result result;

    if (!finite_curve(spectral_density)
        || !std::isfinite(amount)) {
        return result;
    }

    result.spectral_density = spectral_density;
    result.mean_density_before =
        mean_density(spectral_density);
    result.spectral_span_before =
        spectral_span(spectral_density);

    const float strength =
        clamped_amount(amount);

    if (strength > 0.0f
        && spectral_density.x.size() > 1) {

        const float wavelength_min =
            spectral_density.x.front();

        const float wavelength_max =
            spectral_density.x.back();

        const float wavelength_span =
            wavelength_max - wavelength_min;

        if (wavelength_span > 0.0f) {
            std::vector<float> shape(
                spectral_density.x.size(),
                0.0f);

            double shape_sum = 0.0;

            for (std::size_t i = 0;
                 i < spectral_density.x.size();
                 ++i) {

                const float t =
                    (spectral_density.x[i] - wavelength_min)
                    / wavelength_span;

                // +1 at the short-wave end, -1 at the long-wave end.
                // This leaves more long-wave transmission in the negative,
                // driving more cyan-forming exposure in the print stage.
                shape[i] =
                    1.0f - 2.0f * t;

                shape_sum +=
                    static_cast<double>(shape[i]);
            }

            const float shape_mean =
                static_cast<float>(
                    shape_sum
                    / static_cast<double>(shape.size()));

            for (float& value : shape) {
                value -= shape_mean;
            }

            float delta_scale =
                strength
                * kNegativeCoolDensitySwing;

            // Keep the density physically non-negative without introducing a
            // neutral density offset. Scaling a zero-mean tilt preserves the
            // mean density and therefore avoids the previous contrast boost.
            for (std::size_t i = 0;
                 i < spectral_density.y.size();
                 ++i) {

                if (shape[i] < 0.0f) {
                    const float available =
                        spectral_density.y[i]
                        / -shape[i];

                    delta_scale =
                        std::min(
                            delta_scale,
                            available);
                }
            }

            for (std::size_t i = 0;
                 i < result.spectral_density.y.size();
                 ++i) {

                result.spectral_density.y[i] =
                    std::max(
                        0.0f,
                        result.spectral_density.y[i]
                            + delta_scale * shape[i]);
            }
        }
    }

    result.mean_density_after =
        mean_density(result.spectral_density);

    result.spectral_span_after =
        spectral_span(result.spectral_density);

    result.valid =
        finite_curve(result.spectral_density);

    return result;
}

BleachBypass::Result
BleachBypass::apply_print(
    const SampledCurve& spectral_density,
    float amount)
{
    Result result;

    if (!finite_curve(spectral_density)
        || !std::isfinite(amount)) {
        return result;
    }

    result.spectral_density = spectral_density;
    result.mean_density_before =
        mean_density(spectral_density);
    result.spectral_span_before =
        spectral_span(spectral_density);

    const float strength =
        clamped_amount(amount);

    if (strength > 0.0f) {
        const float chroma_scale =
            1.0f
            - strength
                * kPrintChromaReductionAtFullBypass;

        for (float& density : result.spectral_density.y) {
            density =
                result.mean_density_before
                + chroma_scale
                    * (density - result.mean_density_before);
        }
    }

    result.mean_density_after =
        mean_density(result.spectral_density);

    result.spectral_span_after =
        spectral_span(result.spectral_density);

    result.valid =
        finite_curve(result.spectral_density);

    return result;
}

float
BleachBypass::mean_density(
    const SampledCurve& spectral_density)
{
    if (!finite_curve(spectral_density)
        || spectral_density.y.empty()) {
        return 0.0f;
    }

    double sum = 0.0;

    for (float density : spectral_density.y) {
        sum +=
            static_cast<double>(density);
    }

    return
        static_cast<float>(
            sum
            / static_cast<double>(spectral_density.y.size()));
}

float
BleachBypass::spectral_span(
    const SampledCurve& spectral_density)
{
    if (!finite_curve(spectral_density)
        || spectral_density.y.empty()) {
        return 0.0f;
    }

    const auto range =
        std::minmax_element(
            spectral_density.y.begin(),
            spectral_density.y.end());

    return
        *range.second
        - *range.first;
}
