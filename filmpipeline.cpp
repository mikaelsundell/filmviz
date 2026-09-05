// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"

#include "bleachbypass.h"
#include "colortransform.h"
#include "filmdensitycalibration.h"
#include "filmdyemodel.h"
#include "filmprocessor.h"
#include "filmstock.h"
#include "granularitymodel.h"
#include "printfilmprocessor.h"
#include "printfilmstock.h"
#include "printviewer.h"
#include "spectralilluminant.h"
#include "spectralreconstructor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

FilmPipeline::FilmPipeline() = default;
FilmPipeline::~FilmPipeline() = default;

bool
FilmPipeline::initialize()
{
    return
        initialize(
            Settings());
}

bool
FilmPipeline::initialize(
    const Settings& settings)
{
    settings_ = settings;
    error_.clear();
    valid_ = false;

    const auto valid_unit_control =
        [](float value) {
            return std::isfinite(value)
                && value >= 0.0f
                && value <= 1.0f;
        };

    const auto valid_printer_light =
        [](float value) {
            return std::isfinite(value)
                && value >= 0.0f
                && value <= 50.0f;
        };

    if (!valid_unit_control(settings_.negative_bleach_bypass)
        || !valid_unit_control(settings_.print_bleach_bypass)
        || !valid_printer_light(settings_.printer_light_red)
        || !valid_printer_light(settings_.printer_light_green)
        || !valid_printer_light(settings_.printer_light_blue)) {

        error_ = "invalid bleach-bypass or printer-light settings";
        return false;
    }

    const std::string rgb2spec_file =
        resource_path("spectral/reconstruction/ACES2065_1.spec");

    std::string negative_name;
    std::string negative_directory;
    std::string negative_prefix;

    if (settings_.negative_profile == "verita-200d") {
        negative_name = "Kodak Verita 200D";
        negative_directory = "profiles/verita_200d";
        negative_prefix = "kodak_verita_200d";
    }
    else if (settings_.negative_profile == "kodak-50d") {
        negative_name = "Kodak VISION3 50D 5203/7203";
        negative_directory = "profiles/kodak_50d";
        negative_prefix = "kodak_50d";
    }
    else {
        error_ =
            "unknown negative profile: "
            + settings_.negative_profile;

        return false;
    }

    const std::string negative_sensitivity_file =
        resource_path(
            negative_directory
            + "/"
            + negative_prefix
            + "_spectral_sensitivity_curves.csv");

    const std::string negative_characteristic_file =
        resource_path(
            negative_directory
            + "/"
            + negative_prefix
            + "_sensitometric_curves.csv");

    const std::string negative_dye_file =
        resource_path(
            negative_directory
            + "/"
            + negative_prefix
            + "_spectral_dye_density_curves.csv");

    const std::string negative_granularity_file =
        resource_path(
            negative_directory
            + "/"
            + negative_prefix
            + "_diffuse_rms_granularity_curves.csv");

    const std::string print_sensitivity_file =
        resource_path("profiles/kodak_2383/kodak_2383_spectral_sensitivity_curves.csv");

    const std::string print_characteristic_file =
        resource_path("profiles/kodak_2383/kodak_2383_sensitometric_curves.csv");

    const std::string print_dye_file =
        resource_path("profiles/kodak_2383/kodak_2383_corrected_spectral_dye_density_curves.csv");

    const std::string print_mtf_file =
        resource_path("profiles/kodak_2383/kodak_2383_modulation_transfer_function_curves.csv");

    const std::string print_granularity_file =
        resource_path("profiles/kodak_2383/kodak_2383_diffuse_rms_granularity_curves.csv");

    granularity_model_ =
        std::make_unique<GranularityModel>();

    if (!granularity_model_->load(
            negative_granularity_file,
            print_granularity_file)) {

        error_ =
            "could not initialize measured granularity resources";

        return false;
    }

    const std::string observer_file =
        resource_path("colorimetry/observers/CIE_xyz_1931_2deg.csv");

    const std::string d55_file =
        resource_path("colorimetry/illuminants/CIE_std_illum_D55.csv");

    reconstructor_ =
        std::make_unique<SpectralReconstructor>(
            rgb2spec_file);

    scene_illuminant_ =
        std::make_unique<SpectralIlluminant>(
            SpectralIlluminant::Standard::D60);

    negative_stock_ =
        std::make_unique<FilmStock>(
            negative_name);

    if (!reconstructor_->valid()
        || !scene_illuminant_->valid()
        || !negative_stock_->load(
            negative_sensitivity_file,
            negative_characteristic_file)) {

        error_ =
            "could not initialize "
            + negative_name
            + " negative resources";

        return false;
    }

    FilmProcessor::Settings negative_settings;

    negative_settings.exposure_stops = 0.0f;
    negative_settings.log_exposure_calibration = 0.0f;
    negative_settings.neutral_balance = false;
    negative_settings.wavelength_min_nm = settings_.wavelength_min_nm;
    negative_settings.wavelength_max_nm = settings_.wavelength_max_nm;
    negative_settings.wavelength_step_nm = settings_.wavelength_step_nm;

    negative_processor_ =
        std::make_unique<FilmProcessor>(
            nullptr,
            *negative_stock_,
            scene_illuminant_->curve(),
            negative_settings);

    negative_dye_model_ =
        std::make_unique<FilmDyeModel>();

    if (!negative_dye_model_->load_and_estimate(
            negative_dye_file,
            *negative_stock_,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm,
            settings_.wavelength_step_nm,
            settings_.negative_zero_stop_log_exposure)) {

        error_ =
            "could not initialize "
            + negative_name
            + " spectral dye model";

        return false;
    }

    print_stock_ =
        std::make_unique<PrintFilmStock>(
            "Kodak 2383 corrected");

    if (!print_stock_->load(
            print_sensitivity_file,
            print_characteristic_file,
            print_dye_file,
            print_mtf_file,
            print_granularity_file)) {

        error_ =
            "could not initialize Kodak 2383 print-film resources";

        return false;
    }

    const std::array<float, 3> reference_ap0 = {{
        settings_.middle_gray,
        settings_.middle_gray,
        settings_.middle_gray
    }};

    const auto reference_spectrum =
        reconstructor_->reconstruct(
            reference_ap0,
            SpectralReconstructor::Method::Optimized);

    const SampledCurve reference_factor =
        reconstructor_->sample(
            reference_spectrum,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm,
            settings_.wavelength_step_nm);

    const SampledCurve reference_illuminated =
        scene_illuminant_->illuminate(
            reference_factor);

    reference_negative_exposure_ =
        negative_processor_->expose(
            reference_illuminated);

    reference_negative_density_ =
        negative_processor_->develop(
            FilmLogExposure{
                settings_.negative_zero_stop_log_exposure,
                settings_.negative_zero_stop_log_exposure,
                settings_.negative_zero_stop_log_exposure});

    const SampledCurve reference_negative_spectral_density =
        negative_dye_model_->synthesize_density(
            reference_negative_density_);

    reference_negative_transmittance_ =
        transmittance_from_density(
            reference_negative_spectral_density);

    const SampledCurve minimum_negative_spectral_density =
        load_minimum_negative_density_curve(
            negative_dye_file);

    negative_density_calibration_ =
        std::make_unique<FilmDensityCalibration>(
            *negative_dye_model_,
            reference_negative_density_,
            minimum_negative_spectral_density);

    if (!reference_negative_spectral_density.valid()
        || !reference_negative_transmittance_.valid()
        || !minimum_negative_spectral_density.valid()
        || !negative_density_calibration_->valid()) {

        error_ =
            "could not initialize "
            + negative_name
            + " Status-M density calibration";

        return false;
    }

    const SampledCurve printer_illuminant =
        PrintFilmProcessor::make_blackbody_illuminant(
            settings_.printer_temperature_kelvin,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm,
            settings_.wavelength_step_nm);

    PrintFilmProcessor::Settings print_settings;

    print_settings.exposure_stops = 0.0f;
    print_settings.log_exposure_calibration = 0.0f;
    print_settings.wavelength_min_nm = settings_.wavelength_min_nm;
    print_settings.wavelength_max_nm = settings_.wavelength_max_nm;
    print_settings.wavelength_step_nm = settings_.wavelength_step_nm;
    print_settings.reference_status_a_density =
        settings_.print_reference_status_a_density;
    print_settings.printer_light_red =
        settings_.printer_light_red;
    print_settings.printer_light_green =
        settings_.printer_light_green;
    print_settings.printer_light_blue =
        settings_.printer_light_blue;

    print_processor_ =
        std::make_unique<PrintFilmProcessor>(
            *print_stock_,
            printer_illuminant,
            reference_negative_transmittance_,
            print_settings);

    if (!print_processor_->valid()) {
        error_ =
            "could not initialize Kodak 2383 print processor";

        return false;
    }

    PrintViewer::Settings viewer_settings;

    viewer_settings.wavelength_min_nm = settings_.wavelength_min_nm;
    viewer_settings.wavelength_max_nm = settings_.wavelength_max_nm;
    viewer_settings.wavelength_step_nm = settings_.wavelength_step_nm;

    viewer_ =
        std::make_unique<PrintViewer>();

    if (!viewer_->load(
            observer_file,
            d55_file,
            viewer_settings)) {

        error_ =
            "could not initialize D55/CIE print viewer";

        return false;
    }

    ap0_to_rec709_ =
        std::make_unique<ColorTransform>(
            ColorTransform::ColorSpace::ACES2065_1,
            ColorTransform::TransferFunction::Linear,
            ColorTransform::ColorSpace::Rec709,
            ColorTransform::TransferFunction::Gamma24);

    valid_ = true;
    return true;
}

bool
FilmPipeline::valid() const
{
    return valid_;
}

FilmPipeline::Result
FilmPipeline::process(
    const std::array<float, 3>& ap0_linear) const
{
    FilmExposure exposure;

    if (!negative_exposure(
            ap0_linear,
            exposure)) {

        return Result();
    }

    return
        process_negative_exposure(
            exposure);
}

bool
FilmPipeline::negative_exposure(
    const std::array<float, 3>& ap0_linear,
    FilmExposure& exposure) const
{
    exposure = FilmExposure();

    if (!valid_) {
        return false;
    }

    const auto spectrum =
        reconstructor_->reconstruct(
            ap0_linear,
            SpectralReconstructor::Method::Optimized);

    const SampledCurve factor =
        reconstructor_->sample(
            spectrum,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm,
            settings_.wavelength_step_nm);

    const SampledCurve illuminated =
        scene_illuminant_->illuminate(
            factor);

    if (!illuminated.valid()) {
        return false;
    }

    exposure =
        negative_processor_->expose(
            illuminated);

    return
        std::isfinite(exposure.red)
        && std::isfinite(exposure.green)
        && std::isfinite(exposure.blue);
}

FilmPipeline::Result
FilmPipeline::process_negative_exposure(
    const FilmExposure& negative_exposure) const
{
    Result result;

    if (!valid_
        || !std::isfinite(negative_exposure.red)
        || !std::isfinite(negative_exposure.green)
        || !std::isfinite(negative_exposure.blue)) {
        return result;
    }

    result.negative_status_m_density =
        negative_processor_->develop(
            relative_negative_log_exposure(
                negative_exposure));

    if (std::abs(settings_.push_pull_stops) > 1e-7f) {
        // No alternate-development measurements are available for the active
        // negative profile. Use an explicit contrast approximation around the
        // density: one push/pull stop changes slope by 2^0.2 (~14.9%).
        const float contrast =
            std::exp2(
                0.2f
                * settings_.push_pull_stops);

        result.negative_status_m_density.red =
            reference_negative_density_.red
            + contrast
                * (result.negative_status_m_density.red
                   - reference_negative_density_.red);

        result.negative_status_m_density.green =
            reference_negative_density_.green
            + contrast
                * (result.negative_status_m_density.green
                   - reference_negative_density_.green);

        result.negative_status_m_density.blue =
            reference_negative_density_.blue
            + contrast
                * (result.negative_status_m_density.blue
                   - reference_negative_density_.blue);
    }

    result.negative_granularity_sigma =
        granularity_model_->negative_sigma(
            result.negative_status_m_density);

    if (!negative_density_calibration_->calibrate(
            result.negative_status_m_density,
            result.calibrated_negative_density)) {

        return result;
    }

    SampledCurve negative_density =
        negative_dye_model_->synthesize_density(
            result.calibrated_negative_density);

    const BleachBypass::Result negative_bypass =
        BleachBypass::apply_negative(
            negative_density,
            settings_.negative_bleach_bypass);

    if (!negative_bypass.valid) {
        return result;
    }

    negative_density =
        negative_bypass.spectral_density;

    result.negative_bleach_mean_density_delta =
        negative_bypass.mean_density_after
        - negative_bypass.mean_density_before;

    const SampledCurve negative_transmittance =
        transmittance_from_density(
            negative_density);

    if (!negative_transmittance.valid()) {
        return result;
    }

    const FilmExposure print_exposure =
        print_processor_->expose(
            negative_transmittance);

    result.print_density =
        print_processor_->develop(
            print_processor_->log_exposure(
                print_exposure));

    result.print_granularity_sigma =
        granularity_model_->print_sigma(
            result.print_density);

    SampledCurve print_density =
        density_from_print_records(
            result.print_density);

    const BleachBypass::Result print_bypass =
        BleachBypass::apply_print(
            print_density,
            settings_.print_bleach_bypass);

    if (!print_bypass.valid) {
        return result;
    }

    print_density =
        print_bypass.spectral_density;

    result.print_bleach_mean_density_delta =
        print_bypass.mean_density_after
        - print_bypass.mean_density_before;

    const SampledCurve print_transmittance =
        transmittance_from_density(
            print_density);

    if (!print_transmittance.valid()) {
        return result;
    }

    const PrintViewer::Result viewed =
        viewer_->view(
            print_transmittance);

    result.ap0 =
        viewed.aces2065_1;

    result.rec709_gamma24 =
        ap0_to_rec709_->transform(
            result.ap0);

    result.valid =
        finite_rgb(result.ap0)
        && finite_rgb(result.rec709_gamma24)
        && std::isfinite(result.negative_status_m_density.red)
        && std::isfinite(result.negative_status_m_density.green)
        && std::isfinite(result.negative_status_m_density.blue)
        && std::isfinite(result.calibrated_negative_density.red)
        && std::isfinite(result.calibrated_negative_density.green)
        && std::isfinite(result.calibrated_negative_density.blue)
        && std::isfinite(result.print_density.red)
        && std::isfinite(result.print_density.green)
        && std::isfinite(result.print_density.blue)
        && std::isfinite(result.negative_granularity_sigma.red)
        && std::isfinite(result.negative_granularity_sigma.green)
        && std::isfinite(result.negative_granularity_sigma.blue)
        && std::isfinite(result.print_granularity_sigma.red)
        && std::isfinite(result.print_granularity_sigma.green)
        && std::isfinite(result.print_granularity_sigma.blue);

    return result;
}

const FilmPipeline::Settings&
FilmPipeline::settings() const
{
    return settings_;
}

const std::string&
FilmPipeline::error() const
{
    return error_;
}

const FilmDensityCalibration*
FilmPipeline::negative_density_calibration() const
{
    return negative_density_calibration_.get();
}

SampledCurve
FilmPipeline::load_minimum_negative_density_curve(
    const std::string& filename) const
{
    SampledCurve result;

    std::ifstream file(
        filename.c_str());

    if (!file) {
        return result;
    }

    std::string line;
    bool first = true;

    while (std::getline(file, line)) {
        if (first) {
            first = false;
            continue;
        }

        if (line.empty()) {
            continue;
        }

        std::stringstream stream(line);
        std::string wavelength_field;
        std::string minimum_field;

        if (!std::getline(stream, wavelength_field, ',')
            || !std::getline(stream, minimum_field, ',')) {

            continue;
        }

        try {
            const float wavelength =
                std::stof(
                    wavelength_field);

            const float density =
                std::stof(
                    minimum_field);

            if (!std::isfinite(wavelength)
                || !std::isfinite(density)) {

                continue;
            }

            result.x.push_back(
                wavelength);

            result.y.push_back(
                density);
        }
        catch (...) {
        }
    }

    return
        result.valid()
            ? result
            : SampledCurve();
}

SampledCurve
FilmPipeline::density_from_print_records(
    const FilmDensity& record_density) const
{
    SampledCurve result;

    if (!print_stock_) {
        return result;
    }

    const auto& dye =
        print_stock_->dye_density();

    for (float wavelength = settings_.wavelength_min_nm;
         wavelength <= settings_.wavelength_max_nm + 0.001f;
         wavelength += settings_.wavelength_step_nm) {

        const float c =
            dye.cyan_density.sample(
                wavelength,
                std::numeric_limits<float>::quiet_NaN());

        const float m =
            dye.magenta_density.sample(
                wavelength,
                std::numeric_limits<float>::quiet_NaN());

        const float y =
            dye.yellow_density.sample(
                wavelength,
                std::numeric_limits<float>::quiet_NaN());

        if (!std::isfinite(c)
            || !std::isfinite(m)
            || !std::isfinite(y)) {

            return SampledCurve();
        }

        const double density =
            settings_.print_cyan_amplitude
                * static_cast<double>(record_density.red)
                * static_cast<double>(c)
            + settings_.print_magenta_amplitude
                * static_cast<double>(record_density.green)
                * static_cast<double>(m)
            + settings_.print_yellow_amplitude
                * static_cast<double>(record_density.blue)
                * static_cast<double>(y);

        result.x.push_back(
            wavelength);

        result.y.push_back(
            static_cast<float>(
                std::max(
                    0.0,
                    density)));
    }

    return result;
}

SampledCurve
FilmPipeline::transmittance_from_density(
    const SampledCurve& density)
{
    SampledCurve result;

    if (!density.valid()) {
        return result;
    }

    result.x =
        density.x;

    result.y.reserve(
        density.y.size());

    for (float value : density.y) {
        result.y.push_back(
            std::pow(
                10.0f,
                -value));
    }

    return result;
}

FilmLogExposure
FilmPipeline::relative_negative_log_exposure(
    const FilmExposure& exposure) const
{
    FilmLogExposure result;

    constexpr float log10_two =
        0.3010299956639812f;

    const float exposure_offset =
        settings_.exposure_stops
        * log10_two;

    result.red =
        settings_.negative_zero_stop_log_exposure
        + exposure_offset
        + std::log10(
            std::max(exposure.red, 1e-20f)
            / std::max(reference_negative_exposure_.red, 1e-20f));

    result.green =
        settings_.negative_zero_stop_log_exposure
        + exposure_offset
        + std::log10(
            std::max(exposure.green, 1e-20f)
            / std::max(reference_negative_exposure_.green, 1e-20f));

    result.blue =
        settings_.negative_zero_stop_log_exposure
        + exposure_offset
        + std::log10(
            std::max(exposure.blue, 1e-20f)
            / std::max(reference_negative_exposure_.blue, 1e-20f));

    return result;
}

bool
FilmPipeline::finite_rgb(
    const std::array<float, 3>& rgb)
{
    return
        std::isfinite(rgb[0])
        && std::isfinite(rgb[1])
        && std::isfinite(rgb[2]);
}

std::string
FilmPipeline::resource_path(
    const std::string& filename) const
{
    return
        (std::filesystem::path(
             settings_.resources_directory)
         / filename)
        .string();
}
