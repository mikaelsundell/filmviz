// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "printfilmstock.h"

#include <cstddef>
#include <vector>

// Spectral dye model for Kodak Vision 2383/3383 print film.
//
// The production implementation uses a qualified nonlinear dye-growth mapping
// and chroma-aware neutral correction. Kodak's published C/M/Y curves remain
// the spectral basis. Density-1 amplitudes retain the D55/CIE-1931
// colorimetric reference calibration.
//
// The former linear growth law is retained explicitly for A/B diagnostics.
// Active synthesis maps each normalized Status-A record increment through a
// 161-point monotone-cubic calibration derived from a neutral-preserving
// inverse. The mapping is calibration-derived; it is not claimed to be
// measured multi-density Kodak dye physics.
class PrintDyeModel
{
public:
    struct Diagnostics
    {
        std::size_t sample_count = 0;
        float reference_status_a_density = 1.0f;
        FilmDensity minimum_record_density;
        FilmDensity reference_record_increment;
        FilmDensity calibrated_reference_amplitude;
        float reconstruction_rms_error = 0.0f;
        float reconstruction_max_abs_error = 0.0f;
        float reconstruction_max_error_wavelength_nm = 0.0f;
        float residual_min_density = 0.0f;
        float residual_max_density = 0.0f;

        std::size_t growth_mapping_points = 0;
        FilmDensity growth_mapping_input_min;
        FilmDensity growth_mapping_input_max;
    };

    bool build(
        const PrintFilmStock& stock,
        float wavelength_min_nm = 380.0f,
        float wavelength_max_nm = 700.0f,
        float wavelength_step_nm = 5.0f,
        float reference_status_a_density = 1.0f);

    bool valid() const;

    const SampledCurve& visual_neutral_density() const;
    const SampledCurve& cyan_reference_density() const;
    const SampledCurve& magenta_reference_density() const;
    const SampledCurve& yellow_reference_density() const;
    const SampledCurve& neutral_residual_density() const;
    const SampledCurve& cyan_basis_per_record_density() const;
    const SampledCurve& magenta_basis_per_record_density() const;
    const SampledCurve& yellow_basis_per_record_density() const;
    const SampledCurve& reconstructed_visual_neutral() const;
    const SampledCurve& reconstruction_residual() const;
    const Diagnostics& diagnostics() const;

    // Nonlinear-growth synthesis retained explicitly for A/B. The qualified
    // law additionally requires the matched neutral-manifold density at the
    // same linear-reference viewed tone.
    SampledCurve synthesize_density(const FilmDensity& density) const;
    SampledCurve synthesize_transmittance(const FilmDensity& density) const;

    // Qualified production synthesis. Thresholds are fixed by the DigitalSG
    // relative-RMS qualification:
    //   relative RMS <= 0.012 -> exact nonlinear growth
    //   relative RMS >= 0.018 -> exact linear reference
    //   smooth transition only between those bounds.
    SampledCurve synthesize_density_qualified(
        const FilmDensity& density,
        const FilmDensity& neutral_density) const;
    SampledCurve synthesize_transmittance_qualified(
        const FilmDensity& density,
        const FilmDensity& neutral_density) const;


    // Signal-coordinate blend retained for A/B comparison.
    SampledCurve synthesize_density_signal_blended(
        const FilmDensity& density,
        float fade_end) const;
    SampledCurve synthesize_transmittance_signal_blended(
        const FilmDensity& density,
        float fade_end) const;
    FilmDensity blended_reference_amplitudes(
        const FilmDensity& density,
        float fade_end) const;
    float neutrality_distance(const FilmDensity& density) const;

    // Distance is measured relative to the
    // calibrated neutral-record trajectory at matching tone rather than
    // relative to the equal-R/G/B line. The matching neutral density is
    // supplied by the diagnostic caller.
    float neutrality_distance_relative(
        const FilmDensity& density,
        const FilmDensity& neutral_density) const;
    // Alternative diagnostic record-space distances relative to the matched
    // neutral manifold.
    float neutrality_distance_relative_rms(
        const FilmDensity& density,
        const FilmDensity& neutral_density) const;
    float neutrality_distance_relative_log_rms(
        const FilmDensity& density,
        const FilmDensity& neutral_density) const;
    // Relative-RMS blend with an exact nonlinear-growth neutral plateau and
    // exact linear-reference chromatic release.
    FilmDensity blended_reference_amplitudes_relative_rms(
        const FilmDensity& density,
        const FilmDensity& neutral_density,
        float neutral_end,
        float chroma_start) const;
    SampledCurve synthesize_density_relative_rms(
        const FilmDensity& density,
        const FilmDensity& neutral_density,
        float neutral_end,
        float chroma_start) const;
    SampledCurve synthesize_transmittance_relative_rms(
        const FilmDensity& density,
        const FilmDensity& neutral_density,
        float neutral_end,
        float chroma_start) const;
    FilmDensity blended_reference_amplitudes_neutral_relative(
        const FilmDensity& density,
        const FilmDensity& neutral_density,
        float fade_end) const;
    SampledCurve synthesize_density_neutral_relative(
        const FilmDensity& density,
        const FilmDensity& neutral_density,
        float fade_end) const;
    SampledCurve synthesize_transmittance_neutral_relative(
        const FilmDensity& density,
        const FilmDensity& neutral_density,
        float fade_end) const;

    // Clean physical-reference A/B branch.
    //
    // Uses the same Dmin/reference-record coordinate as linear reference:
    //   n = max(0, (D - Dmin) / (Dref - Dmin))
    //
    // but omits the D55-reference unequal C:M:Y metameric amplitudes.
    // Neutral printer operating-point calibration remains owned by
    // PrintFilmProcessor.
    FilmDensity unbiased_reference_amplitudes(
        const FilmDensity& density) const;

    SampledCurve synthesize_density_unbiased(
        const FilmDensity& density) const;

    SampledCurve synthesize_transmittance_unbiased(
        const FilmDensity& density) const;

    // Residual-free, linearly scaled reference model.
    SampledCurve synthesize_density_linear_reference(const FilmDensity& density) const;
    SampledCurve synthesize_transmittance_linear_reference(const FilmDensity& density) const;

    // Earlier residual-based synthesis retained only for explicit diagnostics.
    SampledCurve synthesize_density_legacy(const FilmDensity& density) const;
    SampledCurve synthesize_transmittance_legacy(const FilmDensity& density) const;

    FilmDensity linear_reference_amplitudes(const FilmDensity& density) const;
    FilmDensity mapped_reference_amplitudes(const FilmDensity& density) const;

private:
    SampledCurve synthesize_transmittance_from_amplitudes(
        const FilmDensity& amplitude) const;

    static float sample_growth_mapping(
        const SampledCurve& curve,
        const std::vector<float>& tangents,
        float x);

    static bool build_growth_tangents(
        const SampledCurve& curve,
        std::vector<float>& tangents);

    SampledCurve visual_neutral_density_;
    SampledCurve cyan_reference_density_;
    SampledCurve magenta_reference_density_;
    SampledCurve yellow_reference_density_;
    SampledCurve neutral_residual_density_;
    SampledCurve cyan_basis_per_record_density_;
    SampledCurve magenta_basis_per_record_density_;
    SampledCurve yellow_basis_per_record_density_;
    SampledCurve reconstructed_visual_neutral_;
    SampledCurve reconstruction_residual_;

    SampledCurve cyan_growth_mapping_;
    SampledCurve magenta_growth_mapping_;
    SampledCurve yellow_growth_mapping_;
    std::vector<float> cyan_growth_tangents_;
    std::vector<float> magenta_growth_tangents_;
    std::vector<float> yellow_growth_tangents_;

    Diagnostics diagnostics_;
    bool valid_ = false;
};
