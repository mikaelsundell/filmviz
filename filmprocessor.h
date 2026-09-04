// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "filmstock.h"
#include "mitsuba/rgb2spec.h"

#include <array>

class FilmProcessor
{
public:
    struct Settings
    {
        float exposure_stops = 0.0f;
        float log_exposure_calibration = 0.0f;

        float wavelength_min_nm = 380.0f;
        float wavelength_max_nm = 700.0f;
        float wavelength_step_nm = 5.0f;

        bool neutral_balance = true;
    };

    FilmProcessor(
        RGB2Spec* rgb2spec_model,
        const FilmStock& stock,
        const SampledCurve& illuminant,
        const Settings& settings);

    // Integrate an already-illuminated scene spectrum against the stock's
    // measured R/G/B spectral sensitivities.
    //
    // This is the core negative-exposure operation:
    //
    //     H_channel = sum E(lambda) * S_channel(lambda) * delta_lambda
    //
    // No log conversion, neutral balance, exposure-stop adjustment or
    // characteristic-curve development is applied here.
    FilmExposure expose(
        const SampledCurve& illuminated_spectrum) const;

    FilmExposure expose(
        const std::array<float, 3>& aces_rgb) const;

    FilmLogExposure log_exposure(
        const FilmExposure& exposure) const;

    // Evaluate the stock characteristic curves. For the Verita production
    // profile these R/G/B values are Kodak sensitometric/Status-M density
    // coordinates; they are not passed directly to FilmDyeModel in production.
    FilmDensity develop(
        const FilmLogExposure& exposure) const;

    FilmDensity process(
        const std::array<float, 3>& aces_rgb) const;

    const FilmExposureBalance&
    balance() const;

    const Settings&
    settings() const;

    // Return the illuminant exactly as sampled by the processor:
    // wavelength_min_nm .. wavelength_max_nm at wavelength_step_nm.
    SampledCurve
    sampled_illuminant() const;

private:
    FilmExposureBalance derive_neutral_balance() const;

    static float log_sensitivity_to_linear(
        const SampledCurve& curve,
        float wavelength_nm);

    static float sample_characteristic_clamped(
        const SampledCurve& curve,
        float log_exposure);

    RGB2Spec* rgb2spec_model_;
    const FilmStock& stock_;
    const SampledCurve& illuminant_;
    Settings settings_;
    FilmExposureBalance balance_;
};
