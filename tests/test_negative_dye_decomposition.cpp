// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdyemodel.h"
#include "filmpipeline.h"
#include "filmstock.h"
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

struct DyeDecomposition
{
    float cyan_increment = 0.0f;
    float magenta_increment = 0.0f;
    float yellow_increment = 0.0f;

    SampledCurve minimum;
    SampledCurve cyan;
    SampledCurve magenta;
    SampledCurve yellow;
    SampledCurve reconstructed;
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
curve_area(
    const SampledCurve& curve)
{
    if (!curve.valid()
        || curve.x.size() < 2) {
        return 0.0;
    }

    double area = 0.0;

    for (std::size_t i = 1;
         i < curve.x.size();
         ++i) {

        const double dx =
            static_cast<double>(
                curve.x[i]
                - curve.x[i - 1]);

        const double y0 =
            static_cast<double>(
                curve.y[i - 1]);

        const double y1 =
            static_cast<double>(
                curve.y[i]);

        area +=
            0.5
            * (y0 + y1)
            * dx;
    }

    return area;
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
maximum_absolute_difference(
    const SampledCurve& a,
    const SampledCurve& b)
{
    if (!a.valid()
        || !b.valid()
        || a.x.size() != b.x.size()) {
        return
            std::numeric_limits<double>::infinity();
    }

    double result = 0.0;

    for (std::size_t i = 0;
         i < a.x.size();
         ++i) {

        result =
            std::max(
                result,
                std::abs(
                    static_cast<double>(a.y[i])
                    - static_cast<double>(b.y[i])));
    }

    return result;
}

bool
decompose(
    const FilmDyeModel& model,
    const FilmDensity& calibrated,
    DyeDecomposition& result)
{
    result =
        DyeDecomposition();

    if (!model.valid()
        || !model.calibrated()) {
        return false;
    }

    const FilmDensity& minimum_record =
        model.diagnostics().minimum_record_density;

    result.cyan_increment =
        std::max(
            0.0f,
            calibrated.red
            - minimum_record.red);

    result.magenta_increment =
        std::max(
            0.0f,
            calibrated.green
            - minimum_record.green);

    result.yellow_increment =
        std::max(
            0.0f,
            calibrated.blue
            - minimum_record.blue);

    const SampledCurve& minimum =
        model.minimum_density();

    const SampledCurve& cyan_basis =
        model.cyan_basis_per_record_density();

    const SampledCurve& magenta_basis =
        model.magenta_basis_per_record_density();

    const SampledCurve& yellow_basis =
        model.yellow_basis_per_record_density();

    if (!minimum.valid()
        || !cyan_basis.valid()
        || !magenta_basis.valid()
        || !yellow_basis.valid()
        || cyan_basis.x.size() != magenta_basis.x.size()
        || cyan_basis.x.size() != yellow_basis.x.size()) {
        return false;
    }

    for (std::size_t i = 0;
         i < cyan_basis.x.size();
         ++i) {

        const float wavelength =
            cyan_basis.x[i];

        const float dmin =
            minimum.sample(
                wavelength,
                0.0f);

        const float cyan =
            result.cyan_increment
            * cyan_basis.y[i];

        const float magenta =
            result.magenta_increment
            * magenta_basis.y[i];

        const float yellow =
            result.yellow_increment
            * yellow_basis.y[i];

        result.minimum.x.push_back(
            wavelength);
        result.minimum.y.push_back(
            dmin);

        result.cyan.x.push_back(
            wavelength);
        result.cyan.y.push_back(
            cyan);

        result.magenta.x.push_back(
            wavelength);
        result.magenta.y.push_back(
            magenta);

        result.yellow.x.push_back(
            wavelength);
        result.yellow.y.push_back(
            yellow);

        result.reconstructed.x.push_back(
            wavelength);
        result.reconstructed.y.push_back(
            std::max(
                0.0f,
                dmin
                + cyan
                + magenta
                + yellow));
    }

    return
        result.minimum.valid()
        && result.cyan.valid()
        && result.magenta.valid()
        && result.yellow.valid()
        && result.reconstructed.valid();
}

bool
initialize_dye_model(
    const std::string& stock,
    const FilmPipeline::Settings& settings,
    FilmStock& negative_stock,
    FilmDyeModel& dye_model)
{
    namespace fs = std::filesystem;

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

    const fs::path root(
        settings.resources_directory);

    const fs::path profile_root =
        root
        / negative_directory;

    if (!negative_stock.load(
            (profile_root
             / (negative_prefix
                + "_spectral_sensitivity_curves.csv")).string(),
            (profile_root
             / (negative_prefix
                + "_sensitometric_curves.csv")).string())) {

        return false;
    }

    return
        dye_model.load_and_estimate(
            (profile_root
             / (negative_prefix
                + "_spectral_dye_density_curves.csv")).string(),
            negative_stock,
            settings.wavelength_min_nm,
            settings.wavelength_max_nm,
            settings.wavelength_step_nm,
            settings.negative_zero_stop_log_exposure);
}

bool
run_stock(
    const std::string& stock,
    const std::string& stock_name,
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

    FilmStock negative_stock(
        stock_name);

    FilmDyeModel dye_model;

    passed &= test::check(
        initialize_dye_model(
            stock,
            settings,
            negative_stock,
            dye_model),
        std::string("negative dye model initializes for ")
            + stock);

    if (!passed) {
        return false;
    }

    const std::filesystem::path csv_path =
        output_directory
        / (stock + ".csv");

    std::ofstream csv(
        csv_path);

    passed &= test::check(
        static_cast<bool>(csv),
        std::string("dye-decomposition CSV opens for ")
            + stock);

    if (!csv) {
        return false;
    }

    csv
        << "stock,family,stop,"
        << "cal_r,cal_g,cal_b,"
        << "cyan_increment,magenta_increment,yellow_increment,"
        << "minimum_area,cyan_area,magenta_area,yellow_area,total_area,"
        << "cyan_fraction,magenta_fraction,yellow_fraction,"
        << "minimum_450,cyan_450,magenta_450,yellow_450,total_450,"
        << "minimum_550,cyan_550,magenta_550,yellow_550,total_550,"
        << "minimum_650,cyan_650,magenta_650,yellow_650,total_650,"
        << "synthesis_max_abs_error\n";

    csv
        << std::setprecision(10);

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

    const std::array<float, 4> stops = {{
        0.0f,
        2.0f,
        4.0f,
        6.0f
    }};

    std::cout
        << "\nNegative dye decomposition: "
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

        std::cout
            << "  "
            << family.name
            << "\n";

        for (float stop : stops) {
            const FilmExposure exposure =
                scale_exposure(
                    base_exposure,
                    std::exp2(stop));

            const FilmPipeline::Result production =
                pipeline.process_negative_exposure(
                    exposure);

            passed &= test::check(
                production.valid,
                std::string("production negative stage is valid for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            if (!production.valid) {
                continue;
            }

            DyeDecomposition decomposition;

            const bool decomposition_ok =
                decompose(
                    dye_model,
                    production.calibrated_negative_density,
                    decomposition);

            passed &= test::check(
                decomposition_ok,
                std::string("dye decomposition succeeds for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            if (!decomposition_ok) {
                continue;
            }

            const SampledCurve synthesized =
                dye_model.synthesize_density(
                    production.calibrated_negative_density);

            const double synthesis_error =
                maximum_absolute_difference(
                    decomposition.reconstructed,
                    synthesized);

            passed &= test::check(
                std::isfinite(synthesis_error)
                && synthesis_error < 1e-6,
                std::string("decomposition reproduces synthesize_density for ")
                    + stock
                    + " / "
                    + family.name
                    + " / stop "
                    + std::to_string(stop));

            const double minimum_area =
                curve_area(
                    decomposition.minimum);

            const double cyan_area =
                curve_area(
                    decomposition.cyan);

            const double magenta_area =
                curve_area(
                    decomposition.magenta);

            const double yellow_area =
                curve_area(
                    decomposition.yellow);

            const double total_dye_area =
                cyan_area
                + magenta_area
                + yellow_area;

            const double total_area =
                minimum_area
                + total_dye_area;

            const double cyan_fraction =
                total_dye_area > 1e-20
                    ? cyan_area / total_dye_area
                    : 0.0;

            const double magenta_fraction =
                total_dye_area > 1e-20
                    ? magenta_area / total_dye_area
                    : 0.0;

            const double yellow_fraction =
                total_dye_area > 1e-20
                    ? yellow_area / total_dye_area
                    : 0.0;

            csv
                << stock << ','
                << family.name << ','
                << stop << ','
                << production.calibrated_negative_density.red << ','
                << production.calibrated_negative_density.green << ','
                << production.calibrated_negative_density.blue << ','
                << decomposition.cyan_increment << ','
                << decomposition.magenta_increment << ','
                << decomposition.yellow_increment << ','
                << minimum_area << ','
                << cyan_area << ','
                << magenta_area << ','
                << yellow_area << ','
                << total_area << ','
                << cyan_fraction << ','
                << magenta_fraction << ','
                << yellow_fraction << ','
                << curve_sample(decomposition.minimum, 450.0f) << ','
                << curve_sample(decomposition.cyan, 450.0f) << ','
                << curve_sample(decomposition.magenta, 450.0f) << ','
                << curve_sample(decomposition.yellow, 450.0f) << ','
                << curve_sample(decomposition.reconstructed, 450.0f) << ','
                << curve_sample(decomposition.minimum, 550.0f) << ','
                << curve_sample(decomposition.cyan, 550.0f) << ','
                << curve_sample(decomposition.magenta, 550.0f) << ','
                << curve_sample(decomposition.yellow, 550.0f) << ','
                << curve_sample(decomposition.reconstructed, 550.0f) << ','
                << curve_sample(decomposition.minimum, 650.0f) << ','
                << curve_sample(decomposition.cyan, 650.0f) << ','
                << curve_sample(decomposition.magenta, 650.0f) << ','
                << curve_sample(decomposition.yellow, 650.0f) << ','
                << curve_sample(decomposition.reconstructed, 650.0f) << ','
                << synthesis_error
                << '\n';

            std::cout
                << "    stop="
                << std::setw(4)
                << stop
                << " coord=("
                << production.calibrated_negative_density.red
                << ", "
                << production.calibrated_negative_density.green
                << ", "
                << production.calibrated_negative_density.blue
                << ") increments C/M/Y=("
                << decomposition.cyan_increment
                << ", "
                << decomposition.magenta_increment
                << ", "
                << decomposition.yellow_increment
                << ") fractions C/M/Y=("
                << cyan_fraction
                << ", "
                << magenta_fraction
                << ", "
                << yellow_fraction
                << ") D450 C/M/Y=("
                << curve_sample(decomposition.cyan, 450.0f)
                << ", "
                << curve_sample(decomposition.magenta, 450.0f)
                << ", "
                << curve_sample(decomposition.yellow, 450.0f)
                << ") D550=("
                << curve_sample(decomposition.cyan, 550.0f)
                << ", "
                << curve_sample(decomposition.magenta, 550.0f)
                << ", "
                << curve_sample(decomposition.yellow, 550.0f)
                << ") D650=("
                << curve_sample(decomposition.cyan, 650.0f)
                << ", "
                << curve_sample(decomposition.magenta, 650.0f)
                << ", "
                << curve_sample(decomposition.yellow, 650.0f)
                << ")"
                << '\n';
        }
    }

    const FilmDyeModel::Diagnostics& diagnostics =
        dye_model.diagnostics();

    std::cout
        << "  basis peaks C/M/Y=("
        << diagnostics.cyan_peak_wavelength_nm
        << ", "
        << diagnostics.magenta_peak_wavelength_nm
        << ", "
        << diagnostics.yellow_peak_wavelength_nm
        << ")"
        << " neutral integrated fractions=("
        << diagnostics.cyan_integrated_fraction
        << ", "
        << diagnostics.magenta_integrated_fraction
        << ", "
        << diagnostics.yellow_integrated_fraction
        << ")"
        << "\n"
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
        / "test_negative_dye_decomposition";

    std::error_code error;

    fs::create_directories(
        output_directory,
        error);

    bool passed = true;

    passed &= test::check(
        !error,
        "negative dye decomposition output directory exists");

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
            "negative dye decomposition");
}
