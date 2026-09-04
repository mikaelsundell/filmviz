// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdensitycalibration.h"

#include "filmdyemodel.h"

#include <algorithm>
#include <cmath>

FilmDensityCalibration::FilmDensityCalibration(
    const FilmDyeModel& model,
    const FilmDensity& zero_target,
    const SampledCurve& minimum_spectral_density)
    : FilmDensityCalibration(
          model,
          zero_target,
          minimum_spectral_density,
          Settings())
{
}

FilmDensityCalibration::FilmDensityCalibration(
    const FilmDyeModel& model,
    const FilmDensity& zero_target,
    const SampledCurve& minimum_spectral_density,
    const Settings& settings)
    : model_(model)
    , settings_(settings)
    , zero_target_(
          to_vec3(
              zero_target))
    , minimum_status_m_(
          densitometer_.measure(
              minimum_spectral_density))
{
    const SampledCurve zero_spectrum =
        model_.synthesize_density(
            zero_target);

    zero_measured_ =
        densitometer_.measure(
            zero_spectrum);

    if (!zero_spectrum.valid()
        || !finite(zero_measured_)
        || !finite(minimum_status_m_)
        || settings_.jacobian_step <= 0.0) {

        return;
    }

    for (int input_channel = 0;
         input_channel < 3;
         ++input_channel) {

        Vec3 perturbed =
            zero_target_;

        perturbed[input_channel] +=
            settings_.jacobian_step;

        const SampledCurve spectrum =
            model_.synthesize_density(
                to_density(
                    perturbed));

        const Vec3 measured =
            densitometer_.measure(
                spectrum);

        if (!spectrum.valid()
            || !finite(measured)) {

            return;
        }

        for (int output_channel = 0;
             output_channel < 3;
             ++output_channel) {

            zero_jacobian_[output_channel][input_channel] =
                (measured[output_channel]
                 - zero_measured_[output_channel])
                / settings_.jacobian_step;
        }
    }

    Vec3 probe;

    valid_ =
        solve_3x3(
            zero_jacobian_,
            {{1.0, 0.0, 0.0}},
            probe);
}

bool
FilmDensityCalibration::valid() const
{
    return valid_;
}

FilmDensityCalibration::Result
FilmDensityCalibration::solve(
    const FilmDensity& target_status_m) const
{
    Result result;

    if (!valid_) {
        return result;
    }

    const Vec3 target =
        to_vec3(
            target_status_m);

    Vec3 desired =
        add(
            zero_measured_,
            subtract(
                target,
                zero_target_));

    for (int channel = 0;
         channel < 3;
         ++channel) {

        if (desired[channel]
            < minimum_status_m_[channel]) {

            desired[channel] =
                minimum_status_m_[channel];

            result.floor_projected = true;
        }
    }

    result.desired_status_m =
        desired;

    const Vec3 effective_delta =
        subtract(
            desired,
            zero_measured_);

    Vec3 input_delta;

    if (!solve_3x3(
            zero_jacobian_,
            effective_delta,
            input_delta)) {

        return result;
    }

    Vec3 input =
        add(
            zero_target_,
            input_delta);

    for (int iteration = 0;
         iteration < settings_.iterations;
         ++iteration) {

        const SampledCurve spectrum =
            model_.synthesize_density(
                to_density(
                    input));

        const Vec3 measured =
            densitometer_.measure(
                spectrum);

        if (!spectrum.valid()
            || !finite(measured)) {

            return result;
        }

        const Vec3 residual =
            subtract(
                desired,
                measured);

        if (max_abs(residual)
            <= settings_.convergence_tolerance) {

            result.calibrated_density =
                to_density(
                    input);

            result.measured_status_m =
                measured;

            result.residual =
                residual;

            result.converged = true;
            result.valid = true;

            return result;
        }

        Mat3 jacobian = {{
            {{0.0, 0.0, 0.0}},
            {{0.0, 0.0, 0.0}},
            {{0.0, 0.0, 0.0}}
        }};

        for (int input_channel = 0;
             input_channel < 3;
             ++input_channel) {

            Vec3 perturbed =
                input;

            perturbed[input_channel] +=
                settings_.jacobian_step;

            const SampledCurve perturbed_spectrum =
                model_.synthesize_density(
                    to_density(
                        perturbed));

            const Vec3 perturbed_measured =
                densitometer_.measure(
                    perturbed_spectrum);

            if (!perturbed_spectrum.valid()
                || !finite(perturbed_measured)) {

                return result;
            }

            for (int output_channel = 0;
                 output_channel < 3;
                 ++output_channel) {

                jacobian[output_channel][input_channel] =
                    (perturbed_measured[output_channel]
                     - measured[output_channel])
                    / settings_.jacobian_step;
            }
        }

        Mat3 normal = {{
            {{0.0, 0.0, 0.0}},
            {{0.0, 0.0, 0.0}},
            {{0.0, 0.0, 0.0}}
        }};

        Vec3 rhs = {{0.0, 0.0, 0.0}};

        for (int row = 0;
             row < 3;
             ++row) {

            for (int column = 0;
                 column < 3;
                 ++column) {

                for (int output = 0;
                     output < 3;
                     ++output) {

                    normal[row][column] +=
                        jacobian[output][row]
                        * jacobian[output][column];
                }
            }

            for (int output = 0;
                 output < 3;
                 ++output) {

                rhs[row] +=
                    jacobian[output][row]
                    * residual[output];
            }
        }

        for (int channel = 0;
             channel < 3;
             ++channel) {

            normal[channel][channel] +=
                settings_.levenberg_marquardt_lambda;
        }

        Vec3 update;

        if (!solve_3x3(
                normal,
                rhs,
                update)) {

            break;
        }

        const double maximum_update =
            std::max(
                1e-12,
                max_abs(
                    update));

        double line_scale =
            maximum_update > settings_.maximum_update
                ? settings_.maximum_update / maximum_update
                : 1.0;

        const double residual_before =
            rms(
                residual);

        bool accepted = false;

        for (int line_search = 0;
             line_search < settings_.line_search_steps;
             ++line_search) {

            Vec3 candidate =
                input;

            for (int channel = 0;
                 channel < 3;
                 ++channel) {

                candidate[channel] +=
                    line_scale
                    * update[channel];
            }

            const SampledCurve candidate_spectrum =
                model_.synthesize_density(
                    to_density(
                        candidate));

            const Vec3 candidate_measured =
                densitometer_.measure(
                    candidate_spectrum);

            if (candidate_spectrum.valid()
                && finite(candidate_measured)) {

                const Vec3 candidate_residual =
                    subtract(
                        desired,
                        candidate_measured);

                if (rms(candidate_residual)
                    < residual_before) {

                    input = candidate;
                    accepted = true;
                    break;
                }
            }

            line_scale *= 0.5;
        }

        if (!accepted) {
            break;
        }
    }

    // Full RGB LUTs intentionally contain chromatic targets that can lie
    // outside the spectral model's exactly invertible Status-M gamut. Keep
    // the best finite least-squares state rather than turning a physically
    // meaningful gamut projection into a hard failure.
    const SampledCurve final_spectrum =
        model_.synthesize_density(
            to_density(
                input));

    const Vec3 final_measured =
        densitometer_.measure(
            final_spectrum);

    if (!final_spectrum.valid()
        || !finite(final_measured)) {

        return result;
    }

    result.calibrated_density =
        to_density(
            input);

    result.measured_status_m =
        final_measured;

    result.residual =
        subtract(
            desired,
            final_measured);

    result.valid =
        finite(
            result.residual);

    return result;
}

bool
FilmDensityCalibration::calibrate(
    const FilmDensity& target_status_m,
    FilmDensity& calibrated_density) const
{
    const Result result =
        solve(
            target_status_m);

    if (!result.valid) {
        return false;
    }

    calibrated_density =
        result.calibrated_density;

    return true;
}

const FilmDensityCalibration::Vec3&
FilmDensityCalibration::zero_target() const
{
    return zero_target_;
}

const FilmDensityCalibration::Vec3&
FilmDensityCalibration::zero_measured() const
{
    return zero_measured_;
}

const FilmDensityCalibration::Vec3&
FilmDensityCalibration::minimum_status_m() const
{
    return minimum_status_m_;
}

const std::array<std::array<double, 3>, 3>&
FilmDensityCalibration::zero_jacobian() const
{
    return zero_jacobian_;
}

const FilmDensityCalibration::Settings&
FilmDensityCalibration::settings() const
{
    return settings_;
}

FilmDensityCalibration::Vec3
FilmDensityCalibration::to_vec3(
    const FilmDensity& density)
{
    return {{
        static_cast<double>(density.red),
        static_cast<double>(density.green),
        static_cast<double>(density.blue)
    }};
}

FilmDensity
FilmDensityCalibration::to_density(
    const Vec3& value)
{
    FilmDensity result;

    result.red =
        static_cast<float>(
            value[0]);

    result.green =
        static_cast<float>(
            value[1]);

    result.blue =
        static_cast<float>(
            value[2]);

    return result;
}

bool
FilmDensityCalibration::finite(
    const Vec3& value)
{
    return
        std::isfinite(value[0])
        && std::isfinite(value[1])
        && std::isfinite(value[2]);
}

FilmDensityCalibration::Vec3
FilmDensityCalibration::add(
    const Vec3& a,
    const Vec3& b)
{
    return {{
        a[0] + b[0],
        a[1] + b[1],
        a[2] + b[2]
    }};
}

FilmDensityCalibration::Vec3
FilmDensityCalibration::subtract(
    const Vec3& a,
    const Vec3& b)
{
    return {{
        a[0] - b[0],
        a[1] - b[1],
        a[2] - b[2]
    }};
}

double
FilmDensityCalibration::rms(
    const Vec3& value)
{
    return
        std::sqrt(
            (value[0] * value[0]
             + value[1] * value[1]
             + value[2] * value[2])
            / 3.0);
}

double
FilmDensityCalibration::max_abs(
    const Vec3& value)
{
    return
        std::max(
            std::abs(value[0]),
            std::max(
                std::abs(value[1]),
                std::abs(value[2])));
}

bool
FilmDensityCalibration::solve_3x3(
    Mat3 matrix,
    Vec3 rhs,
    Vec3& solution)
{
    for (int column = 0;
         column < 3;
         ++column) {

        int pivot = column;

        for (int row = column + 1;
             row < 3;
             ++row) {

            if (std::abs(matrix[row][column])
                > std::abs(matrix[pivot][column])) {

                pivot = row;
            }
        }

        if (std::abs(matrix[pivot][column])
            < 1e-12) {

            return false;
        }

        if (pivot != column) {
            std::swap(
                matrix[pivot],
                matrix[column]);

            std::swap(
                rhs[pivot],
                rhs[column]);
        }

        const double divisor =
            matrix[column][column];

        for (int j = column;
             j < 3;
             ++j) {

            matrix[column][j] /=
                divisor;
        }

        rhs[column] /=
            divisor;

        for (int row = 0;
             row < 3;
             ++row) {

            if (row == column) {
                continue;
            }

            const double factor =
                matrix[row][column];

            for (int j = column;
                 j < 3;
                 ++j) {

                matrix[row][j] -=
                    factor
                    * matrix[column][j];
            }

            rhs[row] -=
                factor
                * rhs[column];
        }
    }

    solution = rhs;

    return true;
}
