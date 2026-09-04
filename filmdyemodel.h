// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "filmstock.h"

#include <cstddef>
#include <string>

// Negative spectral-density basis model reconstructed from Kodak Verita data.
//
// The model owns the measured minimum and midscale-neutral spectra and an
// inferred C/M/Y-forming spectral basis. It deliberately does not know about
// ISO Status-M densitometry. That distinction is important: the numerical
// R/G/B densities published on Kodak sensitometric curves are measurement
// coordinates, not automatically the same coordinates as this spectral basis.
//
// Production code must therefore pass negative characteristic-curve densities
// through FilmDensityCalibration before calling synthesize_density(). The
// calibration solves the Status-M -> spectral-basis coordinate relationship.
//
// Legacy tests may still call this class directly in order to reproduce and
// compare earlier stages of the investigation.
class FilmDyeModel
{
public:
    struct Diagnostics
    {
        // Stage 1 partition diagnostics.
        float rms_reconstruction_error = 0.0f;
        float max_abs_reconstruction_error = 0.0f;
        float max_error_wavelength_nm = 0.0f;

        float cyan_peak_wavelength_nm = 0.0f;
        float magenta_peak_wavelength_nm = 0.0f;
        float yellow_peak_wavelength_nm = 0.0f;

        float cyan_prior_center_nm = 0.0f;
        float magenta_prior_center_nm = 0.0f;
        float yellow_prior_center_nm = 0.0f;

        float cyan_prior_sigma_nm = 0.0f;
        float magenta_prior_sigma_nm = 0.0f;
        float yellow_prior_sigma_nm = 0.0f;

        float cyan_integrated_fraction = 0.0f;
        float magenta_integrated_fraction = 0.0f;
        float yellow_integrated_fraction = 0.0f;

        std::size_t sample_count = 0;

        // Stage 2 calibration diagnostics.
        float calibration_log_exposure = -0.515f;

        FilmDensity minimum_record_density;
        FilmDensity calibration_record_density;
        FilmDensity calibration_record_increment;

        float calibration_rms_error = 0.0f;
        float calibration_max_abs_error = 0.0f;
        float calibration_max_error_wavelength_nm = 0.0f;
    };

    bool load_and_estimate(
        const std::string& filename,
        const FilmStock& stock,
        float wavelength_min_nm = 380.0f,
        float wavelength_max_nm = 700.0f,
        float wavelength_step_nm = 5.0f,
        float calibration_log_exposure = -0.515f);

    bool valid() const;
    bool calibrated() const;

    const SampledCurve& minimum_density() const;
    const SampledCurve& midscale_neutral_density() const;
    const SampledCurve& neutral_increment() const;

    // Neutral-state contributions. These sum to the measured neutral increment.
    const SampledCurve& cyan_contribution() const;
    const SampledCurve& magenta_contribution() const;
    const SampledCurve& yellow_contribution() const;

    // Per-unit developed record-density basis functions.
    const SampledCurve& cyan_basis_per_record_density() const;
    const SampledCurve& magenta_basis_per_record_density() const;
    const SampledCurve& yellow_basis_per_record_density() const;

    const SampledCurve& reconstructed_increment() const;
    const SampledCurve& reconstructed_midscale() const;
    const SampledCurve& residual() const;

    const SampledCurve& calibrated_reconstructed_midscale() const;
    const SampledCurve& calibration_residual() const;

    const Diagnostics& diagnostics() const;

    // Synthesize total diffuse spectral density from FilmDyeModel coordinates.
    // In the production pipeline these coordinates come from
    // FilmDensityCalibration, not directly from Kodak Status-M sensitometry.
    SampledCurve synthesize_density(
        const FilmDensity& spectral_coordinate) const;

    // Spectral transmittance T(lambda) = 10^(-D(lambda)).
    SampledCurve synthesize_transmittance(
        const FilmDensity& spectral_coordinate) const;

    // Convenience helpers for a neutral sensitometric exposure where all
    // three film records receive the same log exposure.
    FilmDensity neutral_record_density(
        const FilmStock& stock,
        float log_exposure) const;

    SampledCurve synthesize_neutral_density(
        const FilmStock& stock,
        float log_exposure) const;

    SampledCurve synthesize_neutral_transmittance(
        const FilmStock& stock,
        float log_exposure) const;

private:
    bool load_reference_density_csv(
        const std::string& filename);

    bool estimate_from_stock(
        const FilmStock& stock,
        float wavelength_min_nm,
        float wavelength_max_nm,
        float wavelength_step_nm);

    bool calibrate_to_characteristic(
        const FilmStock& stock,
        float calibration_log_exposure);

    static float sample_characteristic_clamped(
        const SampledCurve& curve,
        float log_exposure);

    SampledCurve minimum_density_;
    SampledCurve midscale_neutral_density_;
    SampledCurve neutral_increment_;

    SampledCurve cyan_contribution_;
    SampledCurve magenta_contribution_;
    SampledCurve yellow_contribution_;

    SampledCurve cyan_basis_per_record_density_;
    SampledCurve magenta_basis_per_record_density_;
    SampledCurve yellow_basis_per_record_density_;

    SampledCurve reconstructed_increment_;
    SampledCurve reconstructed_midscale_;
    SampledCurve residual_;

    SampledCurve calibrated_reconstructed_midscale_;
    SampledCurve calibration_residual_;

    Diagnostics diagnostics_;
    bool calibrated_ = false;
};
