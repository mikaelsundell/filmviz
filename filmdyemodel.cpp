// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdyemodel.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

namespace {

std::vector<std::string>
split_csv_line(
    const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }

    return fields;
}

bool
parse_float(
    const std::string& text,
    float& value)
{
    if (text.empty()) {
        return false;
    }

    try {
        std::size_t pos = 0;
        value = std::stof(text, &pos);

        while (pos < text.size()
               && std::isspace(
                   static_cast<unsigned char>(
                       text[pos]))) {
            ++pos;
        }

        return pos == text.size();
    }
    catch (...) {
        return false;
    }
}

float
log_sensitivity_to_linear(
    float log_sensitivity)
{
    return std::pow(
        10.0f,
        log_sensitivity);
}

struct SensitivityWindow
{
    float center_nm = 0.0f;
    float sigma_nm = 1.0f;
    bool valid = false;
};

SensitivityWindow
measure_sensitivity_window(
    const SampledCurve& log_curve)
{
    SensitivityWindow result;

    if (!log_curve.valid()) {
        return result;
    }

    double weight_sum = 0.0;
    double weighted_x = 0.0;

    for (std::size_t i = 0;
         i < log_curve.x.size();
         ++i) {

        const double weight =
            static_cast<double>(
                log_sensitivity_to_linear(
                    log_curve.y[i]));

        weight_sum += weight;
        weighted_x +=
            weight
            * static_cast<double>(
                log_curve.x[i]);
    }

    if (weight_sum <= 0.0) {
        return result;
    }

    const double center =
        weighted_x / weight_sum;

    double variance_sum = 0.0;

    for (std::size_t i = 0;
         i < log_curve.x.size();
         ++i) {

        const double weight =
            static_cast<double>(
                log_sensitivity_to_linear(
                    log_curve.y[i]));

        const double dx =
            static_cast<double>(
                log_curve.x[i])
            - center;

        variance_sum +=
            weight * dx * dx;
    }

    const double sigma =
        std::sqrt(
            variance_sum / weight_sum);

    if (!std::isfinite(center)
        || !std::isfinite(sigma)
        || sigma <= 1e-6) {
        return result;
    }

    result.center_nm =
        static_cast<float>(center);

    result.sigma_nm =
        static_cast<float>(sigma);

    result.valid = true;
    return result;
}

float
sensitivity_window_weight(
    const SensitivityWindow& window,
    float wavelength_nm)
{
    if (!window.valid) {
        return 0.0f;
    }

    const float z =
        (wavelength_nm - window.center_nm)
        / window.sigma_nm;

    return std::exp(
        -0.5f * z * z);
}

float
peak_wavelength(
    const SampledCurve& curve)
{
    if (!curve.valid()) {
        return 0.0f;
    }

    std::size_t peak_index = 0;

    for (std::size_t i = 1;
         i < curve.y.size();
         ++i) {

        if (curve.y[i] > curve.y[peak_index]) {
            peak_index = i;
        }
    }

    return curve.x[peak_index];
}

float
integrated_area(
    const SampledCurve& curve)
{
    if (!curve.valid()
        || curve.x.size() < 2) {
        return 0.0f;
    }

    double area = 0.0;

    for (std::size_t i = 1;
         i < curve.x.size();
         ++i) {

        const double dx =
            static_cast<double>(
                curve.x[i] - curve.x[i - 1]);

        const double y0 =
            std::max(
                0.0,
                static_cast<double>(
                    curve.y[i - 1]));

        const double y1 =
            std::max(
                0.0,
                static_cast<double>(
                    curve.y[i]));

        area +=
            0.5 * (y0 + y1) * dx;
    }

    return static_cast<float>(area);
}

bool
basic_partition_valid(
    const SampledCurve& neutral_increment,
    const SampledCurve& cyan,
    const SampledCurve& magenta,
    const SampledCurve& yellow,
    const SampledCurve& reconstructed_increment,
    const SampledCurve& reconstructed_midscale,
    const SampledCurve& residual)
{
    return
        neutral_increment.valid()
        && cyan.valid()
        && magenta.valid()
        && yellow.valid()
        && reconstructed_increment.valid()
        && reconstructed_midscale.valid()
        && residual.valid();
}

} // namespace

bool
FilmDyeModel::load_and_estimate(
    const std::string& filename,
    const FilmStock& stock,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm,
    float calibration_log_exposure)
{
    minimum_density_ = SampledCurve();
    midscale_neutral_density_ = SampledCurve();
    neutral_increment_ = SampledCurve();

    cyan_contribution_ = SampledCurve();
    magenta_contribution_ = SampledCurve();
    yellow_contribution_ = SampledCurve();

    cyan_basis_per_record_density_ = SampledCurve();
    magenta_basis_per_record_density_ = SampledCurve();
    yellow_basis_per_record_density_ = SampledCurve();

    reconstructed_increment_ = SampledCurve();
    reconstructed_midscale_ = SampledCurve();
    residual_ = SampledCurve();

    calibrated_reconstructed_midscale_ = SampledCurve();
    calibration_residual_ = SampledCurve();

    diagnostics_ = Diagnostics();
    calibrated_ = false;

    if (!load_reference_density_csv(
            filename)) {
        return false;
    }

    if (!estimate_from_stock(
            stock,
            wavelength_min_nm,
            wavelength_max_nm,
            wavelength_step_nm)) {
        return false;
    }

    return calibrate_to_characteristic(
        stock,
        calibration_log_exposure);
}

bool
FilmDyeModel::valid() const
{
    return
        minimum_density_.valid()
        && midscale_neutral_density_.valid()
        && basic_partition_valid(
            neutral_increment_,
            cyan_contribution_,
            magenta_contribution_,
            yellow_contribution_,
            reconstructed_increment_,
            reconstructed_midscale_,
            residual_)
        && cyan_basis_per_record_density_.valid()
        && magenta_basis_per_record_density_.valid()
        && yellow_basis_per_record_density_.valid()
        && calibrated_reconstructed_midscale_.valid()
        && calibration_residual_.valid()
        && calibrated_;
}

bool
FilmDyeModel::calibrated() const
{
    return calibrated_;
}

const SampledCurve&
FilmDyeModel::minimum_density() const
{
    return minimum_density_;
}

const SampledCurve&
FilmDyeModel::midscale_neutral_density() const
{
    return midscale_neutral_density_;
}

const SampledCurve&
FilmDyeModel::neutral_increment() const
{
    return neutral_increment_;
}

const SampledCurve&
FilmDyeModel::cyan_contribution() const
{
    return cyan_contribution_;
}

const SampledCurve&
FilmDyeModel::magenta_contribution() const
{
    return magenta_contribution_;
}

const SampledCurve&
FilmDyeModel::yellow_contribution() const
{
    return yellow_contribution_;
}

const SampledCurve&
FilmDyeModel::cyan_basis_per_record_density() const
{
    return cyan_basis_per_record_density_;
}

const SampledCurve&
FilmDyeModel::magenta_basis_per_record_density() const
{
    return magenta_basis_per_record_density_;
}

const SampledCurve&
FilmDyeModel::yellow_basis_per_record_density() const
{
    return yellow_basis_per_record_density_;
}

const SampledCurve&
FilmDyeModel::reconstructed_increment() const
{
    return reconstructed_increment_;
}

const SampledCurve&
FilmDyeModel::reconstructed_midscale() const
{
    return reconstructed_midscale_;
}

const SampledCurve&
FilmDyeModel::residual() const
{
    return residual_;
}

const SampledCurve&
FilmDyeModel::calibrated_reconstructed_midscale() const
{
    return calibrated_reconstructed_midscale_;
}

const SampledCurve&
FilmDyeModel::calibration_residual() const
{
    return calibration_residual_;
}

const FilmDyeModel::Diagnostics&
FilmDyeModel::diagnostics() const
{
    return diagnostics_;
}

bool
FilmDyeModel::load_reference_density_csv(
    const std::string& filename)
{
    std::ifstream file(
        filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open spectral dye density CSV: "
            << filename
            << std::endl;
        return false;
    }

    std::string line;

    if (!std::getline(file, line)) {
        std::cerr
            << "error: empty spectral dye density CSV: "
            << filename
            << std::endl;
        return false;
    }

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.size() < 3) {
            continue;
        }

        float wavelength_nm = 0.0f;
        float minimum_density = 0.0f;
        float midscale_density = 0.0f;

        if (!parse_float(
                fields[0],
                wavelength_nm)
            || !parse_float(
                fields[1],
                minimum_density)
            || !parse_float(
                fields[2],
                midscale_density)) {
            continue;
        }

        minimum_density_.x.push_back(
            wavelength_nm);
        minimum_density_.y.push_back(
            minimum_density);

        midscale_neutral_density_.x.push_back(
            wavelength_nm);
        midscale_neutral_density_.y.push_back(
            midscale_density);
    }

    if (!minimum_density_.valid()
        || !midscale_neutral_density_.valid()) {

        std::cerr
            << "error: spectral dye density CSV did not contain usable "
               "minimum and midscale-neutral curves"
            << std::endl;
        return false;
    }

    return true;
}

bool
FilmDyeModel::estimate_from_stock(
    const FilmStock& stock,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm)
{
    if (!stock.valid()
        || wavelength_step_nm <= 0.0f
        || wavelength_max_nm < wavelength_min_nm) {
        return false;
    }

    const float start_nm =
        std::max(
            wavelength_min_nm,
            std::max(
                minimum_density_.x.front(),
                midscale_neutral_density_.x.front()));

    const float end_nm =
        std::min(
            wavelength_max_nm,
            std::min(
                minimum_density_.x.back(),
                midscale_neutral_density_.x.back()));

    const auto& sensitivity =
        stock.sensitivity();

    const SensitivityWindow cyan_window =
        measure_sensitivity_window(
            sensitivity.red_sensitive_log);

    const SensitivityWindow magenta_window =
        measure_sensitivity_window(
            sensitivity.green_sensitive_log);

    const SensitivityWindow yellow_window =
        measure_sensitivity_window(
            sensitivity.blue_sensitive_log);

    if (!cyan_window.valid
        || !magenta_window.valid
        || !yellow_window.valid) {
        return false;
    }

    diagnostics_.cyan_prior_center_nm =
        cyan_window.center_nm;
    diagnostics_.magenta_prior_center_nm =
        magenta_window.center_nm;
    diagnostics_.yellow_prior_center_nm =
        yellow_window.center_nm;

    diagnostics_.cyan_prior_sigma_nm =
        cyan_window.sigma_nm;
    diagnostics_.magenta_prior_sigma_nm =
        magenta_window.sigma_nm;
    diagnostics_.yellow_prior_sigma_nm =
        yellow_window.sigma_nm;

    double squared_error_sum = 0.0;
    float max_abs_error = 0.0f;
    float max_error_wavelength_nm = 0.0f;
    std::size_t count = 0;

    for (float wavelength_nm = start_nm;
         wavelength_nm <= end_nm + 0.001f;
         wavelength_nm += wavelength_step_nm) {

        const float dmin =
            minimum_density_.sample(
                wavelength_nm,
                0.0f);

        const float dmid =
            midscale_neutral_density_.sample(
                wavelength_nm,
                0.0f);

        const float increment =
            std::max(
                0.0f,
                dmid - dmin);

        const float cyan_weight_raw =
            sensitivity_window_weight(
                cyan_window,
                wavelength_nm);

        const float magenta_weight_raw =
            sensitivity_window_weight(
                magenta_window,
                wavelength_nm);

        const float yellow_weight_raw =
            sensitivity_window_weight(
                yellow_window,
                wavelength_nm);

        const float weight_sum =
            cyan_weight_raw
            + magenta_weight_raw
            + yellow_weight_raw;

        if (weight_sum <= 1e-20f) {
            return false;
        }

        const float cyan =
            increment
            * cyan_weight_raw
            / weight_sum;

        const float magenta =
            increment
            * magenta_weight_raw
            / weight_sum;

        const float yellow =
            increment
            * yellow_weight_raw
            / weight_sum;

        const float reconstructed_increment =
            cyan + magenta + yellow;

        const float reconstructed_midscale =
            dmin + reconstructed_increment;

        const float error =
            reconstructed_midscale - dmid;

        neutral_increment_.x.push_back(
            wavelength_nm);
        neutral_increment_.y.push_back(
            increment);

        cyan_contribution_.x.push_back(
            wavelength_nm);
        cyan_contribution_.y.push_back(
            cyan);

        magenta_contribution_.x.push_back(
            wavelength_nm);
        magenta_contribution_.y.push_back(
            magenta);

        yellow_contribution_.x.push_back(
            wavelength_nm);
        yellow_contribution_.y.push_back(
            yellow);

        reconstructed_increment_.x.push_back(
            wavelength_nm);
        reconstructed_increment_.y.push_back(
            reconstructed_increment);

        reconstructed_midscale_.x.push_back(
            wavelength_nm);
        reconstructed_midscale_.y.push_back(
            reconstructed_midscale);

        residual_.x.push_back(
            wavelength_nm);
        residual_.y.push_back(
            error);

        squared_error_sum +=
            static_cast<double>(error)
            * static_cast<double>(error);

        const float abs_error =
            std::abs(error);

        if (abs_error > max_abs_error) {
            max_abs_error = abs_error;
            max_error_wavelength_nm = wavelength_nm;
        }

        ++count;
    }

    if (count == 0
        || !basic_partition_valid(
            neutral_increment_,
            cyan_contribution_,
            magenta_contribution_,
            yellow_contribution_,
            reconstructed_increment_,
            reconstructed_midscale_,
            residual_)) {
        return false;
    }

    diagnostics_.sample_count = count;
    diagnostics_.rms_reconstruction_error =
        static_cast<float>(
            std::sqrt(
                squared_error_sum
                / static_cast<double>(count)));

    diagnostics_.max_abs_reconstruction_error =
        max_abs_error;

    diagnostics_.max_error_wavelength_nm =
        max_error_wavelength_nm;

    diagnostics_.cyan_peak_wavelength_nm =
        peak_wavelength(
            cyan_contribution_);

    diagnostics_.magenta_peak_wavelength_nm =
        peak_wavelength(
            magenta_contribution_);

    diagnostics_.yellow_peak_wavelength_nm =
        peak_wavelength(
            yellow_contribution_);

    const float cyan_area =
        integrated_area(
            cyan_contribution_);

    const float magenta_area =
        integrated_area(
            magenta_contribution_);

    const float yellow_area =
        integrated_area(
            yellow_contribution_);

    const float total_area =
        cyan_area
        + magenta_area
        + yellow_area;

    if (total_area > 1e-20f) {
        diagnostics_.cyan_integrated_fraction =
            cyan_area / total_area;

        diagnostics_.magenta_integrated_fraction =
            magenta_area / total_area;

        diagnostics_.yellow_integrated_fraction =
            yellow_area / total_area;
    }

    return true;
}

bool
FilmDyeModel::calibrate_to_characteristic(
    const FilmStock& stock,
    float calibration_log_exposure)
{
    if (!basic_partition_valid(
            neutral_increment_,
            cyan_contribution_,
            magenta_contribution_,
            yellow_contribution_,
            reconstructed_increment_,
            reconstructed_midscale_,
            residual_)) {
        return false;
    }

    const auto& curves =
        stock.characteristic();

    diagnostics_.calibration_log_exposure =
        calibration_log_exposure;

    diagnostics_.minimum_record_density.red =
        curves.red_density.y.front();
    diagnostics_.minimum_record_density.green =
        curves.green_density.y.front();
    diagnostics_.minimum_record_density.blue =
        curves.blue_density.y.front();

    diagnostics_.calibration_record_density.red =
        sample_characteristic_clamped(
            curves.red_density,
            calibration_log_exposure);

    diagnostics_.calibration_record_density.green =
        sample_characteristic_clamped(
            curves.green_density,
            calibration_log_exposure);

    diagnostics_.calibration_record_density.blue =
        sample_characteristic_clamped(
            curves.blue_density,
            calibration_log_exposure);

    diagnostics_.calibration_record_increment.red =
        diagnostics_.calibration_record_density.red
        - diagnostics_.minimum_record_density.red;

    diagnostics_.calibration_record_increment.green =
        diagnostics_.calibration_record_density.green
        - diagnostics_.minimum_record_density.green;

    diagnostics_.calibration_record_increment.blue =
        diagnostics_.calibration_record_density.blue
        - diagnostics_.minimum_record_density.blue;

    const FilmDensity& reference_increment =
        diagnostics_.calibration_record_increment;

    if (reference_increment.red <= 1e-8f
        || reference_increment.green <= 1e-8f
        || reference_increment.blue <= 1e-8f) {

        std::cerr
            << "error: dye basis calibration reference density increment "
               "is not positive in all three records"
            << std::endl;
        return false;
    }

    double squared_error_sum = 0.0;
    float max_abs_error = 0.0f;
    float max_error_wavelength_nm = 0.0f;
    std::size_t count = 0;

    for (std::size_t i = 0;
         i < neutral_increment_.x.size();
         ++i) {

        const float wavelength_nm =
            neutral_increment_.x[i];

        const float cyan_basis =
            cyan_contribution_.y[i]
            / reference_increment.red;

        const float magenta_basis =
            magenta_contribution_.y[i]
            / reference_increment.green;

        const float yellow_basis =
            yellow_contribution_.y[i]
            / reference_increment.blue;

        cyan_basis_per_record_density_.x.push_back(
            wavelength_nm);
        cyan_basis_per_record_density_.y.push_back(
            cyan_basis);

        magenta_basis_per_record_density_.x.push_back(
            wavelength_nm);
        magenta_basis_per_record_density_.y.push_back(
            magenta_basis);

        yellow_basis_per_record_density_.x.push_back(
            wavelength_nm);
        yellow_basis_per_record_density_.y.push_back(
            yellow_basis);

        const float dmin =
            minimum_density_.sample(
                wavelength_nm,
                0.0f);

        const float reconstructed =
            dmin
            + reference_increment.red
                * cyan_basis
            + reference_increment.green
                * magenta_basis
            + reference_increment.blue
                * yellow_basis;

        const float measured =
            midscale_neutral_density_.sample(
                wavelength_nm,
                0.0f);

        const float error =
            reconstructed - measured;

        calibrated_reconstructed_midscale_.x.push_back(
            wavelength_nm);
        calibrated_reconstructed_midscale_.y.push_back(
            reconstructed);

        calibration_residual_.x.push_back(
            wavelength_nm);
        calibration_residual_.y.push_back(
            error);

        squared_error_sum +=
            static_cast<double>(error)
            * static_cast<double>(error);

        const float abs_error =
            std::abs(error);

        if (abs_error > max_abs_error) {
            max_abs_error = abs_error;
            max_error_wavelength_nm = wavelength_nm;
        }

        ++count;
    }

    if (count == 0
        || !cyan_basis_per_record_density_.valid()
        || !magenta_basis_per_record_density_.valid()
        || !yellow_basis_per_record_density_.valid()
        || !calibrated_reconstructed_midscale_.valid()
        || !calibration_residual_.valid()) {
        return false;
    }

    diagnostics_.calibration_rms_error =
        static_cast<float>(
            std::sqrt(
                squared_error_sum
                / static_cast<double>(count)));

    diagnostics_.calibration_max_abs_error =
        max_abs_error;

    diagnostics_.calibration_max_error_wavelength_nm =
        max_error_wavelength_nm;

    calibrated_ = true;
    return true;
}

SampledCurve
FilmDyeModel::synthesize_density(
    const FilmDensity& spectral_coordinate) const
{
    SampledCurve result;

    if (!calibrated_) {
        return result;
    }

    const FilmDensity& minimum_record =
        diagnostics_.minimum_record_density;

    const float red_increment =
        std::max(
            0.0f,
            spectral_coordinate.red - minimum_record.red);

    const float green_increment =
        std::max(
            0.0f,
            spectral_coordinate.green - minimum_record.green);

    const float blue_increment =
        std::max(
            0.0f,
            spectral_coordinate.blue - minimum_record.blue);

    for (std::size_t i = 0;
         i < cyan_basis_per_record_density_.x.size();
         ++i) {

        const float wavelength_nm =
            cyan_basis_per_record_density_.x[i];

        const float dmin =
            minimum_density_.sample(
                wavelength_nm,
                0.0f);

        const float total_density =
            dmin
            + red_increment
                * cyan_basis_per_record_density_.y[i]
            + green_increment
                * magenta_basis_per_record_density_.y[i]
            + blue_increment
                * yellow_basis_per_record_density_.y[i];

        result.x.push_back(
            wavelength_nm);

        result.y.push_back(
            std::max(
                0.0f,
                total_density));
    }

    return result;
}

SampledCurve
FilmDyeModel::synthesize_transmittance(
    const FilmDensity& spectral_coordinate) const
{
    const SampledCurve spectral_density =
        synthesize_density(
            spectral_coordinate);

    SampledCurve result;

    if (!spectral_density.valid()) {
        return result;
    }

    result.x = spectral_density.x;
    result.y.reserve(
        spectral_density.y.size());

    for (float density_value :
         spectral_density.y) {

        result.y.push_back(
            std::pow(
                10.0f,
                -density_value));
    }

    return result;
}

FilmDensity
FilmDyeModel::neutral_record_density(
    const FilmStock& stock,
    float log_exposure) const
{
    const auto& curves =
        stock.characteristic();

    FilmDensity result;

    result.red =
        sample_characteristic_clamped(
            curves.red_density,
            log_exposure);

    result.green =
        sample_characteristic_clamped(
            curves.green_density,
            log_exposure);

    result.blue =
        sample_characteristic_clamped(
            curves.blue_density,
            log_exposure);

    return result;
}

SampledCurve
FilmDyeModel::synthesize_neutral_density(
    const FilmStock& stock,
    float log_exposure) const
{
    return synthesize_density(
        neutral_record_density(
            stock,
            log_exposure));
}

SampledCurve
FilmDyeModel::synthesize_neutral_transmittance(
    const FilmStock& stock,
    float log_exposure) const
{
    return synthesize_transmittance(
        neutral_record_density(
            stock,
            log_exposure));
}

float
FilmDyeModel::sample_characteristic_clamped(
    const SampledCurve& curve,
    float log_exposure)
{
    if (!curve.valid()) {
        return 0.0f;
    }

    if (log_exposure <= curve.x.front()) {
        return curve.y.front();
    }

    if (log_exposure >= curve.x.back()) {
        return curve.y.back();
    }

    return curve.sample(
        log_exposure,
        curve.y.front());
}
