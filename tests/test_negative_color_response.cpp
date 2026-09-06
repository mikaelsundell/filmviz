// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Family
{
    const char* name;
    std::array<float, 3> ap0;
};

struct ScopeValues
{
    double y = 0.0;
    double cb = 0.0;
    double cr = 0.0;
    double hue_degrees = 0.0;
    double chroma = 0.0;
    double blue_minus_green = 0.0;
};

bool
finite_exposure(
    const FilmExposure& exposure)
{
    return
        std::isfinite(exposure.red)
        && std::isfinite(exposure.green)
        && std::isfinite(exposure.blue);
}

bool
finite_density(
    const FilmDensity& density)
{
    return
        std::isfinite(density.red)
        && std::isfinite(density.green)
        && std::isfinite(density.blue);
}

bool
finite_rgb(
    const std::array<float, 3>& rgb)
{
    return
        std::isfinite(rgb[0])
        && std::isfinite(rgb[1])
        && std::isfinite(rgb[2]);
}

FilmExposure
scale_exposure(
    const FilmExposure& exposure,
    float factor)
{
    FilmExposure scaled;
    scaled.red = exposure.red * factor;
    scaled.green = exposure.green * factor;
    scaled.blue = exposure.blue * factor;
    return scaled;
}

std::array<float, 3>
scale_rgb(
    const std::array<float, 3>& rgb,
    float factor)
{
    return {{
        rgb[0] * factor,
        rgb[1] * factor,
        rgb[2] * factor
    }};
}

double
relative_error(
    double actual,
    double expected)
{
    const double denominator =
        std::max(
            1e-12,
            std::abs(expected));

    return
        std::abs(actual - expected)
        / denominator;
}

double
max_relative_error(
    const FilmExposure& actual,
    const FilmExposure& expected)
{
    return
        std::max({
            relative_error(actual.red, expected.red),
            relative_error(actual.green, expected.green),
            relative_error(actual.blue, expected.blue)
        });
}

ScopeValues
scope_values(
    const std::array<float, 3>& rgb)
{
    // Same BT.709 Y'CbCr geometry used by the Python diagnostic vectorscope.
    // The values here are already Rec.709/Gamma 2.4 display code values.
    constexpr double kr = 0.2126;
    constexpr double kb = 0.0722;
    constexpr double kg = 1.0 - kr - kb;

    const double r = rgb[0];
    const double g = rgb[1];
    const double b = rgb[2];

    ScopeValues values;
    values.y =
        kr * r
        + kg * g
        + kb * b;

    values.cb =
        (b - values.y)
        / (2.0 * (1.0 - kb));

    values.cr =
        (r - values.y)
        / (2.0 * (1.0 - kr));

    values.hue_degrees =
        std::atan2(
            values.cr,
            values.cb)
        * 180.0
        / kPi;

    if (values.hue_degrees < 0.0) {
        values.hue_degrees += 360.0;
    }

    values.chroma =
        std::sqrt(
            values.cb * values.cb
            + values.cr * values.cr);

    values.blue_minus_green =
        b - g;

    return values;
}

bool
write_header(
    std::ofstream& file)
{
    if (!file) {
        return false;
    }

    file
        << "stock,family,path,stop,input_r,input_g,input_b,"
        << "neg_h_r,neg_h_g,neg_h_b,h_vs_boundary_max_rel_error,"
        << "status_m_r,status_m_g,status_m_b,status_m_b_minus_g,"
        << "cal_density_r,cal_density_g,cal_density_b,cal_density_b_minus_g,"
        << "print_density_r,print_density_g,print_density_b,"
        << "out_r,out_g,out_b,out_y,out_cb,out_cr,out_hue_deg,out_chroma,out_b_minus_g\n";

    return true;
}

void
write_row(
    std::ofstream& file,
    const std::string& stock,
    const Family& family,
    const char* path,
    float stop,
    const std::array<float, 3>& input,
    const FilmExposure& exposure,
    double exposure_error,
    const FilmPipeline::Result& result)
{
    const ScopeValues scope =
        scope_values(
            result.rec709_gamma24);

    file
        << stock << ","
        << family.name << ","
        << path << ","
        << stop << ","
        << input[0] << ","
        << input[1] << ","
        << input[2] << ","
        << exposure.red << ","
        << exposure.green << ","
        << exposure.blue << ","
        << exposure_error << ","
        << result.negative_status_m_density.red << ","
        << result.negative_status_m_density.green << ","
        << result.negative_status_m_density.blue << ","
        << result.negative_status_m_density.blue
            - result.negative_status_m_density.green << ","
        << result.calibrated_negative_density.red << ","
        << result.calibrated_negative_density.green << ","
        << result.calibrated_negative_density.blue << ","
        << result.calibrated_negative_density.blue
            - result.calibrated_negative_density.green << ","
        << result.print_density.red << ","
        << result.print_density.green << ","
        << result.print_density.blue << ","
        << result.rec709_gamma24[0] << ","
        << result.rec709_gamma24[1] << ","
        << result.rec709_gamma24[2] << ","
        << scope.y << ","
        << scope.cb << ","
        << scope.cr << ","
        << scope.hue_degrees << ","
        << scope.chroma << ","
        << scope.blue_minus_green
        << "\n";
}

bool
run_stock(
    const std::string& stock,
    const std::filesystem::path& output_directory)
{
    FilmPipeline::Settings settings;
    settings.resources_directory = FILMVIZ_TEST_RESOURCE_DIR;
    settings.negative_profile = stock;
    settings.print_profile = "kodak-2383";
    settings.exposure_stops = 0.0f;
    settings.push_pull_stops = 0.0f;
    settings.negative_bleach_bypass = 0.0f;
    settings.print_bleach_bypass = 0.0f;
    settings.printer_light_red = 25.0f;
    settings.printer_light_green = 25.0f;
    settings.printer_light_blue = 25.0f;
    settings.printer_temperature_kelvin = 3200.0f;

    FilmPipeline pipeline;

    bool passed = true;

    passed &= test::check(
        pipeline.initialize(
            settings),
        std::string("pipeline initializes for ")
            + stock
            + ": "
            + pipeline.error());

    if (!passed) {
        return false;
    }

    // Fixed chromaticity families. Exposure is swept independently below.
    // The red families deliberately range from a practical warm red to a very
    // saturated deep red so we can see where blue starts overtaking green.
    const std::vector<Family> families = {
        {"neutral", {{0.18f, 0.18f, 0.18f}}},
        {"warm_red", {{0.18f, 0.045f, 0.018f}}},
        {"red", {{0.18f, 0.018f, 0.009f}}},
        {"deep_red", {{0.18f, 0.006f, 0.003f}}},
        {"yellow", {{0.18f, 0.14f, 0.012f}}},
        {"green", {{0.018f, 0.18f, 0.012f}}},
        {"blue", {{0.012f, 0.018f, 0.18f}}}
    };

    const std::filesystem::path csv_path =
        output_directory
        / (stock + ".csv");

    std::ofstream csv(
        csv_path.string().c_str());

    passed &= test::check(
        write_header(csv),
        std::string("CSV opens for ")
            + stock);

    if (!passed) {
        return false;
    }

    csv << std::setprecision(10);

    std::cout
        << "\nNegative color response: "
        << stock
        << "\n";

    for (const Family& family : families) {
        FilmExposure base_exposure;

        passed &= test::check(
            pipeline.negative_exposure(
                family.ap0,
                base_exposure)
            && finite_exposure(
                base_exposure),
            std::string("base negative exposure is finite for ")
                + stock
                + " / "
                + family.name);

        if (!finite_exposure(base_exposure)) {
            continue;
        }

        bool have_zero = false;
        ScopeValues zero_scope;
        FilmDensity zero_status_m;
        FilmDensity zero_calibrated;

        double maximum_reconstruction_error = 0.0;
        double brightest_hue = 0.0;
        double brightest_blue_minus_green = 0.0;
        double brightest_status_m_blue_minus_green = 0.0;
        double brightest_calibrated_blue_minus_green = 0.0;
        float brightest_stop = 0.0f;

        FilmDensity previous_neutral_density;
        bool have_previous_neutral = false;

        for (int half_stop = -12;
             half_stop <= 12;
             ++half_stop) {

            const float stop =
                0.5f
                * static_cast<float>(half_stop);

            const float factor =
                std::exp2(stop);

            const std::array<float, 3> scaled_input =
                scale_rgb(
                    family.ap0,
                    factor);

            const FilmExposure boundary_exposure =
                scale_exposure(
                    base_exposure,
                    factor);

            FilmExposure reconstructed_exposure;
            const bool reconstructed_ok =
                pipeline.negative_exposure(
                    scaled_input,
                    reconstructed_exposure)
                && finite_exposure(
                    reconstructed_exposure);

            passed &= test::check(
                reconstructed_ok,
                std::string("scaled scene reconstructs for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            if (!reconstructed_ok) {
                continue;
            }

            const double reconstruction_error =
                max_relative_error(
                    reconstructed_exposure,
                    boundary_exposure);

            maximum_reconstruction_error =
                std::max(
                    maximum_reconstruction_error,
                    reconstruction_error);

            const FilmPipeline::Result scene_result =
                pipeline.process_negative_exposure(
                    reconstructed_exposure);

            const FilmPipeline::Result boundary_result =
                pipeline.process_negative_exposure(
                    boundary_exposure);

            passed &= test::check(
                scene_result.valid
                && boundary_result.valid
                && finite_density(scene_result.negative_status_m_density)
                && finite_density(scene_result.calibrated_negative_density)
                && finite_density(scene_result.print_density)
                && finite_rgb(scene_result.rec709_gamma24)
                && finite_density(boundary_result.negative_status_m_density)
                && finite_density(boundary_result.calibrated_negative_density)
                && finite_density(boundary_result.print_density)
                && finite_rgb(boundary_result.rec709_gamma24),
                std::string("all stages stay finite for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            if (!scene_result.valid
                || !boundary_result.valid) {
                continue;
            }

            write_row(
                csv,
                stock,
                family,
                "scene_reconstruct",
                stop,
                scaled_input,
                reconstructed_exposure,
                reconstruction_error,
                scene_result);

            write_row(
                csv,
                stock,
                family,
                "boundary_scaled",
                stop,
                scaled_input,
                boundary_exposure,
                0.0,
                boundary_result);

            if (family.name == std::string("neutral")) {
                if (have_previous_neutral) {
                    passed &= test::check(
                        boundary_result.negative_status_m_density.red
                            >= previous_neutral_density.red - 1e-5f
                        && boundary_result.negative_status_m_density.green
                            >= previous_neutral_density.green - 1e-5f
                        && boundary_result.negative_status_m_density.blue
                            >= previous_neutral_density.blue - 1e-5f,
                        std::string("neutral negative density is monotonic for ")
                            + stock);
                }

                previous_neutral_density =
                    boundary_result.negative_status_m_density;
                have_previous_neutral = true;
            }

            const ScopeValues boundary_scope =
                scope_values(
                    boundary_result.rec709_gamma24);

            if (std::abs(stop) < 1e-6f) {
                have_zero = true;
                zero_scope = boundary_scope;
                zero_status_m =
                    boundary_result.negative_status_m_density;
                zero_calibrated =
                    boundary_result.calibrated_negative_density;
            }

            if (stop > brightest_stop
                || half_stop == -12) {
                brightest_stop = stop;
                brightest_hue = boundary_scope.hue_degrees;
                brightest_blue_minus_green =
                    boundary_scope.blue_minus_green;
                brightest_status_m_blue_minus_green =
                    boundary_result.negative_status_m_density.blue
                    - boundary_result.negative_status_m_density.green;
                brightest_calibrated_blue_minus_green =
                    boundary_result.calibrated_negative_density.blue
                    - boundary_result.calibrated_negative_density.green;
            }
        }

        std::cout
            << "  "
            << std::setw(10)
            << family.name
            << "  max spectral-H rel.err="
            << maximum_reconstruction_error;

        if (have_zero) {
            const double hue_delta =
                brightest_hue
                - zero_scope.hue_degrees;

            std::cout
                << "  hue@0="
                << zero_scope.hue_degrees
                << "  hue@"
                << std::showpos
                << brightest_stop
                << std::noshowpos
                << "="
                << brightest_hue
                << "  delta="
                << hue_delta
                << "  out(B-G)="
                << zero_scope.blue_minus_green
                << " -> "
                << brightest_blue_minus_green
                << "  StatusM(B-G)="
                << zero_status_m.blue - zero_status_m.green
                << " -> "
                << brightest_status_m_blue_minus_green
                << "  CalD(B-G)="
                << zero_calibrated.blue - zero_calibrated.green
                << " -> "
                << brightest_calibrated_blue_minus_green;
        }

        std::cout << "\n";
    }

    std::cout
        << "  CSV: "
        << csv_path
        << "\n";

    return passed;
}

} // namespace

int
main()
{
    bool passed = true;

    const std::filesystem::path output_directory =
        std::filesystem::path("tests")
        / "output"
        / "test_negative_color_response";

    std::error_code error;
    std::filesystem::create_directories(
        output_directory,
        error);

    passed &= test::check(
        !error,
        std::string("output directory is available: ")
            + output_directory.string());

    if (error) {
        return
            test::finish(
                false,
                "negative color response");
    }

    passed &= run_stock(
        "verita-200d",
        output_directory);

    passed &= run_stock(
        "kodak-50d",
        output_directory);

    return
        test::finish(
            passed,
            "negative color response");
}
