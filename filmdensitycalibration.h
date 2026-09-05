// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "statusmdensitometer.h"

#include <array>

class FilmDyeModel;

// Converts published negative-film densitometry coordinates into the internal
// FilmDyeModel spectral-basis coordinates.
//
// Why this exists:
// Kodak's characteristic curves are Status-M measurements, while FilmDyeModel
// synthesizes a wavelength-dependent optical-density spectrum. measurement-validation diagnostics
// established that these coordinate systems are not numerically identical.
// This class solves the nonlinear inverse problem so that a synthesized
// negative, remeasured with Status M, reproduces the requested characteristic-
// curve density excursion around the exact measured neutral spectral anchor.
//
// This is a calibration/coordinate conversion, not a creative contrast or
// colour-look control. Targets outside the spectral model's achievable gamut
// are mapped to the nearest finite least-squares state; the measured spectral
// D-min is enforced as the lower physical boundary.
class FilmDensityCalibration
{
public:
    using Vec3 = std::array<double, 3>;

    struct Settings
    {
        int iterations = 24;
        int line_search_steps = 8;
        double jacobian_step = 1e-3;
        double convergence_tolerance = 1e-6;
        double levenberg_marquardt_lambda = 1e-5;
        double maximum_update = 0.5;
    };

    struct Result
    {
        FilmDensity calibrated_density;
        Vec3 desired_status_m = {{0.0, 0.0, 0.0}};
        Vec3 measured_status_m = {{0.0, 0.0, 0.0}};
        Vec3 residual = {{0.0, 0.0, 0.0}};
        bool floor_projected = false;
        bool converged = false;
        bool valid = false;
    };

    FilmDensityCalibration(
        const FilmDyeModel& model,
        const FilmDensity& zero_target,
        const SampledCurve& minimum_spectral_density);

    FilmDensityCalibration(
        const FilmDyeModel& model,
        const FilmDensity& zero_target,
        const SampledCurve& minimum_spectral_density,
        const Settings& settings);

    bool valid() const;

    Result solve(
        const FilmDensity& target_status_m) const;

    bool calibrate(
        const FilmDensity& target_status_m,
        FilmDensity& calibrated_density) const;

    const Vec3& zero_target() const;
    const Vec3& zero_measured() const;
    const Vec3& minimum_status_m() const;
    const std::array<std::array<double, 3>, 3>& zero_jacobian() const;
    const Settings& settings() const;

private:
    using Mat3 = std::array<std::array<double, 3>, 3>;

    static Vec3 to_vec3(
        const FilmDensity& density);

    static FilmDensity to_density(
        const Vec3& value);

    static bool finite(
        const Vec3& value);

    static Vec3 add(
        const Vec3& a,
        const Vec3& b);

    static Vec3 subtract(
        const Vec3& a,
        const Vec3& b);

    static double rms(
        const Vec3& value);

    static double max_abs(
        const Vec3& value);

    static bool solve_3x3(
        Mat3 matrix,
        Vec3 rhs,
        Vec3& solution);

    const FilmDyeModel& model_;
    StatusMDensitometer densitometer_;
    Settings settings_;
    Vec3 zero_target_ = {{0.0, 0.0, 0.0}};
    Vec3 zero_measured_ = {{0.0, 0.0, 0.0}};
    Vec3 minimum_status_m_ = {{0.0, 0.0, 0.0}};
    Mat3 zero_jacobian_ = {{
        {{0.0, 0.0, 0.0}},
        {{0.0, 0.0, 0.0}},
        {{0.0, 0.0, 0.0}}
    }};
    bool valid_ = false;
};
