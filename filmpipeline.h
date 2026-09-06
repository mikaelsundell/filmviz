// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "negativeprofile.h"
#include "printprofile.h"

#include <array>
#include <memory>
#include <string>

class ColorTransform;
class FilmDensityCalibration;
class FilmColorResponse;
class FilmDyeModel;
class FilmProcessor;
class FilmStock;
class GranularityModel;
class PrintFilmProcessor;
class PrintFilmStock;
class PrintViewer;
class SpectralIlluminant;
class SpectralReconstructor;

// Production end-to-end spectral film pipeline derived from validated production.
//
// Input  : linear ACES2065-1 (AP0/D60)
// Output : viewed print represented as linear ACES2065-1 (AP0/D60)
//          plus a simple Rec.709/Gamma 2.4 preview.
//
// Important: the Rec.709 preview is only a matrix/transfer-function display
// conversion. No ACES RRT/ODT is applied. The authoritative film result is the
// viewed AP0 value returned by process().
class FilmPipeline
{
public:
    struct Settings
    {
        std::string resources_directory = "resources";
        std::string negative_profile =
            NegativeProfileCatalog::default_profile().identifier;
        std::string print_profile =
            PrintProfileCatalog::default_profile().identifier;

        float middle_gray = 0.18f;
        float negative_zero_stop_log_exposure = -0.515f;
        float exposure_stops = 0.0f;

        // Uniform exposure added before negative development, expressed as a
        // percentage of the calibrated middle-gray record exposure.
        float negative_flash_percent = 0.0f;

        // Push/pull is an explicit approximation because alternate-process
        // characteristic curves are not part of the active negative profiles.
        // Positive values increase negative contrast around the calibrated
        // middle-gray density.
        float push_pull_stops = 0.0f;

        // Signed empirical density-domain colour trim. Zero selects the
        // accepted standard response, -4 is calibrated bypass, and positive
        // values progressively calm and deepen chromatic regions.
        float color_density = 0.0f;

        // Profile-independent bleach-bypass look controls. Zero is normal
        // processing; one is the full modeled process look. The current
        // approximation preserves mean spectral density to avoid introducing
        // a large contrast shift: negative bypass adds a cool/cyan spectral
        // tilt, while print bypass contracts spectral colour differences.
        float negative_bleach_bypass = 0.0f;
        float print_bleach_bypass = 0.0f;

        float print_reference_status_a_density = 1.0f;
        float printer_temperature_kelvin = 3200.0f;

        // Linked printer-light offset. One point equals 0.025 LogE and is
        // added to all three record-specific light settings.
        float printer_light_master = 0.0f;

        // Uniform exposure added before print development, expressed as a
        // percentage of the neutral reference printer exposure.
        float print_flash_percent = 0.0f;

        // Traditional printer-light controls. 25/25/25 is the calibrated
        // neutral operating point. Each point changes the corresponding
        // print-record log exposure by 0.025.
        float printer_light_red = 25.0f;
        float printer_light_green = 25.0f;
        float printer_light_blue = 25.0f;

        float wavelength_min_nm = 380.0f;
        float wavelength_max_nm = 700.0f;
        float wavelength_step_nm = 5.0f;

        // Kodak Vision 2383/3383 dye-amplitude calibration selected in
        // JIS/D55 calibration.
        double print_cyan_amplitude = 1.10093;
        double print_magenta_amplitude = 1.09650;
        double print_yellow_amplitude = 1.14626;
    };

    struct Result
    {
        FilmExposure negative_exposure;
        FilmExposure print_exposure;

        FilmDensity negative_status_m_density;
        FilmDensity calibrated_negative_density;
        FilmDensity color_response_negative_density;
        FilmDensity print_density;
        FilmDensity negative_granularity_sigma;
        FilmDensity print_granularity_sigma;

        // Mean-density diagnostics from the profile-independent bypass model.
        // These should remain close to zero because the process look preserves
        // mean spectral density instead of adding a neutral density veil.
        float negative_bleach_mean_density_delta = 0.0f;
        float print_bleach_mean_density_delta = 0.0f;

        std::array<float, 3> ap0 = {{0.0f, 0.0f, 0.0f}};
        std::array<float, 3> rec709_gamma24 = {{0.0f, 0.0f, 0.0f}};

        bool valid = false;
    };

    FilmPipeline();
    ~FilmPipeline();

    FilmPipeline(const FilmPipeline&) = delete;
    FilmPipeline& operator=(const FilmPipeline&) = delete;

    bool initialize();

    bool initialize(
        const Settings& settings);

    bool valid() const;

    Result process(
        const std::array<float, 3>& ap0_linear) const;

    // Image-space effects such as halation need access to the developed film
    // path at the negative-exposure boundary. These helpers expose that
    // boundary without exposing stock/profile internals.
    bool negative_exposure(
        const std::array<float, 3>& ap0_linear,
        FilmExposure& exposure) const;

    // Diagnostic access to the exposure-separated rgb2spec scene factor used
    // immediately before D60 illumination.
    bool scene_factor(
        const std::array<float, 3>& ap0_linear,
        SampledCurve& factor) const;

    Result process_negative_exposure(
        const FilmExposure& negative_exposure) const;

    const Settings& settings() const;
    const std::string& error() const;

    const FilmDensityCalibration* negative_density_calibration() const;

private:
    SampledCurve load_minimum_negative_density_curve(
        const std::string& filename) const;

    SampledCurve density_from_print_records(
        const FilmDensity& record_density) const;

    static SampledCurve transmittance_from_density(
        const SampledCurve& density);

    SampledCurve reconstruct_scene_factor(
        const std::array<float, 3>& ap0_linear) const;

    FilmLogExposure relative_negative_log_exposure(
        const FilmExposure& exposure) const;

    static bool finite_rgb(
        const std::array<float, 3>& rgb);

    std::string resource_path(
        const std::string& filename) const;

    Settings settings_;
    std::string error_;
    bool valid_ = false;

    std::unique_ptr<SpectralReconstructor> reconstructor_;
    std::unique_ptr<SpectralIlluminant> scene_illuminant_;
    std::unique_ptr<FilmStock> negative_stock_;
    std::unique_ptr<GranularityModel> granularity_model_;
    std::unique_ptr<FilmProcessor> negative_processor_;
    std::unique_ptr<FilmDyeModel> negative_dye_model_;
    std::unique_ptr<FilmDensityCalibration> negative_density_calibration_;
    std::unique_ptr<FilmColorResponse> color_response_;
    std::unique_ptr<PrintFilmStock> print_stock_;
    std::unique_ptr<PrintFilmProcessor> print_processor_;
    std::unique_ptr<PrintViewer> viewer_;
    std::unique_ptr<ColorTransform> ap0_to_rec709_;

    FilmExposure reference_negative_exposure_;
    FilmDensity reference_negative_density_;
    FilmDensity reference_calibrated_negative_density_;
    SampledCurve reference_negative_transmittance_;

};
