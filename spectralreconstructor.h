// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "mitsuba/rgb2spec.h"

#include <array>
#include <string>

class SpectralReconstructor
{
public:
    enum class Method
    {
        Lookup,
        Optimized
    };

    struct Spectrum
    {
        float coeff[RGB2SPEC_N_COEFFS] = {0.0f, 0.0f, 0.0f};
        float scale = 1.0f;
    };

    SpectralReconstructor() = default;
    explicit SpectralReconstructor(const std::string& filename);

    ~SpectralReconstructor();

    SpectralReconstructor(const SpectralReconstructor&) = delete;
    SpectralReconstructor& operator=(const SpectralReconstructor&) = delete;

    SpectralReconstructor(SpectralReconstructor&& other) noexcept;
    SpectralReconstructor& operator=(SpectralReconstructor&& other) noexcept;

    bool load(const std::string& filename);
    void reset();

    bool valid() const;
    const std::string& filename() const;

    // Reconstruct rgb2spec coefficients. Lookup preserves the normal fast path.
    // Optimized performs rgb2spec_fetch_opt(), which applies one LM refinement
    // against the forward model stored in the .spec file.
    Spectrum reconstruct(
        const std::array<float, 3>& linear_rgb,
        Method method = Method::Lookup) const;

    float evaluate(
        const Spectrum& spectrum,
        float wavelength_nm) const;

    // Sample an already reconstructed polynomial. This avoids performing a new
    // rgb2spec lookup when the same spectrum is needed at multiple resolutions.
    SampledCurve sample(
        const Spectrum& spectrum,
        float wavelength_min_nm = 360.0f,
        float wavelength_max_nm = 830.0f,
        float wavelength_step_nm = 5.0f) const;

    SampledCurve reconstruct_sampled(
        const std::array<float, 3>& linear_rgb,
        float wavelength_min_nm = 360.0f,
        float wavelength_max_nm = 830.0f,
        float wavelength_step_nm = 5.0f,
        Method method = Method::Lookup) const;

    // Re-evaluate a reconstructed spectrum using the exact forward RGB weights
    // embedded in the .spec file. For an ACES2065_1 model this is the same
    // D60/CIE forward model used by rgb2spec_opt when the table was generated.
    std::array<float, 3> forward_rgb(
        const Spectrum& spectrum) const;

    bool has_forward_model() const;

private:
    RGB2Spec* model_ = nullptr;
    std::string filename_;
};
