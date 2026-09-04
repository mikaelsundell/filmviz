// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "printdyemodel.h"
#include "printdyegrowthdata.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace {

float
curve_minimum(
    const SampledCurve& curve)
{
    if (!curve.valid()) {
        return 0.0f;
    }

    return *std::min_element(
        curve.y.begin(),
        curve.y.end());
}

float
curve_maximum(
    const SampledCurve& curve)
{
    if (!curve.valid()) {
        return 0.0f;
    }

    return *std::max_element(
        curve.y.begin(),
        curve.y.end());
}

} // namespace

bool
PrintDyeModel::build(
    const PrintFilmStock& stock,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm,
    float reference_status_a_density)
{
    visual_neutral_density_ = SampledCurve();
    cyan_reference_density_ = SampledCurve();
    magenta_reference_density_ = SampledCurve();
    yellow_reference_density_ = SampledCurve();

    neutral_residual_density_ = SampledCurve();

    cyan_basis_per_record_density_ = SampledCurve();
    magenta_basis_per_record_density_ = SampledCurve();
    yellow_basis_per_record_density_ = SampledCurve();

    reconstructed_visual_neutral_ = SampledCurve();
    reconstruction_residual_ = SampledCurve();

    cyan_growth_mapping_ = SampledCurve();
    magenta_growth_mapping_ = SampledCurve();
    yellow_growth_mapping_ = SampledCurve();
    cyan_growth_tangents_.clear();
    magenta_growth_tangents_.clear();
    yellow_growth_tangents_.clear();

    diagnostics_ = Diagnostics();
    valid_ = false;

    if (!stock.valid()
        || wavelength_step_nm <= 0.0f
        || wavelength_max_nm < wavelength_min_nm
        || reference_status_a_density <= 0.0f) {

        std::cerr
            << "error: invalid Kodak 2383 dye-model settings"
            << std::endl;
        return false;
    }

    const auto& source =
        stock.dye_density();

    const auto& characteristic =
        stock.characteristic();

    diagnostics_.reference_status_a_density =
        reference_status_a_density;

    diagnostics_.minimum_record_density.red =
        characteristic.red_density.y.front();

    diagnostics_.minimum_record_density.green =
        characteristic.green_density.y.front();

    diagnostics_.minimum_record_density.blue =
        characteristic.blue_density.y.front();

    diagnostics_.reference_record_increment.red =
        reference_status_a_density
        - diagnostics_.minimum_record_density.red;

    diagnostics_.reference_record_increment.green =
        reference_status_a_density
        - diagnostics_.minimum_record_density.green;

    diagnostics_.reference_record_increment.blue =
        reference_status_a_density
        - diagnostics_.minimum_record_density.blue;

    // Prototype 18 D55/CIE 1931 colorimetric reference calibration.
    // These amplitudes make the three peak-normalized Kodak dye shapes a
    // metameric match to the measured Visual Neutral reference under the
    // current viewing condition. They are not claimed to be absolute physical
    // dye concentrations.
    diagnostics_.calibrated_reference_amplitude.red = 1.18937325f;
    diagnostics_.calibrated_reference_amplitude.green = 1.07063532f;
    diagnostics_.calibrated_reference_amplitude.blue = 1.15189266f;

    if (diagnostics_.reference_record_increment.red <= 1e-6f
        || diagnostics_.reference_record_increment.green <= 1e-6f
        || diagnostics_.reference_record_increment.blue <= 1e-6f) {

        std::cerr
            << "error: 2383 reference density must be above all three D-min values"
            << std::endl;
        return false;
    }

    double squared_error_sum = 0.0;
    float max_abs_error = 0.0f;
    float max_error_wavelength_nm = 0.0f;
    std::size_t count = 0;

    for (float wavelength_nm = wavelength_min_nm;
         wavelength_nm <= wavelength_max_nm + 0.001f;
         wavelength_nm += wavelength_step_nm) {

        // All four published dye curves cover the active 380-700 nm interval.
        const float visual =
            source.visual_neutral_density.sample(
                wavelength_nm,
                std::numeric_limits<float>::quiet_NaN());

        const float cyan =
            source.cyan_density.sample(
                wavelength_nm,
                std::numeric_limits<float>::quiet_NaN());

        const float magenta =
            source.magenta_density.sample(
                wavelength_nm,
                std::numeric_limits<float>::quiet_NaN());

        const float yellow =
            source.yellow_density.sample(
                wavelength_nm,
                std::numeric_limits<float>::quiet_NaN());

        if (!std::isfinite(visual)
            || !std::isfinite(cyan)
            || !std::isfinite(magenta)
            || !std::isfinite(yellow)) {

            std::cerr
                << "error: 2383 dye-density data does not cover wavelength "
                << wavelength_nm
                << " nm"
                << std::endl;
            return false;
        }

        // Preserve Kodak's measured difference rather than pretending it is
        // independently measured film-base density.
        const float neutral_residual =
            visual
            - cyan
            - magenta
            - yellow;

        visual_neutral_density_.x.push_back(wavelength_nm);
        visual_neutral_density_.y.push_back(visual);

        cyan_reference_density_.x.push_back(wavelength_nm);
        cyan_reference_density_.y.push_back(cyan);

        magenta_reference_density_.x.push_back(wavelength_nm);
        magenta_reference_density_.y.push_back(magenta);

        yellow_reference_density_.x.push_back(wavelength_nm);
        yellow_reference_density_.y.push_back(yellow);

        neutral_residual_density_.x.push_back(wavelength_nm);
        neutral_residual_density_.y.push_back(neutral_residual);

        const float cyan_basis =
            diagnostics_.calibrated_reference_amplitude.red
            * cyan
            / diagnostics_.reference_record_increment.red;

        const float magenta_basis =
            diagnostics_.calibrated_reference_amplitude.green
            * magenta
            / diagnostics_.reference_record_increment.green;

        const float yellow_basis =
            diagnostics_.calibrated_reference_amplitude.blue
            * yellow
            / diagnostics_.reference_record_increment.blue;

        cyan_basis_per_record_density_.x.push_back(wavelength_nm);
        cyan_basis_per_record_density_.y.push_back(cyan_basis);

        magenta_basis_per_record_density_.x.push_back(wavelength_nm);
        magenta_basis_per_record_density_.y.push_back(magenta_basis);

        yellow_basis_per_record_density_.x.push_back(wavelength_nm);
        yellow_basis_per_record_density_.y.push_back(yellow_basis);

        const float reconstructed =
            diagnostics_.reference_record_increment.red
                * cyan_basis
            + diagnostics_.reference_record_increment.green
                * magenta_basis
            + diagnostics_.reference_record_increment.blue
                * yellow_basis;

        const float error =
            reconstructed - visual;

        reconstructed_visual_neutral_.x.push_back(wavelength_nm);
        reconstructed_visual_neutral_.y.push_back(reconstructed);

        reconstruction_residual_.x.push_back(wavelength_nm);
        reconstruction_residual_.y.push_back(error);

        const float abs_error =
            std::abs(error);

        squared_error_sum +=
            static_cast<double>(error)
            * static_cast<double>(error);

        if (abs_error > max_abs_error) {
            max_abs_error = abs_error;
            max_error_wavelength_nm = wavelength_nm;
        }

        ++count;
    }

    diagnostics_.sample_count =
        count;

    diagnostics_.reconstruction_rms_error =
        count > 0
            ? static_cast<float>(
                std::sqrt(
                    squared_error_sum
                    / static_cast<double>(count)))
            : 0.0f;

    diagnostics_.reconstruction_max_abs_error =
        max_abs_error;

    diagnostics_.reconstruction_max_error_wavelength_nm =
        max_error_wavelength_nm;

    diagnostics_.residual_min_density =
        curve_minimum(
            neutral_residual_density_);

    diagnostics_.residual_max_density =
        curve_maximum(
            neutral_residual_density_);

    for (std::size_t i = 0; i < 161; ++i) {
        cyan_growth_mapping_.x.push_back(kCyan_x[i]);
        cyan_growth_mapping_.y.push_back(kCyan_y[i]);
        magenta_growth_mapping_.x.push_back(kMagenta_x[i]);
        magenta_growth_mapping_.y.push_back(kMagenta_y[i]);
        yellow_growth_mapping_.x.push_back(kYellow_x[i]);
        yellow_growth_mapping_.y.push_back(kYellow_y[i]);
    }

    diagnostics_.growth_mapping_points = 161;
    diagnostics_.growth_mapping_input_min.red = cyan_growth_mapping_.x.front();
    diagnostics_.growth_mapping_input_min.green = magenta_growth_mapping_.x.front();
    diagnostics_.growth_mapping_input_min.blue = yellow_growth_mapping_.x.front();
    diagnostics_.growth_mapping_input_max.red = cyan_growth_mapping_.x.back();
    diagnostics_.growth_mapping_input_max.green = magenta_growth_mapping_.x.back();
    diagnostics_.growth_mapping_input_max.blue = yellow_growth_mapping_.x.back();

    const bool growth_tangents_valid =
        build_growth_tangents(cyan_growth_mapping_, cyan_growth_tangents_)
        && build_growth_tangents(magenta_growth_mapping_, magenta_growth_tangents_)
        && build_growth_tangents(yellow_growth_mapping_, yellow_growth_tangents_);

    valid_ =
        visual_neutral_density_.valid()
        && cyan_reference_density_.valid()
        && magenta_reference_density_.valid()
        && yellow_reference_density_.valid()
        && neutral_residual_density_.valid()
        && cyan_basis_per_record_density_.valid()
        && magenta_basis_per_record_density_.valid()
        && yellow_basis_per_record_density_.valid()
        && reconstructed_visual_neutral_.valid()
        && reconstruction_residual_.valid()
        && cyan_growth_mapping_.valid()
        && magenta_growth_mapping_.valid()
        && yellow_growth_mapping_.valid()
        && growth_tangents_valid;

    return valid_;
}

bool
PrintDyeModel::valid() const
{
    return valid_;
}

const SampledCurve&
PrintDyeModel::visual_neutral_density() const
{
    return visual_neutral_density_;
}

const SampledCurve&
PrintDyeModel::cyan_reference_density() const
{
    return cyan_reference_density_;
}

const SampledCurve&
PrintDyeModel::magenta_reference_density() const
{
    return magenta_reference_density_;
}

const SampledCurve&
PrintDyeModel::yellow_reference_density() const
{
    return yellow_reference_density_;
}

const SampledCurve&
PrintDyeModel::neutral_residual_density() const
{
    return neutral_residual_density_;
}

const SampledCurve&
PrintDyeModel::cyan_basis_per_record_density() const
{
    return cyan_basis_per_record_density_;
}

const SampledCurve&
PrintDyeModel::magenta_basis_per_record_density() const
{
    return magenta_basis_per_record_density_;
}

const SampledCurve&
PrintDyeModel::yellow_basis_per_record_density() const
{
    return yellow_basis_per_record_density_;
}

const SampledCurve&
PrintDyeModel::reconstructed_visual_neutral() const
{
    return reconstructed_visual_neutral_;
}

const SampledCurve&
PrintDyeModel::reconstruction_residual() const
{
    return reconstruction_residual_;
}

const PrintDyeModel::Diagnostics&
PrintDyeModel::diagnostics() const
{
    return diagnostics_;
}

bool
PrintDyeModel::build_growth_tangents(
    const SampledCurve& curve,
    std::vector<float>& tangents)
{
    const std::size_t n = curve.x.size();
    tangents.assign(n, 0.0f);
    if (n < 2 || curve.y.size() != n) {
        return false;
    }

    std::vector<float> h(n - 1, 0.0f);
    std::vector<float> delta(n - 1, 0.0f);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        h[i] = curve.x[i + 1] - curve.x[i];
        if (h[i] <= 0.0f) {
            return false;
        }
        delta[i] = (curve.y[i + 1] - curve.y[i]) / h[i];
    }

    if (n == 2) {
        tangents[0] = tangents[1] = delta[0];
        return true;
    }

    tangents[0] = ((2.0f * h[0] + h[1]) * delta[0] - h[0] * delta[1])
        / (h[0] + h[1]);
    if (tangents[0] * delta[0] <= 0.0f) {
        tangents[0] = 0.0f;
    }
    else if (delta[0] * delta[1] < 0.0f
             && std::abs(tangents[0]) > std::abs(3.0f * delta[0])) {
        tangents[0] = 3.0f * delta[0];
    }

    for (std::size_t i = 1; i + 1 < n; ++i) {
        if (delta[i - 1] * delta[i] <= 0.0f) {
            tangents[i] = 0.0f;
        }
        else {
            const float w1 = 2.0f * h[i] + h[i - 1];
            const float w2 = h[i] + 2.0f * h[i - 1];
            tangents[i] = (w1 + w2)
                / (w1 / delta[i - 1] + w2 / delta[i]);
        }
    }

    const std::size_t last = n - 1;
    tangents[last] = ((2.0f * h[last - 1] + h[last - 2]) * delta[last - 1]
                      - h[last - 1] * delta[last - 2])
        / (h[last - 1] + h[last - 2]);
    if (tangents[last] * delta[last - 1] <= 0.0f) {
        tangents[last] = 0.0f;
    }
    else if (delta[last - 1] * delta[last - 2] < 0.0f
             && std::abs(tangents[last]) > std::abs(3.0f * delta[last - 1])) {
        tangents[last] = 3.0f * delta[last - 1];
    }

    return true;
}

float
PrintDyeModel::sample_growth_mapping(
    const SampledCurve& curve,
    const std::vector<float>& tangents,
    float x)
{
    const std::size_t n = curve.x.size();
    if (n < 2 || curve.y.size() != n || tangents.size() != n) {
        return std::max(0.0f, x);
    }

    // Prototype 24 qualified the mapping only over this calibrated domain.
    // Preserve that exact qualification behavior by clamping to the endpoint
    // values instead of inventing an unvalidated extrapolation law.
    if (x <= curve.x.front()) {
        return curve.y.front();
    }
    if (x >= curve.x.back()) {
        return curve.y.back();
    }

    const auto upper = std::upper_bound(curve.x.begin(), curve.x.end(), x);
    std::size_t interval = static_cast<std::size_t>(upper - curve.x.begin() - 1);
    interval = std::min(interval, n - 2);

    const float h = curve.x[interval + 1] - curve.x[interval];
    const float t = (x - curve.x[interval]) / h;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;

    return std::max(0.0f,
        h00 * curve.y[interval]
        + h10 * h * tangents[interval]
        + h01 * curve.y[interval + 1]
        + h11 * h * tangents[interval + 1]);
}

FilmDensity
PrintDyeModel::unbiased_reference_amplitudes(
    const FilmDensity& density) const
{
    FilmDensity result;

    if (!valid_) {
        return result;
    }

    const FilmDensity& minimum =
        diagnostics_.minimum_record_density;

    const FilmDensity& reference_increment =
        diagnostics_.reference_record_increment;

    result.red =
        reference_increment.red > 1e-12f
            ? std::max(
                  0.0f,
                  (density.red - minimum.red)
                      / reference_increment.red)
            : 0.0f;

    result.green =
        reference_increment.green > 1e-12f
            ? std::max(
                  0.0f,
                  (density.green - minimum.green)
                      / reference_increment.green)
            : 0.0f;

    result.blue =
        reference_increment.blue > 1e-12f
            ? std::max(
                  0.0f,
                  (density.blue - minimum.blue)
                      / reference_increment.blue)
            : 0.0f;

    return result;
}

FilmDensity
PrintDyeModel::linear_reference_amplitudes(
    const FilmDensity& density) const
{
    FilmDensity result;
    if (!valid_) {
        return result;
    }

    const FilmDensity& minimum = diagnostics_.minimum_record_density;
    const FilmDensity& reference_increment = diagnostics_.reference_record_increment;
    const FilmDensity& reference_amplitude = diagnostics_.calibrated_reference_amplitude;

    const float nr = reference_increment.red > 1e-12f
        ? std::max(0.0f, (density.red - minimum.red) / reference_increment.red) : 0.0f;
    const float ng = reference_increment.green > 1e-12f
        ? std::max(0.0f, (density.green - minimum.green) / reference_increment.green) : 0.0f;
    const float nb = reference_increment.blue > 1e-12f
        ? std::max(0.0f, (density.blue - minimum.blue) / reference_increment.blue) : 0.0f;

    result.red = reference_amplitude.red * nr;
    result.green = reference_amplitude.green * ng;
    result.blue = reference_amplitude.blue * nb;
    return result;
}

FilmDensity
PrintDyeModel::mapped_reference_amplitudes(
    const FilmDensity& density) const
{
    FilmDensity result;
    if (!valid_) {
        return result;
    }

    const FilmDensity& minimum = diagnostics_.minimum_record_density;
    const FilmDensity& reference_increment = diagnostics_.reference_record_increment;
    const FilmDensity& reference_amplitude = diagnostics_.calibrated_reference_amplitude;

    const float nr = reference_increment.red > 1e-12f
        ? std::max(0.0f, (density.red - minimum.red) / reference_increment.red) : 0.0f;
    const float ng = reference_increment.green > 1e-12f
        ? std::max(0.0f, (density.green - minimum.green) / reference_increment.green) : 0.0f;
    const float nb = reference_increment.blue > 1e-12f
        ? std::max(0.0f, (density.blue - minimum.blue) / reference_increment.blue) : 0.0f;

    result.red = reference_amplitude.red
        * sample_growth_mapping(cyan_growth_mapping_, cyan_growth_tangents_, nr);
    result.green = reference_amplitude.green
        * sample_growth_mapping(magenta_growth_mapping_, magenta_growth_tangents_, ng);
    result.blue = reference_amplitude.blue
        * sample_growth_mapping(yellow_growth_mapping_, yellow_growth_tangents_, nb);
    return result;
}

float
PrintDyeModel::neutrality_distance(
    const FilmDensity& density) const
{
    if (!valid_) {
        return 0.0f;
    }

    const FilmDensity& minimum = diagnostics_.minimum_record_density;
    const FilmDensity& reference_increment = diagnostics_.reference_record_increment;

    const float nr = reference_increment.red > 1e-12f
        ? std::max(0.0f, (density.red - minimum.red) / reference_increment.red) : 0.0f;
    const float ng = reference_increment.green > 1e-12f
        ? std::max(0.0f, (density.green - minimum.green) / reference_increment.green) : 0.0f;
    const float nb = reference_increment.blue > 1e-12f
        ? std::max(0.0f, (density.blue - minimum.blue) / reference_increment.blue) : 0.0f;

    const float mean = (nr + ng + nb) / 3.0f;
    if (mean <= 1e-12f) {
        return 0.0f;
    }

    const float lo = std::min(nr, std::min(ng, nb));
    const float hi = std::max(nr, std::max(ng, nb));
    return std::max(0.0f, (hi - lo) / mean);
}

FilmDensity
PrintDyeModel::blended_reference_amplitudes(
    const FilmDensity& density,
    float fade_end) const
{
    const FilmDensity p19 = linear_reference_amplitudes(density);
    const FilmDensity p25 = mapped_reference_amplitudes(density);

    FilmDensity result = p19;
    if (!valid_) {
        return result;
    }

    const float end = std::max(1e-6f, fade_end);
    const float d = neutrality_distance(density);
    const float t = std::max(0.0f, std::min(1.0f, d / end));
    const float smooth = t * t * (3.0f - 2.0f * t);
    const float weight = 1.0f - smooth;

    result.red = p19.red + weight * (p25.red - p19.red);
    result.green = p19.green + weight * (p25.green - p19.green);
    result.blue = p19.blue + weight * (p25.blue - p19.blue);
    return result;
}


SampledCurve
PrintDyeModel::synthesize_transmittance_from_amplitudes(
    const FilmDensity& amplitude) const
{
    SampledCurve result;

    if (!valid_) {
        return result;
    }

    result.x = cyan_reference_density_.x;
    result.y.reserve(
        cyan_reference_density_.y.size());

    for (std::size_t i = 0;
         i < cyan_reference_density_.y.size();
         ++i) {

        const float total_density =
            amplitude.red * cyan_reference_density_.y[i]
            + amplitude.green * magenta_reference_density_.y[i]
            + amplitude.blue * yellow_reference_density_.y[i];

        result.y.push_back(
            std::pow(
                10.0f,
                -std::max(
                    0.0f,
                    total_density)));
    }

    return result;
}

SampledCurve
PrintDyeModel::synthesize_density_p26(
    const FilmDensity& density,
    float fade_end) const
{
    SampledCurve result;
    if (!valid_) {
        return result;
    }

    const FilmDensity amplitude = blended_reference_amplitudes(density, fade_end);
    for (std::size_t i = 0; i < cyan_reference_density_.x.size(); ++i) {
        const float total_density =
            amplitude.red * cyan_reference_density_.y[i]
            + amplitude.green * magenta_reference_density_.y[i]
            + amplitude.blue * yellow_reference_density_.y[i];
        result.x.push_back(cyan_reference_density_.x[i]);
        result.y.push_back(std::max(0.0f, total_density));
    }
    return result;
}

SampledCurve
PrintDyeModel::synthesize_transmittance_p26(
    const FilmDensity& density,
    float fade_end) const
{
    const SampledCurve spectral_density = synthesize_density_p26(density, fade_end);
    SampledCurve result;
    if (!spectral_density.valid()) {
        return result;
    }
    result.x = spectral_density.x;
    result.y.reserve(spectral_density.y.size());
    for (float value : spectral_density.y) {
        result.y.push_back(std::pow(10.0f, -value));
    }
    return result;
}


float
PrintDyeModel::neutrality_distance_relative(
    const FilmDensity& density,
    const FilmDensity& neutral_density) const
{
    if (!valid_) {
        return 0.0f;
    }

    const FilmDensity& minimum = diagnostics_.minimum_record_density;
    const FilmDensity& reference_increment = diagnostics_.reference_record_increment;

    const auto normalized = [&](float value, float min_value, float increment) {
        return increment > 1e-12f
            ? std::max(0.0f, (value - min_value) / increment)
            : 0.0f;
    };

    const float nr = normalized(density.red, minimum.red, reference_increment.red);
    const float ng = normalized(density.green, minimum.green, reference_increment.green);
    const float nb = normalized(density.blue, minimum.blue, reference_increment.blue);
    const float rr = normalized(neutral_density.red, minimum.red, reference_increment.red);
    const float rg = normalized(neutral_density.green, minimum.green, reference_increment.green);
    const float rb = normalized(neutral_density.blue, minimum.blue, reference_increment.blue);

    // Relative coordinates collapse the calibrated neutral manifold to
    // approximately (1,1,1) at every tone.  Clamp only the denominator to
    // keep the metric finite at the extreme toe.
    const float qr = nr / std::max(1e-6f, rr);
    const float qg = ng / std::max(1e-6f, rg);
    const float qb = nb / std::max(1e-6f, rb);
    const float mean = (qr + qg + qb) / 3.0f;
    if (mean <= 1e-12f) {
        return 0.0f;
    }

    const float lo = std::min(qr, std::min(qg, qb));
    const float hi = std::max(qr, std::max(qg, qb));
    return std::max(0.0f, (hi - lo) / mean);
}


float
PrintDyeModel::neutrality_distance_relative_rms(
    const FilmDensity& density,
    const FilmDensity& neutral_density) const
{
    if (!valid_) {
        return 0.0f;
    }

    const FilmDensity& minimum = diagnostics_.minimum_record_density;
    const FilmDensity& reference_increment = diagnostics_.reference_record_increment;
    const auto normalized = [&](float value, float min_value, float increment) {
        return increment > 1e-12f
            ? std::max(0.0f, (value - min_value) / increment)
            : 0.0f;
    };

    const float q[3] = {
        normalized(density.red, minimum.red, reference_increment.red)
            / std::max(1e-6f, normalized(neutral_density.red, minimum.red, reference_increment.red)),
        normalized(density.green, minimum.green, reference_increment.green)
            / std::max(1e-6f, normalized(neutral_density.green, minimum.green, reference_increment.green)),
        normalized(density.blue, minimum.blue, reference_increment.blue)
            / std::max(1e-6f, normalized(neutral_density.blue, minimum.blue, reference_increment.blue))
    };
    const float mean = (q[0] + q[1] + q[2]) / 3.0f;
    if (mean <= 1e-12f) {
        return 0.0f;
    }
    const float d0 = q[0] / mean - 1.0f;
    const float d1 = q[1] / mean - 1.0f;
    const float d2 = q[2] / mean - 1.0f;
    return std::sqrt((d0*d0 + d1*d1 + d2*d2) / 3.0f);
}

float
PrintDyeModel::neutrality_distance_relative_log_rms(
    const FilmDensity& density,
    const FilmDensity& neutral_density) const
{
    if (!valid_) {
        return 0.0f;
    }

    const FilmDensity& minimum = diagnostics_.minimum_record_density;
    const FilmDensity& reference_increment = diagnostics_.reference_record_increment;
    const auto normalized = [&](float value, float min_value, float increment) {
        return increment > 1e-12f
            ? std::max(0.0f, (value - min_value) / increment)
            : 0.0f;
    };

    const float q0 = std::max(1e-6f,
        normalized(density.red, minimum.red, reference_increment.red)
        / std::max(1e-6f, normalized(neutral_density.red, minimum.red, reference_increment.red)));
    const float q1 = std::max(1e-6f,
        normalized(density.green, minimum.green, reference_increment.green)
        / std::max(1e-6f, normalized(neutral_density.green, minimum.green, reference_increment.green)));
    const float q2 = std::max(1e-6f,
        normalized(density.blue, minimum.blue, reference_increment.blue)
        / std::max(1e-6f, normalized(neutral_density.blue, minimum.blue, reference_increment.blue)));

    const float l0 = std::log(q0);
    const float l1 = std::log(q1);
    const float l2 = std::log(q2);
    const float mean = (l0 + l1 + l2) / 3.0f;
    const float d0 = l0 - mean;
    const float d1 = l1 - mean;
    const float d2 = l2 - mean;
    return std::sqrt((d0*d0 + d1*d1 + d2*d2) / 3.0f);
}


FilmDensity
PrintDyeModel::blended_reference_amplitudes_p29(
    const FilmDensity& density,
    const FilmDensity& neutral_density,
    float neutral_end,
    float chroma_start) const
{
    const FilmDensity p19 = linear_reference_amplitudes(density);
    const FilmDensity p25 = mapped_reference_amplitudes(density);

    FilmDensity result = p19;
    if (!valid_) {
        return result;
    }

    const float start = std::max(0.0f, neutral_end);
    const float end = std::max(start + 1e-6f, chroma_start);
    const float d = neutrality_distance_relative_rms(density, neutral_density);

    float weight = 0.0f;
    if (d <= start) {
        weight = 1.0f;
    }
    else if (d >= end) {
        weight = 0.0f;
    }
    else {
        const float t = (d - start) / (end - start);
        const float smooth = t * t * (3.0f - 2.0f * t);
        weight = 1.0f - smooth;
    }

    result.red = p19.red + weight * (p25.red - p19.red);
    result.green = p19.green + weight * (p25.green - p19.green);
    result.blue = p19.blue + weight * (p25.blue - p19.blue);
    return result;
}

SampledCurve
PrintDyeModel::synthesize_density_p29(
    const FilmDensity& density,
    const FilmDensity& neutral_density,
    float neutral_end,
    float chroma_start) const
{
    SampledCurve result;
    if (!valid_) {
        return result;
    }

    const FilmDensity amplitude = blended_reference_amplitudes_p29(
        density, neutral_density, neutral_end, chroma_start);

    for (std::size_t i = 0; i < cyan_reference_density_.x.size(); ++i) {
        const float total_density =
            amplitude.red * cyan_reference_density_.y[i]
            + amplitude.green * magenta_reference_density_.y[i]
            + amplitude.blue * yellow_reference_density_.y[i];

        result.x.push_back(cyan_reference_density_.x[i]);
        result.y.push_back(std::max(0.0f, total_density));
    }

    return result;
}

SampledCurve
PrintDyeModel::synthesize_transmittance_p29(
    const FilmDensity& density,
    const FilmDensity& neutral_density,
    float neutral_end,
    float chroma_start) const
{
    return synthesize_transmittance_from_amplitudes(
        blended_reference_amplitudes_p29(
            density,
            neutral_density,
            neutral_end,
            chroma_start));
}

FilmDensity
PrintDyeModel::blended_reference_amplitudes_p27(
    const FilmDensity& density,
    const FilmDensity& neutral_density,
    float fade_end) const
{
    const FilmDensity p19 = linear_reference_amplitudes(density);
    const FilmDensity p25 = mapped_reference_amplitudes(density);

    FilmDensity result = p19;
    if (!valid_) {
        return result;
    }

    const float end = std::max(1e-6f, fade_end);
    const float d = neutrality_distance_relative(density, neutral_density);
    const float t = std::max(0.0f, std::min(1.0f, d / end));
    const float smooth = t * t * (3.0f - 2.0f * t);
    const float weight = 1.0f - smooth;

    result.red = p19.red + weight * (p25.red - p19.red);
    result.green = p19.green + weight * (p25.green - p19.green);
    result.blue = p19.blue + weight * (p25.blue - p19.blue);
    return result;
}

SampledCurve
PrintDyeModel::synthesize_density_p27(
    const FilmDensity& density,
    const FilmDensity& neutral_density,
    float fade_end) const
{
    SampledCurve result;
    if (!valid_) {
        return result;
    }

    const FilmDensity amplitude = blended_reference_amplitudes_p27(
        density, neutral_density, fade_end);
    for (std::size_t i = 0; i < cyan_reference_density_.x.size(); ++i) {
        const float total_density =
            amplitude.red * cyan_reference_density_.y[i]
            + amplitude.green * magenta_reference_density_.y[i]
            + amplitude.blue * yellow_reference_density_.y[i];
        result.x.push_back(cyan_reference_density_.x[i]);
        result.y.push_back(std::max(0.0f, total_density));
    }
    return result;
}

SampledCurve
PrintDyeModel::synthesize_transmittance_p27(
    const FilmDensity& density,
    const FilmDensity& neutral_density,
    float fade_end) const
{
    const SampledCurve spectral_density = synthesize_density_p27(
        density, neutral_density, fade_end);
    SampledCurve result;
    if (!spectral_density.valid()) {
        return result;
    }
    result.x = spectral_density.x;
    result.y.reserve(spectral_density.y.size());
    for (float value : spectral_density.y) {
        result.y.push_back(std::pow(10.0f, -value));
    }
    return result;
}


SampledCurve
PrintDyeModel::synthesize_density_p30(
    const FilmDensity& density,
    const FilmDensity& neutral_density) const
{
    static const float kNeutralEnd = 0.012f;
    static const float kChromaStart = 0.018f;

    return synthesize_density_p29(
        density,
        neutral_density,
        kNeutralEnd,
        kChromaStart);
}

SampledCurve
PrintDyeModel::synthesize_transmittance_p30(
    const FilmDensity& density,
    const FilmDensity& neutral_density) const
{
    static const float kNeutralEnd = 0.012f;
    static const float kChromaStart = 0.018f;

    return synthesize_transmittance_p29(
        density,
        neutral_density,
        kNeutralEnd,
        kChromaStart);
}

SampledCurve
PrintDyeModel::synthesize_density(
    const FilmDensity& density) const
{
    SampledCurve result;
    if (!valid_) {
        return result;
    }

    const FilmDensity amplitude = mapped_reference_amplitudes(density);
    for (std::size_t i = 0; i < cyan_reference_density_.x.size(); ++i) {
        const float total_density =
            amplitude.red * cyan_reference_density_.y[i]
            + amplitude.green * magenta_reference_density_.y[i]
            + amplitude.blue * yellow_reference_density_.y[i];
        result.x.push_back(cyan_reference_density_.x[i]);
        result.y.push_back(std::max(0.0f, total_density));
    }
    return result;
}

SampledCurve
PrintDyeModel::synthesize_transmittance(
    const FilmDensity& density) const
{
    return synthesize_transmittance_from_amplitudes(
        mapped_reference_amplitudes(
            density));
}

SampledCurve
PrintDyeModel::synthesize_density_unbiased(
    const FilmDensity& density) const
{
    SampledCurve result;

    if (!valid_) {
        return result;
    }

    const FilmDensity amplitude =
        unbiased_reference_amplitudes(
            density);

    for (std::size_t i = 0;
         i < cyan_reference_density_.x.size();
         ++i) {

        const float total_density =
            amplitude.red
                * cyan_reference_density_.y[i]
            + amplitude.green
                * magenta_reference_density_.y[i]
            + amplitude.blue
                * yellow_reference_density_.y[i];

        result.x.push_back(
            cyan_reference_density_.x[i]);

        result.y.push_back(
            std::max(
                0.0f,
                total_density));
    }

    return result;
}

SampledCurve
PrintDyeModel::synthesize_transmittance_unbiased(
    const FilmDensity& density) const
{
    return
        synthesize_transmittance_from_amplitudes(
            unbiased_reference_amplitudes(
                density));
}

SampledCurve
PrintDyeModel::synthesize_density_p19(
    const FilmDensity& density) const
{
    SampledCurve result;
    if (!valid_) {
        return result;
    }

    const FilmDensity amplitude = linear_reference_amplitudes(density);
    for (std::size_t i = 0; i < cyan_reference_density_.x.size(); ++i) {
        const float total_density =
            amplitude.red * cyan_reference_density_.y[i]
            + amplitude.green * magenta_reference_density_.y[i]
            + amplitude.blue * yellow_reference_density_.y[i];
        result.x.push_back(cyan_reference_density_.x[i]);
        result.y.push_back(std::max(0.0f, total_density));
    }
    return result;
}

SampledCurve
PrintDyeModel::synthesize_transmittance_p19(
    const FilmDensity& density) const
{
    return synthesize_transmittance_from_amplitudes(
        linear_reference_amplitudes(
            density));
}

SampledCurve
PrintDyeModel::synthesize_density_legacy(
    const FilmDensity& density) const
{
    SampledCurve result;

    if (!valid_) {
        return result;
    }

    const FilmDensity& minimum = diagnostics_.minimum_record_density;

    const float red_increment = std::max(0.0f, density.red - minimum.red);
    const float green_increment = std::max(0.0f, density.green - minimum.green);
    const float blue_increment = std::max(0.0f, density.blue - minimum.blue);

    for (std::size_t i = 0; i < neutral_residual_density_.x.size(); ++i) {
        // Reconstruct the pre-Prototype-19 basis by undoing the calibrated
        // amplitude factors now embedded in the active per-record bases.
        const float legacy_cyan_basis =
            cyan_basis_per_record_density_.y[i]
            / diagnostics_.calibrated_reference_amplitude.red;
        const float legacy_magenta_basis =
            magenta_basis_per_record_density_.y[i]
            / diagnostics_.calibrated_reference_amplitude.green;
        const float legacy_yellow_basis =
            yellow_basis_per_record_density_.y[i]
            / diagnostics_.calibrated_reference_amplitude.blue;

        const float total_density =
            neutral_residual_density_.y[i]
            + red_increment * legacy_cyan_basis
            + green_increment * legacy_magenta_basis
            + blue_increment * legacy_yellow_basis;

        result.x.push_back(neutral_residual_density_.x[i]);
        result.y.push_back(std::max(0.0f, total_density));
    }

    return result;
}

SampledCurve
PrintDyeModel::synthesize_transmittance_legacy(
    const FilmDensity& density) const
{
    const SampledCurve spectral_density = synthesize_density_legacy(density);
    SampledCurve result;
    if (!spectral_density.valid()) {
        return result;
    }
    result.x = spectral_density.x;
    result.y.reserve(spectral_density.y.size());
    for (float value : spectral_density.y) {
        result.y.push_back(std::pow(10.0f, -value));
    }
    return result;
}
