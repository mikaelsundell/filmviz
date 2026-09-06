// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
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

struct StageSample
{
    float stop = 0.0f;
    FilmExposure exposure;
    FilmDensity status_m;
    FilmDensity calibrated;
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
finite_density(
    const FilmDensity& value)
{
    return
        std::isfinite(value.red)
        && std::isfinite(value.green)
        && std::isfinite(value.blue);
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

double
safe_ratio(
    double numerator,
    double denominator)
{
    if (std::abs(denominator) <= 1e-20) {
        return 0.0;
    }

    return numerator / denominator;
}

double
safe_log10(
    double value)
{
    return
        std::log10(
            std::max(
                value,
                1e-30));
}

double
slope(
    double a,
    double b,
    double stop_a,
    double stop_b)
{
    const double span =
        stop_b - stop_a;

    if (std::abs(span) <= 1e-12) {
        return 0.0;
    }

    return
        (b - a)
        / span;
}

const StageSample*
find_stop(
    const std::vector<StageSample>& samples,
    float stop)
{
    for (const StageSample& sample : samples) {
        if (std::abs(sample.stop - stop) < 1e-6f) {
            return &sample;
        }
    }

    return nullptr;
}

bool
run_stock(
    const std::string& stock,
    const std::filesystem::path& output_directory)
{
    FilmPipeline::Settings settings;
    settings.resources_directory =
        FILMVIZ_TEST_RESOURCE_DIR;
    settings.negative_profile =
        stock;
    settings.print_profile =
        "none";
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
        std::string("pipeline initializes for ")
            + stock
            + ": "
            + pipeline.error());

    if (!passed) {
        return false;
    }

    const std::vector<Family> families = {
        {
            "neutral",
            {{0.18f, 0.18f, 0.18f}}
        },
        {
            "warm_red",
            {{0.18f, 0.045f, 0.018f}}
        },
        {
            "red",
            {{0.18f, 0.018f, 0.009f}}
        },
        {
            "deep_red",
            {{0.18f, 0.006f, 0.003f}}
        }
    };

    const std::filesystem::path csv_path =
        output_directory
        / (stock + ".csv");

    std::ofstream csv(
        csv_path.string().c_str());

    passed &= test::check(
        static_cast<bool>(csv),
        std::string("CSV opens for ")
            + stock);

    if (!csv) {
        return false;
    }

    csv
        << "stock,family,stop,"
        << "h_r,h_g,h_b,"
        << "h_r_over_g,h_b_over_g,"
        << "logh_r,logh_g,logh_b,"
        << "logh_r_minus_g,logh_b_minus_g,"
        << "status_r,status_g,status_b,"
        << "status_r_minus_g,status_b_minus_g,"
        << "cal_r,cal_g,cal_b,"
        << "cal_r_minus_g,cal_b_minus_g\n";

    csv
        << std::setprecision(10);

    std::cout
        << "\nNegative density mapping: "
        << stock
        << "\n";

    for (const Family& family : families) {
        FilmExposure base_exposure;

        const bool exposure_ok =
            pipeline.negative_exposure(
                family.ap0,
                base_exposure)
            && finite_exposure(
                base_exposure);

        passed &= test::check(
            exposure_ok,
            std::string("base exposure is finite for ")
                + stock
                + " / "
                + family.name);

        if (!exposure_ok) {
            continue;
        }

        const double base_r_over_g =
            safe_ratio(
                base_exposure.red,
                base_exposure.green);

        const double base_b_over_g =
            safe_ratio(
                base_exposure.blue,
                base_exposure.green);

        std::vector<StageSample> samples;

        double worst_r_over_g_error = 0.0;
        double worst_b_over_g_error = 0.0;

        for (int half_stop = -16;
             half_stop <= 16;
             ++half_stop) {

            const float stop =
                static_cast<float>(half_stop)
                * 0.5f;

            const float factor =
                std::exp2(stop);

            const FilmExposure exposure =
                scale_exposure(
                    base_exposure,
                    factor);

            const FilmPipeline::Result result =
                pipeline.process_negative_exposure(
                    exposure);

            const bool valid =
                result.valid
                && finite_density(
                    result.negative_status_m_density)
                && finite_density(
                    result.calibrated_negative_density);

            passed &= test::check(
                valid,
                std::string("density stages are finite for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            if (!valid) {
                continue;
            }

            const double r_over_g =
                safe_ratio(
                    exposure.red,
                    exposure.green);

            const double b_over_g =
                safe_ratio(
                    exposure.blue,
                    exposure.green);

            worst_r_over_g_error =
                std::max(
                    worst_r_over_g_error,
                    std::abs(
                        r_over_g
                        - base_r_over_g));

            worst_b_over_g_error =
                std::max(
                    worst_b_over_g_error,
                    std::abs(
                        b_over_g
                        - base_b_over_g));

            StageSample sample;
            sample.stop = stop;
            sample.exposure = exposure;
            sample.status_m =
                result.negative_status_m_density;
            sample.calibrated =
                result.calibrated_negative_density;

            samples.push_back(
                sample);

            const double logh_r =
                safe_log10(
                    exposure.red);

            const double logh_g =
                safe_log10(
                    exposure.green);

            const double logh_b =
                safe_log10(
                    exposure.blue);

            csv
                << stock << ','
                << family.name << ','
                << stop << ','
                << exposure.red << ','
                << exposure.green << ','
                << exposure.blue << ','
                << r_over_g << ','
                << b_over_g << ','
                << logh_r << ','
                << logh_g << ','
                << logh_b << ','
                << logh_r - logh_g << ','
                << logh_b - logh_g << ','
                << result.negative_status_m_density.red << ','
                << result.negative_status_m_density.green << ','
                << result.negative_status_m_density.blue << ','
                << result.negative_status_m_density.red
                    - result.negative_status_m_density.green << ','
                << result.negative_status_m_density.blue
                    - result.negative_status_m_density.green << ','
                << result.calibrated_negative_density.red << ','
                << result.calibrated_negative_density.green << ','
                << result.calibrated_negative_density.blue << ','
                << result.calibrated_negative_density.red
                    - result.calibrated_negative_density.green << ','
                << result.calibrated_negative_density.blue
                    - result.calibrated_negative_density.green
                << '\n';
        }

        const StageSample* minus_four =
            find_stop(
                samples,
                -4.0f);

        const StageSample* zero =
            find_stop(
                samples,
                0.0f);

        const StageSample* plus_four =
            find_stop(
                samples,
                4.0f);

        const StageSample* plus_six =
            find_stop(
                samples,
                6.0f);

        if (!minus_four
            || !zero
            || !plus_four
            || !plus_six) {

            passed &= test::check(
                false,
                std::string("summary stops exist for ")
                    + stock
                    + " / "
                    + family.name);

            continue;
        }

        const auto status_bg =
            [](const StageSample& value) {
                return
                    static_cast<double>(
                        value.status_m.blue)
                    - static_cast<double>(
                        value.status_m.green);
            };

        const auto calibrated_bg =
            [](const StageSample& value) {
                return
                    static_cast<double>(
                        value.calibrated.blue)
                    - static_cast<double>(
                        value.calibrated.green);
            };

        const auto status_rg =
            [](const StageSample& value) {
                return
                    static_cast<double>(
                        value.status_m.red)
                    - static_cast<double>(
                        value.status_m.green);
            };

        const auto calibrated_rg =
            [](const StageSample& value) {
                return
                    static_cast<double>(
                        value.calibrated.red)
                    - static_cast<double>(
                        value.calibrated.green);
            };

        const double status_bg_slope =
            slope(
                status_bg(*zero),
                status_bg(*plus_six),
                0.0,
                6.0);

        const double calibrated_bg_slope =
            slope(
                calibrated_bg(*zero),
                calibrated_bg(*plus_six),
                0.0,
                6.0);

        const double status_rg_slope =
            slope(
                status_rg(*zero),
                status_rg(*plus_six),
                0.0,
                6.0);

        const double calibrated_rg_slope =
            slope(
                calibrated_rg(*zero),
                calibrated_rg(*plus_six),
                0.0,
                6.0);

        std::cout
            << std::setw(10)
            << family.name
            << "  H R/G="
            << base_r_over_g
            << " B/G="
            << base_b_over_g
            << "  ratio drift=("
            << worst_r_over_g_error
            << ", "
            << worst_b_over_g_error
            << ")\n"
            << "            StatusM B-G: "
            << status_bg(*minus_four)
            << " -> "
            << status_bg(*zero)
            << " -> "
            << status_bg(*plus_four)
            << " -> "
            << status_bg(*plus_six)
            << "  slope[0,+6]="
            << status_bg_slope
            << "\n"
            << "            CalD    B-G: "
            << calibrated_bg(*minus_four)
            << " -> "
            << calibrated_bg(*zero)
            << " -> "
            << calibrated_bg(*plus_four)
            << " -> "
            << calibrated_bg(*plus_six)
            << "  slope[0,+6]="
            << calibrated_bg_slope
            << "\n"
            << "            StatusM R-G slope[0,+6]="
            << status_rg_slope
            << "  CalD R-G slope[0,+6]="
            << calibrated_rg_slope
            << "\n";
    }

    std::cout
        << "CSV: "
        << csv_path
        << "\n";

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
        / "test_negative_density_mapping";

    std::error_code error;

    fs::create_directories(
        output_directory,
        error);

    bool passed = true;

    passed &= test::check(
        !error,
        "negative density mapping output directory exists");

    if (error) {
        return 1;
    }

    passed &=
        run_stock(
            "verita-200d",
            output_directory);

    passed &=
        run_stock(
            "kodak-50d",
            output_directory);

    return passed
        ? 0
        : 1;
}
