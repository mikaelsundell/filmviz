// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "printercalibration.h"

#include "colorimetry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

double
norm3(
    const std::array<double, 3>& value)
{
    return std::sqrt(
        value[0] * value[0]
        + value[1] * value[1]
        + value[2] * value[2]);
}

} // namespace

PrinterCalibration::Result
PrinterCalibration::solve(
    const SampledCurve& reference_negative_transmittance,
    const PrintFilmProcessor& processor,
    const PrintDyeModel& print_dye_model,
    const PrintViewer& print_viewer)
{
    return solve(
        reference_negative_transmittance,
        processor,
        print_dye_model,
        print_viewer,
        Target(),
        Settings());
}

PrinterCalibration::Result
PrinterCalibration::solve(
    const SampledCurve& reference_negative_transmittance,
    const PrintFilmProcessor& processor,
    const PrintDyeModel& print_dye_model,
    const PrintViewer& print_viewer,
    const Target& target)
{
    return solve(
        reference_negative_transmittance,
        processor,
        print_dye_model,
        print_viewer,
        target,
        Settings());
}

PrinterCalibration::Result
PrinterCalibration::solve(
    const SampledCurve& reference_negative_transmittance,
    const PrintFilmProcessor& processor,
    const PrintDyeModel& print_dye_model,
    const PrintViewer& print_viewer,
    const Target& target,
    const Settings& settings)
{
    Result result;

    if (!reference_negative_transmittance.valid()
        || !processor.valid()
        || !print_dye_model.valid()
        || !print_viewer.valid()) {

        return result;
    }

    const FilmExposure raw_reference_exposure =
        processor.expose(
            reference_negative_transmittance);

    if (raw_reference_exposure.red <= 0.0f
        || raw_reference_exposure.green <= 0.0f
        || raw_reference_exposure.blue <= 0.0f) {

        return result;
    }

    const auto& processor_settings =
        processor.settings();

    auto evaluate =
        [&](const std::array<double, 3>& offset,
            FilmDensity* density_out = nullptr)
            -> PrintViewer::Result
        {
            // Printer calibration deliberately excludes exposure_stops.
            // User print exposure stays an independent runtime control.
            FilmLogExposure log_exposure;

            log_exposure.red =
                static_cast<float>(
                    std::log10(
                        std::max(
                            static_cast<double>(
                                raw_reference_exposure.red),
                            1e-20))
                    + static_cast<double>(
                        processor_settings.log_exposure_calibration)
                    + offset[0]);

            log_exposure.green =
                static_cast<float>(
                    std::log10(
                        std::max(
                            static_cast<double>(
                                raw_reference_exposure.green),
                            1e-20))
                    + static_cast<double>(
                        processor_settings.log_exposure_calibration)
                    + offset[1]);

            log_exposure.blue =
                static_cast<float>(
                    std::log10(
                        std::max(
                            static_cast<double>(
                                raw_reference_exposure.blue),
                            1e-20))
                    + static_cast<double>(
                        processor_settings.log_exposure_calibration)
                    + offset[2]);

            const FilmDensity density =
                processor.develop(
                    log_exposure);

            if (density_out) {
                *density_out =
                    density;
            }

            const SampledCurve transmittance =
                print_dye_model
                    .synthesize_transmittance_unbiased(
                        density);

            return
                print_viewer.view(
                    transmittance);
        };

    // PrintViewer returns final linear ACES2065-1 / AP0. Calibration is
    // therefore solved in that linear color space; no transfer function or
    // display encoding belongs in this solver.
    auto residual_for =
        [&](const PrintViewer::Result& view)
            -> std::array<double, 3>
        {
            return {{
                static_cast<double>(
                    view.aces2065_1[0])
                    - target.ap0[0],

                static_cast<double>(
                    view.aces2065_1[1])
                    - target.ap0[1],

                static_cast<double>(
                    view.aces2065_1[2])
                    - target.ap0[2]
            }};
        };

    const auto& legacy_balance =
        processor.balance();

    std::array<double, 3> offset = {{
        static_cast<double>(
            legacy_balance.red_log_offset),

        static_cast<double>(
            legacy_balance.green_log_offset),

        static_cast<double>(
            legacy_balance.blue_log_offset)
    }};

    for (int iteration = 0;
         iteration < settings.maximum_iterations;
         ++iteration) {

        result.iterations =
            iteration + 1;

        const PrintViewer::Result center =
            evaluate(
                offset);

        const auto residual =
            residual_for(
                center);

        result.residual_norm =
            norm3(
                residual);

        if (result.residual_norm
            < settings.convergence_tolerance) {

            result.converged =
                true;

            break;
        }

        Colorimetry::Matrix3 jacobian;

        for (int column = 0;
             column < 3;
             ++column) {

            std::array<double, 3> probe =
                offset;

            probe[column] +=
                settings.finite_difference_step;

            const auto probe_residual =
                residual_for(
                    evaluate(
                        probe));

            for (int row = 0;
                 row < 3;
                 ++row) {

                jacobian.m[row][column] =
                    (
                        probe_residual[row]
                        - residual[row]
                    )
                    / settings.finite_difference_step;
            }
        }

        result.jacobian_determinant =
            Colorimetry::determinant(
                jacobian);

        double max_entry =
            0.0;

        double min_nonzero_entry =
            std::numeric_limits<double>::infinity();

        for (int row = 0;
             row < 3;
             ++row) {

            for (int column = 0;
                 column < 3;
                 ++column) {

                const double value =
                    std::abs(
                        jacobian.m[row][column]);

                max_entry =
                    std::max(
                        max_entry,
                        value);

                if (value > 1e-12) {
                    min_nonzero_entry =
                        std::min(
                            min_nonzero_entry,
                            value);
                }
            }
        }

        result.condition_proxy =
            std::isfinite(
                min_nonzero_entry)
            && min_nonzero_entry > 0.0
                ? max_entry
                    / min_nonzero_entry
                : std::numeric_limits<double>::infinity();

        if (std::abs(
                result.jacobian_determinant)
            < settings.singular_determinant_tolerance) {

            break;
        }

        const double rhs[3] = {
            -residual[0],
            -residual[1],
            -residual[2]
        };

        double delta[3] = {
            0.0,
            0.0,
            0.0
        };

        for (int column = 0;
             column < 3;
             ++column) {

            Colorimetry::Matrix3 matrix =
                jacobian;

            for (int row = 0;
                 row < 3;
                 ++row) {

                matrix.m[row][column] =
                    rhs[row];
            }

            delta[column] =
                Colorimetry::determinant(
                    matrix)
                / result.jacobian_determinant;
        }

        for (int channel = 0;
             channel < 3;
             ++channel) {

            offset[channel] +=
                settings.damping
                * delta[channel];
        }
    }

    result.log_offset.red =
        static_cast<float>(
            offset[0]);

    result.log_offset.green =
        static_cast<float>(
            offset[1]);

    result.log_offset.blue =
        static_cast<float>(
            offset[2]);

    result.solved_view =
        evaluate(
            offset,
            &result.solved_reference_density);

    result.residual =
        residual_for(
            result.solved_view);

    result.residual_norm =
        norm3(
            result.residual);

    result.converged =
        result.residual_norm
        < settings.acceptance_tolerance;

    return result;
}

bool
PrinterCalibration::apply(
    PrintFilmProcessor& processor,
    const Result& result)
{
    if (!processor.valid()
        || !result.converged) {

        return false;
    }

    processor.set_balance_log_offsets(
        result.log_offset.red,
        result.log_offset.green,
        result.log_offset.blue);

    return true;
}
