// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "spectralilluminant.h"
#include "spectralreconstructor.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CsvTable
{
    std::vector<std::string> header;
    std::vector<std::map<std::string, double>> rows;
};

struct BandExposure
{
    double b380_450 = 0.0;
    double b450_500 = 0.0;
    double b500_600 = 0.0;
    double b600_700 = 0.0;
    double total = 0.0;
    double peak_value = 0.0;
    double peak_wavelength = 0.0;
};

struct RecordResult
{
    const char* name = "";
    const char* csv_column = "";
    BandExposure exposure;
};

bool
read_csv(
    const std::filesystem::path& filename,
    CsvTable& table)
{
    table = CsvTable();

    std::ifstream file(
        filename);

    if (!file) {
        return false;
    }

    std::string line;

    if (!std::getline(file, line)) {
        return false;
    }

    {
        std::stringstream stream(line);
        std::string field;

        while (std::getline(stream, field, ',')) {
            table.header.push_back(field);
        }
    }

    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        std::stringstream stream(line);
        std::string field;
        std::size_t index = 0;
        std::map<std::string, double> row;

        while (std::getline(stream, field, ',')) {
            if (index >= table.header.size()) {
                break;
            }

            if (!field.empty()) {
                try {
                    row[table.header[index]] =
                        std::stod(field);
                }
                catch (...) {
                }
            }

            ++index;
        }

        table.rows.push_back(
            row);
    }

    return
        !table.header.empty()
        && !table.rows.empty();
}

bool
has_column(
    const CsvTable& table,
    const std::string& name)
{
    return
        std::find(
            table.header.begin(),
            table.header.end(),
            name)
        != table.header.end();
}

double
sample_column(
    const CsvTable& table,
    const std::string& x_name,
    const std::string& y_name,
    double x,
    double missing)
{
    struct Point
    {
        double x;
        double y;
    };

    std::vector<Point> points;

    for (const auto& row : table.rows) {
        const auto ix =
            row.find(x_name);
        const auto iy =
            row.find(y_name);

        if (ix != row.end()
            && iy != row.end()) {

            points.push_back(
                {ix->second, iy->second});
        }
    }

    if (points.empty()
        || x < points.front().x
        || x > points.back().x) {
        return missing;
    }

    for (std::size_t i = 1;
         i < points.size();
         ++i) {

        if (x <= points[i].x) {
            const double x0 =
                points[i - 1].x;
            const double x1 =
                points[i].x;
            const double y0 =
                points[i - 1].y;
            const double y1 =
                points[i].y;

            if (std::abs(x1 - x0) <= 1e-12) {
                return y0;
            }

            const double t =
                (x - x0)
                / (x1 - x0);

            return
                y0
                + t * (y1 - y0);
        }
    }

    return
        points.back().y;
}

double
column_max(
    const CsvTable& table,
    const std::string& name,
    double& peak_x)
{
    double result =
        -std::numeric_limits<double>::infinity();

    peak_x = 0.0;

    for (const auto& row : table.rows) {
        const auto ix =
            row.find("wavelength_nm");
        const auto iy =
            row.find(name);

        if (ix != row.end()
            && iy != row.end()
            && iy->second > result) {

            result =
                iy->second;
            peak_x =
                ix->second;
        }
    }

    return result;
}

void
accumulate_band(
    BandExposure& result,
    double wavelength,
    double value)
{
    result.total += value;

    if (wavelength < 450.0) {
        result.b380_450 += value;
    }
    else if (wavelength < 500.0) {
        result.b450_500 += value;
    }
    else if (wavelength < 600.0) {
        result.b500_600 += value;
    }
    else {
        result.b600_700 += value;
    }

    if (value > result.peak_value) {
        result.peak_value =
            value;
        result.peak_wavelength =
            wavelength;
    }
}

double
fraction(
    double value,
    double total)
{
    return
        total > 1e-30
            ? value / total
            : 0.0;
}

double
relative_error(
    double actual,
    double expected)
{
    return
        std::abs(actual - expected)
        / std::max(
            std::abs(expected),
            1e-20);
}

bool
validate_50d_dye_resource(
    const std::filesystem::path& dye_file)
{
    CsvTable dye;

    bool passed = true;

    passed &= test::check(
        read_csv(
            dye_file,
            dye),
        "50D spectral dye density CSV loads");

    if (!passed) {
        return false;
    }

    const std::array<const char*, 5> plotted_columns = {{
        "minimum_density",
        "midscale_neutral_density",
        "cyan_peak_normalized",
        "magenta_peak_normalized",
        "yellow_peak_normalized"
    }};

    for (const char* column : plotted_columns) {
        passed &= test::check(
            has_column(
                dye,
                column),
            std::string("50D dye CSV has plotted column ")
                + column);
    }

    // These are scalar file metadata. Their presence is valid, but they must
    // never be treated as wavelength-varying curves in the Python profile UI.
    const bool has_source_spacing =
        has_column(
            dye,
            "source_spacing_nm");

    const bool has_working_spacing =
        has_column(
            dye,
            "working_spacing_nm");

    passed &= test::check(
        has_source_spacing
        || has_working_spacing,
        "50D dye CSV carries spacing metadata separately from dye curves");

    double cyan_peak_nm = 0.0;
    double magenta_peak_nm = 0.0;
    double yellow_peak_nm = 0.0;

    const double cyan_peak =
        column_max(
            dye,
            "cyan_peak_normalized",
            cyan_peak_nm);

    const double magenta_peak =
        column_max(
            dye,
            "magenta_peak_normalized",
            magenta_peak_nm);

    const double yellow_peak =
        column_max(
            dye,
            "yellow_peak_normalized",
            yellow_peak_nm);

    passed &= test::check(
        std::abs(cyan_peak - 1.0) < 0.02
        && cyan_peak_nm >= 660.0
        && cyan_peak_nm <= 710.0,
        "50D cyan normalized dye peak is near 1 around the red region");

    passed &= test::check(
        std::abs(magenta_peak - 1.0) < 0.02
        && magenta_peak_nm >= 520.0
        && magenta_peak_nm <= 560.0,
        "50D magenta normalized dye peak is near 1 around green");

    passed &= test::check(
        std::abs(yellow_peak - 1.0) < 0.02
        && yellow_peak_nm >= 425.0
        && yellow_peak_nm <= 460.0,
        "50D yellow normalized dye peak is near 1 around blue");

    std::cout
        << "\n50D dye resource sanity\n"
        << "  normalized peak wavelengths C/M/Y = "
        << cyan_peak_nm
        << " / "
        << magenta_peak_nm
        << " / "
        << yellow_peak_nm
        << " nm\n"
        << "  source_spacing_nm="
        << (
            has_source_spacing
                ? sample_column(
                    dye,
                    "wavelength_nm",
                    "source_spacing_nm",
                    500.0,
                    -1.0)
                : -1.0)
        << " working_spacing_nm="
        << (
            has_working_spacing
                ? sample_column(
                    dye,
                    "wavelength_nm",
                    "working_spacing_nm",
                    500.0,
                    -1.0)
                : -1.0)
        << "\n";

    return passed;
}

std::string
find_forming_column(
    const CsvTable& table,
    const std::string& dye_name)
{
    const std::array<std::string, 4> exact_candidates = {{
        dye_name + "_forming_layer",
        dye_name + "_forming_log",
        dye_name + "_forming",
        dye_name + "_layer"
    }};

    for (const std::string& candidate : exact_candidates) {
        if (has_column(
                table,
                candidate)) {
            return candidate;
        }
    }

    for (const std::string& column : table.header) {
        std::string lowered =
            column;

        std::transform(
            lowered.begin(),
            lowered.end(),
            lowered.begin(),
            [](unsigned char c) {
                return static_cast<char>(
                    std::tolower(c));
            });

        if (lowered.find(dye_name) != std::string::npos
            && lowered.find("forming") != std::string::npos) {
            return column;
        }
    }

    return std::string();
}

bool
run_exposure_origin(
    const std::filesystem::path& sensitivity_file)
{
    CsvTable sensitivity;

    bool passed = true;

    passed &= test::check(
        read_csv(
            sensitivity_file,
            sensitivity),
        "50D spectral sensitivity CSV loads");

    if (!passed) {
        return false;
    }

    const std::string yellow_column =
        find_forming_column(
            sensitivity,
            "yellow");

    const std::string magenta_column =
        find_forming_column(
            sensitivity,
            "magenta");

    const std::string cyan_column =
        find_forming_column(
            sensitivity,
            "cyan");

    passed &= test::check(
        !yellow_column.empty(),
        "50D sensitivity CSV has a yellow-forming column");

    passed &= test::check(
        !magenta_column.empty(),
        "50D sensitivity CSV has a magenta-forming column");

    passed &= test::check(
        !cyan_column.empty(),
        "50D sensitivity CSV has a cyan-forming column");

    std::cout
        << "  sensitivity columns Y/M/C = "
        << yellow_column
        << " / "
        << magenta_column
        << " / "
        << cyan_column
        << "\n";

    FilmPipeline::Settings settings;
    settings.resources_directory =
        FILMVIZ_TEST_RESOURCE_DIR;
    settings.negative_profile =
        "kodak-50d";
    settings.print_profile =
        "none";

    FilmPipeline pipeline;

    passed &= test::check(
        pipeline.initialize(
            settings),
        std::string("50D pipeline initializes: ")
            + pipeline.error());

    if (!passed) {
        return false;
    }

    const std::filesystem::path resources(
        FILMVIZ_TEST_RESOURCE_DIR);

    SpectralReconstructor reconstructor(
        (resources
         / "spectral/reconstruction/ACES2065_1.spec").string());

    SpectralIlluminant d60(
        SpectralIlluminant::Standard::D60);

    passed &= test::check(
        reconstructor.valid()
        && d60.valid(),
        "spectral reconstruction and D60 load");

    if (!passed) {
        return false;
    }

    struct Family
    {
        const char* name;
        std::array<float, 3> ap0;
    };

    const std::array<Family, 2> families = {{
        {
            "warm_red",
            {{0.18f, 0.045f, 0.018f}}
        },
        {
            "red",
            {{0.18f, 0.018f, 0.009f}}
        }
    }};

    const std::filesystem::path output_directory =
        std::filesystem::path("tests")
        / "output"
        / "test_negative_exposure_origin";

    std::error_code error;

    std::filesystem::create_directories(
        output_directory,
        error);

    passed &= test::check(
        !error,
        "negative exposure-origin output directory exists");

    const std::filesystem::path csv_path =
        output_directory
        / "kodak-50d.csv";

    std::ofstream csv(
        csv_path);

    passed &= test::check(
        static_cast<bool>(csv),
        "negative exposure-origin CSV opens");

    if (!csv) {
        return false;
    }

    csv
        << "family,record,total,"
        << "fraction_380_450,fraction_450_500,"
        << "fraction_500_600,fraction_600_700,"
        << "peak_wavelength_nm,peak_contribution,"
        << "pipeline_total,relative_error\n";

    csv
        << std::setprecision(10);

    for (const Family& family : families) {
        const auto spectrum =
            reconstructor.reconstruct(
                family.ap0,
                SpectralReconstructor::Method::Optimized);

        const SampledCurve factor =
            reconstructor.sample(
                spectrum,
                settings.wavelength_min_nm,
                settings.wavelength_max_nm,
                settings.wavelength_step_nm);

        const SampledCurve illuminated =
            d60.illuminate(
                factor);

        FilmExposure pipeline_exposure;

        passed &= test::check(
            pipeline.negative_exposure(
                family.ap0,
                pipeline_exposure),
            std::string("pipeline negative exposure evaluates ")
                + family.name);

        RecordResult yellow;
        yellow.name =
            "yellow_forming";
        yellow.csv_column =
            yellow_column.c_str();

        RecordResult magenta;
        magenta.name =
            "magenta_forming";
        magenta.csv_column =
            magenta_column.c_str();

        RecordResult cyan;
        cyan.name =
            "cyan_forming";
        cyan.csv_column =
            cyan_column.c_str();

        std::array<RecordResult*, 3> records = {{
            &yellow,
            &magenta,
            &cyan
        }};

        for (float wavelength =
                 settings.wavelength_min_nm;
             wavelength <=
                 settings.wavelength_max_nm
                 + 0.001f;
             wavelength +=
                 settings.wavelength_step_nm) {

            const double scene_power =
                illuminated.sample(
                    wavelength,
                    0.0f);

            for (RecordResult* record : records) {
                const double log_s =
                    sample_column(
                        sensitivity,
                        "wavelength_nm",
                        record->csv_column,
                        wavelength,
                        -9999.0);

                const double linear_s =
                    log_s <= -9000.0
                        ? 0.0
                        : std::pow(
                            10.0,
                            log_s);

                const double contribution =
                    scene_power
                    * linear_s
                    * settings.wavelength_step_nm;

                accumulate_band(
                    record->exposure,
                    wavelength,
                    contribution);
            }
        }

        // FilmProcessor mapping:
        // blue exposure  <- yellow-forming layer
        // green exposure <- magenta-forming layer
        // red exposure   <- cyan-forming layer
        const std::array<double, 3> pipeline_totals = {{
            pipeline_exposure.blue,
            pipeline_exposure.green,
            pipeline_exposure.red
        }};

        std::cout
            << "\nNegative exposure origin: "
            << family.name
            << "\n";

        for (std::size_t i = 0;
             i < records.size();
             ++i) {

            const RecordResult& record =
                *records[i];

            const double expected =
                pipeline_totals[i];

            const double rel_error =
                relative_error(
                    record.exposure.total,
                    expected);

            passed &= test::check(
                rel_error < 2e-5,
                std::string("manual sensitivity integration matches pipeline for ")
                    + family.name
                    + " / "
                    + record.name);

            std::cout
                << "  "
                << std::setw(15)
                << record.name
                << " total="
                << record.exposure.total
                << " bands[380-450,450-500,500-600,600-700]=("
                << fraction(
                    record.exposure.b380_450,
                    record.exposure.total)
                << ", "
                << fraction(
                    record.exposure.b450_500,
                    record.exposure.total)
                << ", "
                << fraction(
                    record.exposure.b500_600,
                    record.exposure.total)
                << ", "
                << fraction(
                    record.exposure.b600_700,
                    record.exposure.total)
                << ") peak="
                << record.exposure.peak_wavelength
                << " nm"
                << " rel.err="
                << rel_error
                << "\n";

            csv
                << family.name << ','
                << record.name << ','
                << record.exposure.total << ','
                << fraction(
                    record.exposure.b380_450,
                    record.exposure.total) << ','
                << fraction(
                    record.exposure.b450_500,
                    record.exposure.total) << ','
                << fraction(
                    record.exposure.b500_600,
                    record.exposure.total) << ','
                << fraction(
                    record.exposure.b600_700,
                    record.exposure.total) << ','
                << record.exposure.peak_wavelength << ','
                << record.exposure.peak_value << ','
                << expected << ','
                << rel_error
                << '\n';
        }
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

    const fs::path resources(
        FILMVIZ_TEST_RESOURCE_DIR);

    const fs::path profile =
        resources
        / "profiles/kodak_50d";

    bool passed = true;

    passed &=
        validate_50d_dye_resource(
            profile
            / "kodak_50d_spectral_dye_density_curves.csv");

    passed &=
        run_exposure_origin(
            profile
            / "kodak_50d_spectral_sensitivity_curves.csv");

    return
        test::finish(
            passed,
            "negative exposure origin");
}
