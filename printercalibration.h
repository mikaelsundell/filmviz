// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "colorimetry.h"
#include "filmdata.h"
#include "printdyemodel.h"
#include "printfilmprocessor.h"
#include "printviewer.h"

#include <array>

class PrinterCalibration
{
public:
    struct Target
    {
        // Final ACES2065-1 / AP0 linear target after PrintViewer.
        // The calibration target is intentionally expressed in AP0 rather
        // than a display encoding or transfer function.
        std::array<double, 3> ap0 = {{
            0.18,
            0.18,
            0.18
        }};

        Colorimetry::XYZ xyz_d60() const
        {
            return Colorimetry::ap0_to_xyz_d60(ap0);
        }
    };

    struct Settings
    {
        int maximum_iterations = 40;
        double finite_difference_step = 1e-4;
        double damping = 0.75;
        double convergence_tolerance = 1e-9;
        double acceptance_tolerance = 1e-7;
        double singular_determinant_tolerance = 1e-16;
    };

    struct Result
    {
        bool converged = false;

        FilmLogExposure log_offset;
        FilmDensity solved_reference_density;

        PrintViewer::Result solved_view;

        std::array<double, 3> residual = {{
            0.0,
            0.0,
            0.0
        }};

        double residual_norm = 0.0;
        double jacobian_determinant = 0.0;
        double condition_proxy = 0.0;

        int iterations = 0;
    };

    // Convenience overload using the default AP0 0.18 target and
    // default solver settings.
    static Result solve(
        const SampledCurve& reference_negative_transmittance,
        const PrintFilmProcessor& processor,
        const PrintDyeModel& print_dye_model,
        const PrintViewer& print_viewer);

    // Convenience overload using an explicit target and default settings.
    static Result solve(
        const SampledCurve& reference_negative_transmittance,
        const PrintFilmProcessor& processor,
        const PrintDyeModel& print_dye_model,
        const PrintViewer& print_viewer,
        const Target& target);

    // Full solver API.
    static Result solve(
        const SampledCurve& reference_negative_transmittance,
        const PrintFilmProcessor& processor,
        const PrintDyeModel& print_dye_model,
        const PrintViewer& print_viewer,
        const Target& target,
        const Settings& settings);

    static bool apply(
        PrintFilmProcessor& processor,
        const Result& result);
};
