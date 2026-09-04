// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmprocessor.h"

#include <algorithm>
#include <cmath>

namespace {

float
clamp_nonnegative(
    float value)
{
    return std::max(
        0.0f,
        value);
}

struct RGBSpectrum
{
    float coeff[RGB2SPEC_N_COEFFS];
    float scale = 1.0f;
};

RGBSpectrum
reconstruct_rgb_spectrum(
    RGB2Spec* model,
    const std::array<float, 3>& input_rgb)
{
    RGBSpectrum result;

    float rgb[3] = {
        clamp_nonnegative(input_rgb[0]),
        clamp_nonnegative(input_rgb[1]),
        clamp_nonnegative(input_rgb[2])
    };

    const float max_component =
        std::max(
            rgb[0],
            std::max(
                rgb[1],
                rgb[2]));

    result.scale =
        std::max(
            1.0f,
            max_component);

    rgb[0] /= result.scale;
    rgb[1] /= result.scale;
    rgb[2] /= result.scale;

    rgb2spec_fetch(
        model,
        rgb,
        result.coeff);

    return result;
}

float
evaluate_rgb_spectrum(
    const RGBSpectrum& spectrum,
    float wavelength_nm)
{
    return spectrum.scale
        * rgb2spec_eval_precise(
            const_cast<float*>(
                spectrum.coeff),
            wavelength_nm);
}

} // namespace

FilmProcessor::FilmProcessor(
    RGB2Spec* rgb2spec_model,
    const FilmStock& stock,
    const SampledCurve& illuminant,
    const Settings& settings)
    : rgb2spec_model_(rgb2spec_model)
    , stock_(stock)
    , illuminant_(illuminant)
    , settings_(settings)
{
    if (settings_.neutral_balance) {
        balance_ =
            derive_neutral_balance();
    }
}

FilmExposure
FilmProcessor::expose(
    const SampledCurve& illuminated_spectrum) const
{
    FilmExposure exposure;

    if (!illuminated_spectrum.valid()
        || settings_.wavelength_step_nm <= 0.0f
        || settings_.wavelength_max_nm
            < settings_.wavelength_min_nm) {

        return exposure;
    }

    const auto& sensitivity =
        stock_.sensitivity();

    for (float wavelength =
             settings_.wavelength_min_nm;
         wavelength <=
             settings_.wavelength_max_nm
             + 0.001f;
         wavelength +=
             settings_.wavelength_step_nm) {

        const float scene_power =
            illuminated_spectrum.sample(
                wavelength,
                0.0f);

        const float s_blue =
            log_sensitivity_to_linear(
                sensitivity.blue_sensitive_log,
                wavelength);

        const float s_green =
            log_sensitivity_to_linear(
                sensitivity.green_sensitive_log,
                wavelength);

        const float s_red =
            log_sensitivity_to_linear(
                sensitivity.red_sensitive_log,
                wavelength);

        exposure.blue +=
            scene_power
            * s_blue
            * settings_.wavelength_step_nm;

        exposure.green +=
            scene_power
            * s_green
            * settings_.wavelength_step_nm;

        exposure.red +=
            scene_power
            * s_red
            * settings_.wavelength_step_nm;
    }

    return exposure;
}

FilmExposure
FilmProcessor::expose(
    const std::array<float, 3>& aces_rgb) const
{
    SampledCurve illuminated_spectrum;

    if (!rgb2spec_model_
        || !illuminant_.valid()
        || settings_.wavelength_step_nm <= 0.0f
        || settings_.wavelength_max_nm
            < settings_.wavelength_min_nm) {

        return FilmExposure();
    }

    const RGBSpectrum spectrum =
        reconstruct_rgb_spectrum(
            rgb2spec_model_,
            aces_rgb);

    for (float wavelength =
             settings_.wavelength_min_nm;
         wavelength <=
             settings_.wavelength_max_nm
             + 0.001f;
         wavelength +=
             settings_.wavelength_step_nm) {

        const float reconstructed_power =
            evaluate_rgb_spectrum(
                spectrum,
                wavelength);

        const float illuminant_power =
            illuminant_.sample(
                wavelength,
                0.0f);

        illuminated_spectrum.x.push_back(
            wavelength);

        illuminated_spectrum.y.push_back(
            reconstructed_power
            * illuminant_power);
    }

    return expose(
        illuminated_spectrum);
}

FilmLogExposure
FilmProcessor::log_exposure(
    const FilmExposure& exposure) const
{
    const float stop_multiplier =
        std::pow(
            2.0f,
            settings_.exposure_stops);

    auto to_log =
        [&](float value,
            float offset) {

        float H =
            value * stop_multiplier;

        H = std::max(
            H,
            1e-20f);

        return
            std::log10(H)
            + settings_.log_exposure_calibration
            + offset;
    };

    FilmLogExposure result;

    result.red =
        to_log(
            exposure.red,
            balance_.red_log_offset);

    result.green =
        to_log(
            exposure.green,
            balance_.green_log_offset);

    result.blue =
        to_log(
            exposure.blue,
            balance_.blue_log_offset);

    return result;
}

FilmDensity
FilmProcessor::develop(
    const FilmLogExposure& exposure) const
{
    const auto& curves =
        stock_.characteristic();

    FilmDensity density;

    density.red =
        sample_characteristic_clamped(
            curves.red_density,
            exposure.red);

    density.green =
        sample_characteristic_clamped(
            curves.green_density,
            exposure.green);

    density.blue =
        sample_characteristic_clamped(
            curves.blue_density,
            exposure.blue);

    return density;
}

FilmDensity
FilmProcessor::process(
    const std::array<float, 3>& aces_rgb) const
{
    return develop(
        log_exposure(
            expose(
                aces_rgb)));
}

const FilmExposureBalance&
FilmProcessor::balance() const
{
    return balance_;
}

const FilmProcessor::Settings&
FilmProcessor::settings() const
{
    return settings_;
}

SampledCurve
FilmProcessor::sampled_illuminant() const
{
    SampledCurve result;

    if (!illuminant_.valid()
        || settings_.wavelength_step_nm <= 0.0f
        || settings_.wavelength_max_nm
            < settings_.wavelength_min_nm) {

        return result;
    }

    for (float wavelength =
             settings_.wavelength_min_nm;
         wavelength <=
             settings_.wavelength_max_nm
             + 0.001f;
         wavelength +=
             settings_.wavelength_step_nm) {

        result.x.push_back(
            wavelength);

        result.y.push_back(
            illuminant_.sample(
                wavelength,
                0.0f));
    }

    return result;
}

FilmExposureBalance
FilmProcessor::derive_neutral_balance() const
{
    FilmExposureBalance balance;

    const std::array<float, 3> neutral_ap0 = {{
        1.0f,
        1.0f,
        1.0f
    }};

    balance.neutral_reference =
        expose(neutral_ap0);

    const float r =
        std::max(
            balance.neutral_reference.red,
            1e-20f);

    const float g =
        std::max(
            balance.neutral_reference.green,
            1e-20f);

    const float b =
        std::max(
            balance.neutral_reference.blue,
            1e-20f);

    const float lr = std::log10(r);
    const float lg = std::log10(g);
    const float lb = std::log10(b);

    const float mean =
        (lr + lg + lb) / 3.0f;

    balance.red_log_offset =
        mean - lr;

    balance.green_log_offset =
        mean - lg;

    balance.blue_log_offset =
        mean - lb;

    return balance;
}

float
FilmProcessor::log_sensitivity_to_linear(
    const SampledCurve& curve,
    float wavelength_nm)
{
    const float no_sample =
        -9999.0f;

    const float log_s =
        curve.sample(
            wavelength_nm,
            no_sample);

    if (log_s <= -9000.0f) {
        return 0.0f;
    }

    return std::pow(
        10.0f,
        log_s);
}

float
FilmProcessor::sample_characteristic_clamped(
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
