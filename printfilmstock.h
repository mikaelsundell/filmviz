// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "filmstock.h"

#include <string>
#include <vector>

// Source-data container for Kodak 2383 print film.
//
// This class deliberately does no simulation. It owns the five digitized
// Kodak source datasets so they can be validated and plotted independently
// before they are connected to the print-film processing pipeline.
class PrintFilmStock
{
public:
    struct SpectralSensitivity
    {
        SampledCurve yellow_forming_log;
        SampledCurve magenta_forming_log;
        SampledCurve cyan_forming_log;

        // The Kodak sensitivity graph contains two short disconnected traces
        // near 370-400 nm. Their layer identity is intentionally left
        // unresolved until we have a defensible interpretation.
        SampledCurve auxiliary_trace_a_log;
        SampledCurve auxiliary_trace_b_log;
    };

    struct CharacteristicCurves
    {
        SampledCurve red_density;
        SampledCurve green_density;
        SampledCurve blue_density;
    };

    struct SpectralDyeDensity
    {
        SampledCurve visual_neutral_density;
        SampledCurve cyan_density;
        SampledCurve magenta_density;
        SampledCurve yellow_density;
    };

    struct ModulationTransferFunction
    {
        SampledCurve red_response_percent;
        SampledCurve green_response_percent;
        SampledCurve blue_response_percent;
    };

    struct DiffuseRMSGranularity
    {
        // Density traces drawn on the left-hand axis of the Kodak graph.
        SampledCurve red_density;
        SampledCurve green_density;
        SampledCurve blue_density;

        // RMS granularity traces drawn on the logarithmic right-hand axis.
        SampledCurve red_rms;
        SampledCurve green_rms;
        SampledCurve blue_rms;
    };

    PrintFilmStock() = default;
    explicit PrintFilmStock(
        const std::string& name);

    bool load(
        const std::string& sensitivity_filename,
        const std::string& characteristic_filename,
        const std::string& dye_density_filename,
        const std::string& mtf_filename,
        const std::string& granularity_filename);

    bool valid() const;

    const std::string& name() const;

    const SpectralSensitivity&
    sensitivity() const;

    const CharacteristicCurves&
    characteristic() const;

    const SpectralDyeDensity&
    dye_density() const;

    const ModulationTransferFunction&
    mtf() const;

    const DiffuseRMSGranularity&
    granularity() const;

    // Validate SampledCurve interpolation at every original CSV knot.
    std::vector<CurveValidationResult>
    validate_interpolation(
        float tolerance = 1e-6f) const;

private:
    bool load_sensitivity(
        const std::string& filename);

    bool load_characteristic(
        const std::string& filename);

    bool load_dye_density(
        const std::string& filename);

    bool load_mtf(
        const std::string& filename);

    bool load_granularity(
        const std::string& filename);

    std::string name_;

    SpectralSensitivity sensitivity_;
    CharacteristicCurves characteristic_;
    SpectralDyeDensity dye_density_;
    ModulationTransferFunction mtf_;
    DiffuseRMSGranularity granularity_;
};
