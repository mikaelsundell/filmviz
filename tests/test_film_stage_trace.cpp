// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdyemodel.h"
#include "filmpipeline.h"
#include "filmstock.h"
#include "printfilmprocessor.h"
#include "printfilmstock.h"
#include "printviewer.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct Family
{
    const char* name;
    std::array<float, 3> ap0;
};

struct AuditContext
{
    FilmStock negative_stock;
    FilmDyeModel negative_dye_model;
    PrintFilmStock print_stock;
    SampledCurve reference_negative_transmittance;
    SampledCurve printer_illuminant;
    std::unique_ptr<PrintFilmProcessor> print_processor;
    PrintViewer viewer;

    explicit AuditContext(
        const std::string& negative_name)
        : negative_stock(negative_name)
        , print_stock("Kodak 2383 corrected")
    {
    }
};

bool
finite_exposure(
    const FilmExposure& value)
{
    return
        std::isfinite(value.red)
        && std::isfinite(value.green)
        && std::isfinite(value.blue);
}

bool
finite_log_exposure(
    const FilmLogExposure& value)
{
    return
        std::isfinite(value.red)
        && std::isfinite(value.green)
        && std::isfinite(value.blue);
}

bool
finite_density(
    const FilmDensity& value)
{
    return
        std::isfinite(value.red)
        && std::isfinite(value.green)
        && std::isfinite(value.blue);
}

bool
finite_rgb(
    const std::array<float, 3>& value)
{
    return
        std::isfinite(value[0])
        && std::isfinite(value[1])
        && std::isfinite(value[2]);
}

FilmExposure
scale_exposure(
    const FilmExposure& value,
    float factor)
{
    FilmExposure result;
    result.red = value.red * factor;
    result.green = value.green * factor;
    result.blue = value.blue * factor;
    return result;
}

SampledCurve
density_from_print_records(
    const PrintFilmStock& print_stock,
    const FilmPipeline::Settings& settings,
    const FilmDensity& record_density)
{
    SampledCurve result;

    const auto& dye =
        print_stock.dye_density();

    for (float wavelength =
             settings.wavelength_min_nm;
         wavelength <=
             settings.wavelength_max_nm
             + 0.001f;
         wavelength +=
             settings.wavelength_step_nm) {

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

        const double total =
            settings.print_cyan_amplitude
                * static_cast<double>(record_density.red)
                * static_cast<double>(c)
            + settings.print_magenta_amplitude
                * static_cast<double>(record_density.green)
                * static_cast<double>(m)
            + settings.print_yellow_amplitude
                * static_cast<double>(record_density.blue)
                * static_cast<double>(y);

        result.x.push_back(
            wavelength);

        result.y.push_back(
            static_cast<float>(
                std::max(
                    0.0,
                    total)));
    }

    return result;
}

SampledCurve
transmittance_from_density(
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

double
curve_mean(
    const SampledCurve& curve)
{
    if (!curve.valid()
        || curve.y.empty()) {
        return
            std::numeric_limits<double>::quiet_NaN();
    }

    double sum = 0.0;

    for (float value : curve.y) {
        sum +=
            static_cast<double>(value);
    }

    return
        sum
        / static_cast<double>(
            curve.y.size());
}

double
curve_sample(
    const SampledCurve& curve,
    float wavelength)
{
    return static_cast<double>(
        curve.sample(
            wavelength,
            std::numeric_limits<float>::quiet_NaN()));
}

double
red_magenta_indicator(
    const std::array<float, 3>& ap0)
{
    // Diagnostic only: for a red family, positive B-G means the blue
    // component is winning over green and therefore moving red toward
    // magenta rather than toward yellow/orange.
    return
        static_cast<double>(ap0[2])
        - static_cast<double>(ap0[1]);
}

bool
initialize_audit(
    const std::string& stock,
    const FilmPipeline::Settings& settings,
    AuditContext& audit)
{
    namespace fs = std::filesystem;

    const fs::path resources(
        settings.resources_directory);

    std::string negative_directory;
    std::string negative_prefix;

    if (stock == "verita-200d") {
        negative_directory =
            "profiles/verita_200d";
        negative_prefix =
            "kodak_verita_200d";
    }
    else if (stock == "kodak-50d") {
        negative_directory =
            "profiles/kodak_50d";
        negative_prefix =
            "kodak_50d";
    }
    else {
        return false;
    }

    const fs::path negative_sensitivity =
        resources
        / negative_directory
        / (negative_prefix
           + "_spectral_sensitivity_curves.csv");

    const fs::path negative_characteristic =
        resources
        / negative_directory
        / (negative_prefix
           + "_sensitometric_curves.csv");

    const fs::path negative_dye =
        resources
        / negative_directory
        / (negative_prefix
           + "_spectral_dye_density_curves.csv");

    if (!audit.negative_stock.load(
            negative_sensitivity.string(),
            negative_characteristic.string())) {

        return false;
    }

    if (!audit.negative_dye_model.load_and_estimate(
            negative_dye.string(),
            audit.negative_stock,
            settings.wavelength_min_nm,
            settings.wavelength_max_nm,
            settings.wavelength_step_nm,
            settings.negative_zero_stop_log_exposure)) {

        return false;
    }

    const FilmDensity reference_record =
        audit.negative_dye_model.neutral_record_density(
            audit.negative_stock,
            settings.negative_zero_stop_log_exposure);

    audit.reference_negative_transmittance =
        audit.negative_dye_model.synthesize_transmittance(
            reference_record);

    if (!audit.reference_negative_transmittance.valid()) {
        return false;
    }

    const fs::path print_root =
        resources
        / "profiles/kodak_2383";

    if (!audit.print_stock.load(
            (print_root
             / "kodak_2383_spectral_sensitivity_curves.csv").string(),
            (print_root
             / "kodak_2383_sensitometric_curves.csv").string(),
            (print_root
             / "kodak_2383_corrected_spectral_dye_density_curves.csv").string(),
            (print_root
             / "kodak_2383_modulation_transfer_function_curves.csv").string(),
            (print_root
             / "kodak_2383_diffuse_rms_granularity_curves.csv").string())) {

        return false;
    }

    audit.printer_illuminant =
        PrintFilmProcessor::make_blackbody_illuminant(
            settings.printer_temperature_kelvin,
            settings.wavelength_min_nm,
            settings.wavelength_max_nm,
            settings.wavelength_step_nm);

    if (!audit.printer_illuminant.valid()) {
        return false;
    }

    PrintFilmProcessor::Settings print_settings;
    print_settings.exposure_stops = 0.0f;
    print_settings.log_exposure_calibration = 0.0f;
    print_settings.wavelength_min_nm =
        settings.wavelength_min_nm;
    print_settings.wavelength_max_nm =
        settings.wavelength_max_nm;
    print_settings.wavelength_step_nm =
        settings.wavelength_step_nm;
    print_settings.reference_status_a_density =
        settings.print_reference_status_a_density;

    // These members exist in the current production API and keep this audit
    // on the exact same printer-light operating point as FilmPipeline.
    print_settings.printer_light_red =
        settings.printer_light_red;
    print_settings.printer_light_green =
        settings.printer_light_green;
    print_settings.printer_light_blue =
        settings.printer_light_blue;

    audit.print_processor =
        std::make_unique<PrintFilmProcessor>(
            audit.print_stock,
            audit.printer_illuminant,
            audit.reference_negative_transmittance,
            print_settings);

    if (!audit.print_processor->valid()) {
        return false;
    }

    PrintViewer::Settings viewer_settings;
    viewer_settings.wavelength_min_nm =
        settings.wavelength_min_nm;
    viewer_settings.wavelength_max_nm =
        settings.wavelength_max_nm;
    viewer_settings.wavelength_step_nm =
        settings.wavelength_step_nm;

    if (!audit.viewer.load(
            (resources
             / "colorimetry/observers/CIE_xyz_1931_2deg.csv").string(),
            (resources
             / "colorimetry/illuminants/CIE_std_illum_D55.csv").string(),
            viewer_settings)) {

        return false;
    }

    return
        audit.viewer.valid();
}

bool
run_stock(
    const std::string& stock,
    const std::string& negative_name,
    const std::filesystem::path& output_directory)
{
    FilmPipeline::Settings settings;
    settings.resources_directory =
        FILMVIZ_TEST_RESOURCE_DIR;
    settings.negative_profile =
        stock;
    settings.print_profile =
        "kodak-2383";
    settings.exposure_stops =
        0.0f;
    settings.push_pull_stops =
        0.0f;
    settings.negative_bleach_bypass =
        0.0f;
    settings.print_bleach_bypass =
        0.0f;
    settings.printer_light_red =
        25.0f;
    settings.printer_light_green =
        25.0f;
    settings.printer_light_blue =
        25.0f;
    settings.printer_temperature_kelvin =
        3200.0f;

    FilmPipeline pipeline;

    bool passed = true;

    passed &= test::check(
        pipeline.initialize(
            settings),
        std::string("production pipeline initializes for ")
            + stock
            + ": "
            + pipeline.error());

    if (!passed) {
        return false;
    }

    AuditContext audit(
        negative_name);

    passed &= test::check(
        initialize_audit(
            stock,
            settings,
            audit),
        std::string("independent film-stage audit initializes for ")
            + stock);

    if (!passed) {
        return false;
    }

    const std::vector<Family> families = {
        {
            "warm_red",
            {{0.18f, 0.045f, 0.018f}}
        },
        {
            "red",
            {{0.18f, 0.018f, 0.009f}}
        }
    };

    const std::array<float, 6> stops = {{
        -2.0f,
        0.0f,
        1.0f,
        2.0f,
        4.0f,
        6.0f
    }};

    const std::filesystem::path csv_path =
        output_directory
        / (stock + ".csv");

    std::ofstream csv(
        csv_path);

    passed &= test::check(
        static_cast<bool>(csv),
        std::string("stage-trace CSV opens for ")
            + stock);

    if (!csv) {
        return false;
    }

    csv
        << "stock,family,stop,"
        << "neg_h_r,neg_h_g,neg_h_b,"
        << "status_r,status_g,status_b,"
        << "cal_r,cal_g,cal_b,"
        << "neg_density_mean,"
        << "neg_density_450,neg_density_550,neg_density_650,"
        << "neg_trans_450,neg_trans_550,neg_trans_650,"
        << "print_h_r,print_h_g,print_h_b,"
        << "print_logh_r,print_logh_g,print_logh_b,"
        << "print_density_r,print_density_g,print_density_b,"
        << "print_spectral_density_mean,"
        << "print_density_450,print_density_550,print_density_650,"
        << "ap0_r,ap0_g,ap0_b,ap0_b_minus_g,"
        << "production_ap0_r,production_ap0_g,production_ap0_b,"
        << "production_ap0_b_minus_g,"
        << "manual_vs_production_max_abs\n";

    csv
        << std::setprecision(10);

    std::cout
        << "\nFilm stage trace: "
        << stock
        << "\n";

    for (const Family& family : families) {
        FilmExposure base_exposure;

        const bool base_ok =
            pipeline.negative_exposure(
                family.ap0,
                base_exposure)
            && finite_exposure(
                base_exposure);

        passed &= test::check(
            base_ok,
            std::string("base negative exposure is finite for ")
                + stock
                + " / "
                + family.name);

        if (!base_ok) {
            continue;
        }

        std::cout
            << "  "
            << family.name
            << "\n";

        for (float stop : stops) {
            const FilmExposure negative_exposure =
                scale_exposure(
                    base_exposure,
                    std::exp2(stop));

            const FilmPipeline::Result production =
                pipeline.process_negative_exposure(
                    negative_exposure);

            passed &= test::check(
                production.valid,
                std::string("production stage is valid for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            if (!production.valid) {
                continue;
            }

            const SampledCurve negative_density =
                audit.negative_dye_model.synthesize_density(
                    production.calibrated_negative_density);

            const SampledCurve negative_transmittance =
                audit.negative_dye_model.synthesize_transmittance(
                    production.calibrated_negative_density);

            const FilmExposure print_exposure =
                audit.print_processor->expose(
                    negative_transmittance);

            const FilmLogExposure print_log_exposure =
                audit.print_processor->log_exposure(
                    print_exposure);

            const FilmDensity print_density =
                audit.print_processor->develop(
                    print_log_exposure);

            const SampledCurve print_spectral_density =
                density_from_print_records(
                    audit.print_stock,
                    settings,
                    print_density);

            const SampledCurve print_transmittance =
                transmittance_from_density(
                    print_spectral_density);

            const PrintViewer::Result viewed =
                audit.viewer.view(
                    print_transmittance);

            const std::array<float, 3> manual_ap0 =
                viewed.aces2065_1;

            const bool manual_valid =
                negative_density.valid()
                && negative_transmittance.valid()
                && finite_exposure(print_exposure)
                && finite_log_exposure(print_log_exposure)
                && finite_density(print_density)
                && print_spectral_density.valid()
                && print_transmittance.valid()
                && finite_rgb(manual_ap0);

            passed &= test::check(
                manual_valid,
                std::string("manual stage trace is finite for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            if (!manual_valid) {
                continue;
            }

            const double max_abs =
                std::max({
                    std::abs(
                        static_cast<double>(manual_ap0[0])
                        - static_cast<double>(production.ap0[0])),
                    std::abs(
                        static_cast<double>(manual_ap0[1])
                        - static_cast<double>(production.ap0[1])),
                    std::abs(
                        static_cast<double>(manual_ap0[2])
                        - static_cast<double>(production.ap0[2]))
                });

            csv
                << stock << ','
                << family.name << ','
                << stop << ','
                << negative_exposure.red << ','
                << negative_exposure.green << ','
                << negative_exposure.blue << ','
                << production.negative_status_m_density.red << ','
                << production.negative_status_m_density.green << ','
                << production.negative_status_m_density.blue << ','
                << production.calibrated_negative_density.red << ','
                << production.calibrated_negative_density.green << ','
                << production.calibrated_negative_density.blue << ','
                << curve_mean(negative_density) << ','
                << curve_sample(negative_density, 450.0f) << ','
                << curve_sample(negative_density, 550.0f) << ','
                << curve_sample(negative_density, 650.0f) << ','
                << curve_sample(negative_transmittance, 450.0f) << ','
                << curve_sample(negative_transmittance, 550.0f) << ','
                << curve_sample(negative_transmittance, 650.0f) << ','
                << print_exposure.red << ','
                << print_exposure.green << ','
                << print_exposure.blue << ','
                << print_log_exposure.red << ','
                << print_log_exposure.green << ','
                << print_log_exposure.blue << ','
                << print_density.red << ','
                << print_density.green << ','
                << print_density.blue << ','
                << curve_mean(print_spectral_density) << ','
                << curve_sample(print_spectral_density, 450.0f) << ','
                << curve_sample(print_spectral_density, 550.0f) << ','
                << curve_sample(print_spectral_density, 650.0f) << ','
                << manual_ap0[0] << ','
                << manual_ap0[1] << ','
                << manual_ap0[2] << ','
                << red_magenta_indicator(manual_ap0) << ','
                << production.ap0[0] << ','
                << production.ap0[1] << ','
                << production.ap0[2] << ','
                << red_magenta_indicator(production.ap0) << ','
                << max_abs
                << '\n';

            std::cout
                << "    stop="
                << std::setw(4)
                << stop
                << " StatusM(B-G)="
                << (
                    production.negative_status_m_density.blue
                    - production.negative_status_m_density.green)
                << " CalD(B-G)="
                << (
                    production.calibrated_negative_density.blue
                    - production.calibrated_negative_density.green)
                << " negT(450/550/650)=("
                << curve_sample(negative_transmittance, 450.0f)
                << ", "
                << curve_sample(negative_transmittance, 550.0f)
                << ", "
                << curve_sample(negative_transmittance, 650.0f)
                << ") printH=("
                << print_exposure.red
                << ", "
                << print_exposure.green
                << ", "
                << print_exposure.blue
                << ") printD=("
                << print_density.red
                << ", "
                << print_density.green
                << ", "
                << print_density.blue
                << ") AP0=("
                << manual_ap0[0]
                << ", "
                << manual_ap0[1]
                << ", "
                << manual_ap0[2]
                << ") B-G="
                << red_magenta_indicator(manual_ap0)
                << " manual-vs-prod="
                << max_abs
                << '\n';
        }
    }

    std::cout
        << "  CSV: "
        << csv_path
        << '\n';

    return passed;
}

} // namespace

int
main()
{
    namespace fs = std::filesystem;

    const fs::path output_directory =
        fs::path("tests")
        / "output"
        / "test_film_stage_trace";

    std::error_code error;

    fs::create_directories(
        output_directory,
        error);

    bool passed = true;

    passed &= test::check(
        !error,
        "film-stage trace output directory exists");

    if (error) {
        return 1;
    }

    passed &=
        run_stock(
            "verita-200d",
            "Kodak Verita 200D",
            output_directory);

    passed &=
        run_stock(
            "kodak-50d",
            "Kodak VISION3 50D 5203/7203",
            output_directory);

    return
        test::finish(
            passed,
            "film stage trace");
}
