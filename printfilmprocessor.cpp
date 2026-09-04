// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "printfilmprocessor.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

constexpr float kLog10Two =
    0.3010299956639812f;

float
safe_log10(
    float value)
{
    return std::log10(
        std::max(
            value,
            1e-20f));
}

} // namespace

PrintFilmProcessor::PrintFilmProcessor(
    const PrintFilmStock& stock,
    const SampledCurve& printer_illuminant,
    const SampledCurve& reference_negative_transmittance,
    const Settings& settings)
    : stock_(stock)
    , printer_illuminant_(printer_illuminant)
    , settings_(settings)
{
    valid_ =
        stock_.valid()
        && printer_illuminant_.valid()
        && reference_negative_transmittance.valid()
        && settings_.wavelength_step_nm > 0.0f
        && settings_.wavelength_max_nm
            >= settings_.wavelength_min_nm
        && derive_balance(
            reference_negative_transmittance);
}

bool
PrintFilmProcessor::valid() const
{
    return valid_;
}

FilmExposure
PrintFilmProcessor::expose(
    const SampledCurve& negative_transmittance) const
{
    FilmExposure result;

    if (!stock_.valid()
        || !printer_illuminant_.valid()
        || !negative_transmittance.valid()) {
        return result;
    }

    const auto& sensitivity =
        stock_.sensitivity();

    for (float wavelength_nm =
             settings_.wavelength_min_nm;
         wavelength_nm <=
             settings_.wavelength_max_nm
             + 0.001f;
         wavelength_nm +=
             settings_.wavelength_step_nm) {

        const float printer_power =
            std::max(
                0.0f,
                printer_illuminant_.sample(
                    wavelength_nm,
                    0.0f));

        const float transmission =
            std::max(
                0.0f,
                negative_transmittance.sample(
                    wavelength_nm,
                    0.0f));

        const float transmitted_power =
            printer_power
            * transmission;

        // Kodak layer mapping:
        //   cyan-forming    = red-sensitive record
        //   magenta-forming = green-sensitive record
        //   yellow-forming  = blue-sensitive record
        const float red_sensitivity =
            log_sensitivity_to_linear(
                sensitivity.cyan_forming_log,
                wavelength_nm);

        const float green_sensitivity =
            log_sensitivity_to_linear(
                sensitivity.magenta_forming_log,
                wavelength_nm);

        const float blue_sensitivity =
            log_sensitivity_to_linear(
                sensitivity.yellow_forming_log,
                wavelength_nm);

        result.red +=
            transmitted_power
            * red_sensitivity
            * settings_.wavelength_step_nm;

        result.green +=
            transmitted_power
            * green_sensitivity
            * settings_.wavelength_step_nm;

        result.blue +=
            transmitted_power
            * blue_sensitivity
            * settings_.wavelength_step_nm;
    }

    return result;
}

FilmLogExposure
PrintFilmProcessor::log_exposure(
    const FilmExposure& exposure) const
{
    FilmLogExposure result;

    const float exposure_offset =
        settings_.exposure_stops
        * kLog10Two;

    result.red =
        safe_log10(
            exposure.red)
        + settings_.log_exposure_calibration
        + exposure_offset
        + balance_.red_log_offset;

    result.green =
        safe_log10(
            exposure.green)
        + settings_.log_exposure_calibration
        + exposure_offset
        + balance_.green_log_offset;

    result.blue =
        safe_log10(
            exposure.blue)
        + settings_.log_exposure_calibration
        + exposure_offset
        + balance_.blue_log_offset;

    return result;
}

FilmDensity
PrintFilmProcessor::develop(
    const FilmLogExposure& exposure) const
{
    FilmDensity result;

    const auto& curves =
        stock_.characteristic();

    result.red =
        sample_characteristic_clamped(
            curves.red_density,
            exposure.red);

    result.green =
        sample_characteristic_clamped(
            curves.green_density,
            exposure.green);

    result.blue =
        sample_characteristic_clamped(
            curves.blue_density,
            exposure.blue);

    return result;
}

FilmDensity
PrintFilmProcessor::process(
    const SampledCurve& negative_transmittance) const
{
    return develop(
        log_exposure(
            expose(
                negative_transmittance)));
}

const PrintFilmProcessor::Balance&
PrintFilmProcessor::balance() const
{
    return balance_;
}

const PrintFilmProcessor::Settings&
PrintFilmProcessor::settings() const
{
    return settings_;
}

const SampledCurve&
PrintFilmProcessor::printer_illuminant() const
{
    return printer_illuminant_;
}

void
PrintFilmProcessor::set_balance_log_offsets(
    float red_log_offset,
    float green_log_offset,
    float blue_log_offset)
{
    balance_.red_log_offset =
        red_log_offset;

    balance_.green_log_offset =
        green_log_offset;

    balance_.blue_log_offset =
        blue_log_offset;
}

SampledCurve
PrintFilmProcessor::make_blackbody_illuminant(
    float kelvin,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm)
{
    SampledCurve result;

    if (kelvin <= 0.0f
        || wavelength_step_nm <= 0.0f
        || wavelength_max_nm < wavelength_min_nm) {
        return result;
    }

    // Planck relative spectral radiance.
    //
    // Only relative shape matters because the processor derives a channel
    // balance from a neutral reference negative. Constants can therefore be
    // evaluated in double precision and peak-normalized afterwards.
    constexpr double h =
        6.62607015e-34;

    constexpr double c =
        299792458.0;

    constexpr double k =
        1.380649e-23;

    double peak = 0.0;

    for (float wavelength_nm =
             wavelength_min_nm;
         wavelength_nm <=
             wavelength_max_nm
             + 0.001f;
         wavelength_nm +=
             wavelength_step_nm) {

        const double wavelength_m =
            static_cast<double>(
                wavelength_nm)
            * 1e-9;

        const double numerator =
            2.0 * h * c * c;

        const double exponent =
            h * c
            / (wavelength_m
               * k
               * static_cast<double>(
                   kelvin));

        const double denominator =
            std::pow(
                wavelength_m,
                5.0)
            * std::expm1(
                exponent);

        const double radiance =
            denominator > 0.0
                ? numerator / denominator
                : 0.0;

        result.x.push_back(
            wavelength_nm);

        result.y.push_back(
            static_cast<float>(
                radiance));

        peak =
            std::max(
                peak,
                radiance);
    }

    if (peak <= 0.0) {
        return SampledCurve();
    }

    for (float& value :
         result.y) {

        value =
            static_cast<float>(
                static_cast<double>(value)
                / peak);
    }

    return result;
}

bool
PrintFilmProcessor::derive_balance(
    const SampledCurve& reference_negative_transmittance)
{
    balance_ = Balance();

    const FilmExposure reference =
        expose(
            reference_negative_transmittance);

    if (reference.red <= 0.0f
        || reference.green <= 0.0f
        || reference.blue <= 0.0f) {

        std::cerr
            << "error: Kodak 2383 neutral printer reference produced "
               "non-positive exposure"
            << std::endl;
        return false;
    }

    balance_.neutral_reference_exposure =
        reference;

    balance_.neutral_reference_raw_log_exposure.red =
        safe_log10(
            reference.red);

    balance_.neutral_reference_raw_log_exposure.green =
        safe_log10(
            reference.green);

    balance_.neutral_reference_raw_log_exposure.blue =
        safe_log10(
            reference.blue);

    const auto& curves =
        stock_.characteristic();

    balance_.target_log_exposure.red =
        inverse_characteristic(
            curves.red_density,
            settings_.reference_status_a_density);

    balance_.target_log_exposure.green =
        inverse_characteristic(
            curves.green_density,
            settings_.reference_status_a_density);

    balance_.target_log_exposure.blue =
        inverse_characteristic(
            curves.blue_density,
            settings_.reference_status_a_density);

    balance_.red_log_offset =
        balance_.target_log_exposure.red
        - balance_.neutral_reference_raw_log_exposure.red
        - settings_.log_exposure_calibration;

    balance_.green_log_offset =
        balance_.target_log_exposure.green
        - balance_.neutral_reference_raw_log_exposure.green
        - settings_.log_exposure_calibration;

    balance_.blue_log_offset =
        balance_.target_log_exposure.blue
        - balance_.neutral_reference_raw_log_exposure.blue
        - settings_.log_exposure_calibration;

    return true;
}

float
PrintFilmProcessor::log_sensitivity_to_linear(
    const SampledCurve& curve,
    float wavelength_nm)
{
    if (!curve.valid()
        || wavelength_nm < curve.x.front()
        || wavelength_nm > curve.x.back()) {
        return 0.0f;
    }

    return std::pow(
        10.0f,
        curve.sample(
            wavelength_nm,
            -20.0f));
}

float
PrintFilmProcessor::sample_characteristic_clamped(
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

float
PrintFilmProcessor::inverse_characteristic(
    const SampledCurve& curve,
    float target_density)
{
    if (!curve.valid()) {
        return 0.0f;
    }

    if (target_density <= curve.y.front()) {
        return curve.x.front();
    }

    if (target_density >= curve.y.back()) {
        return curve.x.back();
    }

    for (std::size_t i = 1;
         i < curve.y.size();
         ++i) {

        const float y0 =
            curve.y[i - 1];

        const float y1 =
            curve.y[i];

        if ((target_density >= y0
             && target_density <= y1)
            || (target_density >= y1
                && target_density <= y0)) {

            if (std::abs(y1 - y0) < 1e-12f) {
                return curve.x[i - 1];
            }

            const float t =
                (target_density - y0)
                / (y1 - y0);

            return curve.x[i - 1]
                + t
                    * (curve.x[i]
                       - curve.x[i - 1]);
        }
    }

    // The digitized 2383 curves are monotonic. This fallback should not be
    // reached, but returning the closest endpoint is safer than inventing an
    // extrapolated exposure.
    const float front_error =
        std::abs(
            target_density
            - curve.y.front());

    const float back_error =
        std::abs(
            target_density
            - curve.y.back());

    return front_error <= back_error
        ? curve.x.front()
        : curve.x.back();
}
