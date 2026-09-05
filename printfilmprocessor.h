// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "printfilmstock.h"

class PrintFilmProcessor
{
public:
    struct Settings
    {
        float exposure_stops = 0.0f;
        float log_exposure_calibration = 0.0f;

        float wavelength_min_nm = 380.0f;
        float wavelength_max_nm = 700.0f;
        float wavelength_step_nm = 5.0f;

        // Kodak's spectral dye-density graph is normalized around a
        // visual-neutral density of 1.0.
        float reference_status_a_density = 1.0f;

        // Traditional printer-light scale. The neutral calibration point is
        // 25/25/25 and one printer-light point equals 0.025 LogE.
        float printer_light_red = 25.0f;
        float printer_light_green = 25.0f;
        float printer_light_blue = 25.0f;
        float printer_light_log_exposure_per_point = 0.025f;
    };

    struct Balance
    {
        FilmExposure neutral_reference_exposure;

        FilmLogExposure neutral_reference_raw_log_exposure;
        FilmLogExposure target_log_exposure;

        float red_log_offset = 0.0f;
        float green_log_offset = 0.0f;
        float blue_log_offset = 0.0f;
    };

    PrintFilmProcessor(
        const PrintFilmStock& stock,
        const SampledCurve& printer_illuminant,
        const SampledCurve& reference_negative_transmittance,
        const Settings& settings);

    bool valid() const;

    FilmExposure expose(
        const SampledCurve& negative_transmittance) const;

    FilmLogExposure log_exposure(
        const FilmExposure& exposure) const;

    FilmDensity develop(
        const FilmLogExposure& exposure) const;

    FilmDensity process(
        const SampledCurve& negative_transmittance) const;

    const Balance& balance() const;
    const Settings& settings() const;
    const SampledCurve& printer_illuminant() const;

    // Replace the three printer-record calibration offsets after an external
    // full-chain calibration solve. This changes only the printer operating
    // point; spectral exposure integration and 2383 development are unchanged.
    void set_balance_log_offsets(
        float red_log_offset,
        float green_log_offset,
        float blue_log_offset);

    // Relative Planckian SPD, peak-normalized to 1.0. This is an explicit
    // approximation for the initial printer-light prototype and can later be
    // replaced by a measured printer/filter SPD without changing the processor.
    static SampledCurve make_blackbody_illuminant(
        float kelvin,
        float wavelength_min_nm = 380.0f,
        float wavelength_max_nm = 700.0f,
        float wavelength_step_nm = 5.0f);

private:
    bool derive_balance(
        const SampledCurve& reference_negative_transmittance);

    static float log_sensitivity_to_linear(
        const SampledCurve& curve,
        float wavelength_nm);

    static float sample_characteristic_clamped(
        const SampledCurve& curve,
        float log_exposure);

    static float inverse_characteristic(
        const SampledCurve& curve,
        float target_density);

    const PrintFilmStock& stock_;
    SampledCurve printer_illuminant_;
    Settings settings_;
    Balance balance_;
    bool valid_ = false;
};
