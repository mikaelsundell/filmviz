// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "cieobserver.h"
#include "colorimetry.h"
#include "spectralcolorimetry.h"
#include "spectralilluminant.h"
#include "spectralreconstructor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ColorCase
{
    const char* name;
    std::array<float, 3> chroma;
};

struct Error3
{
    double max_abs = 0.0;
    double nrmse = 0.0;
};

std::array<double, 3>
to_double(
    const std::array<float, 3>& value)
{
    return {{
        static_cast<double>(value[0]),
        static_cast<double>(value[1]),
        static_cast<double>(value[2])
    }};
}

std::array<float, 3>
to_float(
    const std::array<double, 3>& value)
{
    return {{
        static_cast<float>(value[0]),
        static_cast<float>(value[1]),
        static_cast<float>(value[2])
    }};
}

Error3
error3(
    const std::array<float, 3>& actual,
    const std::array<float, 3>& reference)
{
    Error3 result;

    double numerator = 0.0;
    double denominator = 0.0;

    for (int channel = 0; channel < 3; ++channel) {
        const double difference =
            static_cast<double>(actual[channel])
            - static_cast<double>(reference[channel]);

        result.max_abs =
            std::max(
                result.max_abs,
                std::abs(difference));

        numerator +=
            difference * difference;

        denominator +=
            static_cast<double>(reference[channel])
            * static_cast<double>(reference[channel]);
    }

    result.nrmse =
        std::sqrt(
            numerator
            / std::max(
                denominator,
                1e-20));

    return result;
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

void
print_rgb(
    const char* label,
    const std::array<float, 3>& rgb)
{
    std::cout
        << label
        << "=("
        << rgb[0]
        << ", "
        << rgb[1]
        << ", "
        << rgb[2]
        << ")";
}

} // namespace

int
main()
{
    namespace fs = std::filesystem;

    const fs::path resource_root =
        fs::path(
            FILMVIZ_TEST_RESOURCE_DIR);

    const fs::path rgb2spec_file =
        resource_root
        / "spectral"
        / "reconstruction"
        / "ACES2065_1.spec";

    const fs::path observer_file =
        resource_root
        / "colorimetry"
        / "observers"
        / "CIE_xyz_1931_2deg.csv";

    SpectralReconstructor reconstructor(
        rgb2spec_file.string());

    CIEObserver observer(
        observer_file.string());

    const SpectralIlluminant d60(
        SpectralIlluminant::Standard::D60);

    const bool reconstructor_valid =
        reconstructor.valid();

    const bool forward_model_valid =
        reconstructor.has_forward_model();

    const bool observer_valid =
        observer.valid();

    const bool d60_valid =
        d60.valid();

    bool passed =
        reconstructor_valid
        && forward_model_valid
        && observer_valid
        && d60_valid;

    std::cout
        << (reconstructor_valid ? "[PASS] " : "[FAIL] ")
        << "rgb2spec table: "
        << rgb2spec_file
        << '\n';

    std::cout
        << (forward_model_valid ? "[PASS] " : "[FAIL] ")
        << "rgb2spec embedded forward model\n";

    std::cout
        << (observer_valid ? "[PASS] " : "[FAIL] ")
        << "CIE observer: "
        << observer_file
        << '\n';

    std::cout
        << (d60_valid ? "[PASS] " : "[FAIL] ")
        << "D60 illuminant\n";

    if (!passed) {
        return 1;
    }

    const std::vector<ColorCase> colors = {
        {"neutral",  {{1.0f, 1.0f, 1.0f}}},
        {"warm_red", {{1.0f, 0.35f, 0.20f}}},
        {"red",      {{1.0f, 0.10f, 0.05f}}},
        {"deep_red", {{1.0f, 0.02f, 0.01f}}},
        {"orange",   {{1.0f, 0.25f, 0.05f}}},
        {"yellow",   {{1.0f, 1.0f, 0.05f}}},
        {"green",    {{0.05f, 1.0f, 0.05f}}},
        {"cyan",     {{0.05f, 1.0f, 1.0f}}},
        {"blue",     {{0.05f, 0.10f, 1.0f}}},
        {"magenta",  {{1.0f, 0.05f, 1.0f}}}
    };

    const fs::path output_directory =
        fs::path("tests")
        / "output"
        / "test_spectral_reconstruction_accuracy";

    std::error_code directory_error;

    fs::create_directories(
        output_directory,
        directory_error);

    if (directory_error) {
        std::cerr
            << "[FAIL] could not create output directory: "
            << directory_error.message()
            << '\n';

        return 1;
    }

    const fs::path csv_path =
        output_directory
        / "spectral_reconstruction_accuracy.csv";

    std::ofstream csv(
        csv_path);

    if (!csv) {
        std::cerr
            << "[FAIL] could not write "
            << csv_path
            << '\n';

        return 1;
    }

    csv
        << "color,level,"
        << "requested_r,requested_g,requested_b,"
        << "embedded_r,embedded_g,embedded_b,"
        << "independent_r,independent_g,independent_b,"
        << "embedded_max_abs,embedded_nrmse,"
        << "independent_max_abs,independent_nrmse,"
        << "embedded_vs_independent_max_abs,"
        << "embedded_vs_independent_nrmse\n";

    // Two absolute levels are enough to verify both chromaticity and scale.
    // 0.18 is the working middle-gray level; 0.90 probes near the top of the
    // nominal reflectance domain without exceeding one.
    const std::array<float, 2> levels = {{
        0.18f,
        0.90f
    }};

    std::cout
        << std::setprecision(9);

    bool structural_pass = true;

    double worst_embedded_nrmse = 0.0;
    double worst_independent_nrmse = 0.0;
    double worst_cross_nrmse = 0.0;

    for (const ColorCase& color : colors) {
        for (float level : levels) {
            const std::array<float, 3> requested = {{
                color.chroma[0] * level,
                color.chroma[1] * level,
                color.chroma[2] * level
            }};

            const auto spectrum =
                reconstructor.reconstruct(
                    requested,
                    SpectralReconstructor::Method::Optimized);

            const std::array<float, 3> embedded =
                reconstructor.forward_rgb(
                    spectrum);

            const Colorimetry::XYZ independent_xyz =
                SpectralColorimetry::
                    integrate_reconstructed_reflectance_xyz(
                        reconstructor,
                        spectrum,
                        observer,
                        d60);

            const std::array<float, 3> independent =
                to_float(
                    Colorimetry::xyz_d60_to_ap0(
                        independent_xyz));

            const Error3 embedded_error =
                error3(
                    embedded,
                    requested);

            const Error3 independent_error =
                error3(
                    independent,
                    requested);

            const Error3 cross_error =
                error3(
                    independent,
                    embedded);

            worst_embedded_nrmse =
                std::max(
                    worst_embedded_nrmse,
                    embedded_error.nrmse);

            worst_independent_nrmse =
                std::max(
                    worst_independent_nrmse,
                    independent_error.nrmse);

            worst_cross_nrmse =
                std::max(
                    worst_cross_nrmse,
                    cross_error.nrmse);

            if (!finite_rgb(embedded)
                || !finite_rgb(independent)
                || !std::isfinite(embedded_error.nrmse)
                || !std::isfinite(independent_error.nrmse)
                || !std::isfinite(cross_error.nrmse)) {

                structural_pass = false;
            }

            std::cout
                << std::setw(10)
                << color.name
                << "  level="
                << level
                << "  ";

            print_rgb(
                "requested",
                requested);

            std::cout
                << "  ";

            print_rgb(
                "embedded",
                embedded);

            std::cout
                << "  ";

            print_rgb(
                "independent",
                independent);

            std::cout
                << "  err(embed)="
                << embedded_error.nrmse
                << "  err(ind)="
                << independent_error.nrmse
                << "  embed-vs-ind="
                << cross_error.nrmse
                << '\n';

            csv
                << color.name << ','
                << level << ','
                << requested[0] << ','
                << requested[1] << ','
                << requested[2] << ','
                << embedded[0] << ','
                << embedded[1] << ','
                << embedded[2] << ','
                << independent[0] << ','
                << independent[1] << ','
                << independent[2] << ','
                << embedded_error.max_abs << ','
                << embedded_error.nrmse << ','
                << independent_error.max_abs << ','
                << independent_error.nrmse << ','
                << cross_error.max_abs << ','
                << cross_error.nrmse
                << '\n';
        }
    }

    std::cout
        << "\nWorst errors:\n"
        << "  embedded forward vs requested NRMSE: "
        << worst_embedded_nrmse
        << '\n'
        << "  independent D60/CIE/AP0 vs requested NRMSE: "
        << worst_independent_nrmse
        << '\n'
        << "  embedded forward vs independent NRMSE: "
        << worst_cross_nrmse
        << '\n'
        << "CSV: "
        << csv_path
        << '\n';

    std::cout
        << (structural_pass ? "[PASS] " : "[FAIL] ")
        << "spectral reconstruction accuracy diagnostics are finite\n";

    // Diagnostic for now. Do not encode an accuracy threshold until we have
    // inspected the absolute errors for the current ACES2065-1 table.
    return structural_pass
        ? 0
        : 1;
}
