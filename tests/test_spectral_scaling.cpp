// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "spectralreconstructor.h"

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

struct ColorCase
{
    const char* name;
    std::array<float, 3> chroma;
};

struct Metrics
{
    double spectrum_nrmse = 0.0;
    double spectrum_max_rel = 0.0;
    double forward_rgb_nrmse = 0.0;
};

bool
finite_rgb(
    const std::array<float, 3>& rgb)
{
    return
        std::isfinite(rgb[0])
        && std::isfinite(rgb[1])
        && std::isfinite(rgb[2]);
}

double
rgb_nrmse(
    const std::array<float, 3>& actual,
    const std::array<float, 3>& reference)
{
    double numerator = 0.0;
    double denominator = 0.0;

    for (int channel = 0; channel < 3; ++channel) {
        const double difference =
            static_cast<double>(actual[channel])
            - static_cast<double>(reference[channel]);

        numerator += difference * difference;
        denominator +=
            static_cast<double>(reference[channel])
            * static_cast<double>(reference[channel]);
    }

    return
        std::sqrt(
            numerator
            / std::max(
                denominator,
                1e-20));
}

Metrics
compare_spectra(
    const SpectralReconstructor& reconstructor,
    const SpectralReconstructor::Spectrum& actual,
    const SpectralReconstructor::Spectrum& reference)
{
    Metrics metrics;

    double numerator = 0.0;
    double denominator = 0.0;
    double maximum_difference = 0.0;
    double maximum_reference = 0.0;

    for (float wavelength = 360.0f;
         wavelength <= 830.0f + 0.001f;
         wavelength += 5.0f) {

        const double actual_value =
            reconstructor.evaluate(
                actual,
                wavelength);

        const double reference_value =
            reconstructor.evaluate(
                reference,
                wavelength);

        const double difference =
            actual_value
            - reference_value;

        numerator +=
            difference * difference;

        denominator +=
            reference_value
            * reference_value;

        maximum_difference =
            std::max(
                maximum_difference,
                std::abs(difference));

        maximum_reference =
            std::max(
                maximum_reference,
                std::abs(reference_value));
    }

    metrics.spectrum_nrmse =
        std::sqrt(
            numerator
            / std::max(
                denominator,
                1e-20));

    metrics.spectrum_max_rel =
        maximum_difference
        / std::max(
            maximum_reference,
            1e-10);

    const auto actual_rgb =
        reconstructor.forward_rgb(
            actual);

    const auto reference_rgb =
        reconstructor.forward_rgb(
            reference);

    metrics.forward_rgb_nrmse =
        rgb_nrmse(
            actual_rgb,
            reference_rgb);

    return metrics;
}

void
print_result(
    const char* label,
    bool pass)
{
    std::cout
        << (pass ? "[PASS] " : "[FAIL] ")
        << label
        << '\n';
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

    SpectralReconstructor reconstructor(
        rgb2spec_file.string());

    const bool loaded =
        reconstructor.valid()
        && reconstructor.has_forward_model();

    print_result(
        "ACES2065-1 spectral reconstructor loads with forward model",
        loaded);

    if (!loaded) {
        return 1;
    }

    const std::vector<ColorCase> colors = {
        {
            "neutral",
            {{1.0f, 1.0f, 1.0f}}
        },
        {
            "warm_red",
            {{1.0f, 0.35f, 0.20f}}
        },
        {
            "red",
            {{1.0f, 0.10f, 0.05f}}
        },
        {
            "deep_red",
            {{1.0f, 0.02f, 0.01f}}
        },
        {
            "yellow",
            {{1.0f, 1.0f, 0.05f}}
        },
        {
            "green",
            {{0.05f, 1.0f, 0.05f}}
        },
        {
            "cyan",
            {{0.05f, 1.0f, 1.0f}}
        },
        {
            "blue",
            {{0.05f, 0.10f, 1.0f}}
        },
        {
            "magenta",
            {{1.0f, 0.05f, 1.0f}}
        }
    };

    const fs::path output_directory =
        fs::path("tests")
        / "output"
        / "test_spectral_scaling";

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
        / "spectral_scaling.csv";

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
        << "color,stop,scale,"
        << "input_r,input_g,input_b,"
        << "direct_scale,reference_scale,"
        << "direct_forward_r,direct_forward_g,direct_forward_b,"
        << "reference_forward_r,reference_forward_g,reference_forward_b,"
        << "spectrum_nrmse,spectrum_max_rel,forward_rgb_nrmse\n";

    std::cout
        << std::setprecision(8);

    bool structural_pass = true;

    for (const ColorCase& color : colors) {
        const auto unit_spectrum =
            reconstructor.reconstruct(
                color.chroma,
                SpectralReconstructor::Method::Optimized);

        const auto unit_forward =
            reconstructor.forward_rgb(
                unit_spectrum);

        if (!finite_rgb(unit_forward)) {
            std::cerr
                << "[FAIL] non-finite unit forward RGB for "
                << color.name
                << '\n';

            structural_pass = false;
            continue;
        }

        double worst_spectrum_nrmse = 0.0;
        double worst_spectrum_max_rel = 0.0;
        double worst_forward_rgb_nrmse = 0.0;
        int worst_stop = 0;

        double worst_below_one_nrmse = 0.0;
        double worst_above_one_nrmse = 0.0;

        for (int stop = -8; stop <= 8; ++stop) {
            // Use 18% scene-referred middle gray as the zero-stop intensity.
            // Chroma remains fixed while only scalar scene exposure changes.
            const float scale =
                0.18f
                * std::exp2(
                    static_cast<float>(stop));

            const std::array<float, 3> input = {{
                color.chroma[0] * scale,
                color.chroma[1] * scale,
                color.chroma[2] * scale
            }};

            const auto direct =
                reconstructor.reconstruct(
                    input,
                    SpectralReconstructor::Method::Optimized);

            // Reference semantics for a scene-linear signal:
            // reconstruct chromaticity once, then scale spectral power only.
            auto reference =
                unit_spectrum;

            reference.scale *=
                scale;

            const Metrics metrics =
                compare_spectra(
                    reconstructor,
                    direct,
                    reference);

            const auto direct_forward =
                reconstructor.forward_rgb(
                    direct);

            const auto reference_forward =
                reconstructor.forward_rgb(
                    reference);

            if (!finite_rgb(direct_forward)
                || !finite_rgb(reference_forward)
                || !std::isfinite(metrics.spectrum_nrmse)
                || !std::isfinite(metrics.spectrum_max_rel)
                || !std::isfinite(metrics.forward_rgb_nrmse)) {

                std::cerr
                    << "[FAIL] non-finite scaling result for "
                    << color.name
                    << " at "
                    << stop
                    << " stops\n";

                structural_pass = false;
                continue;
            }

            if (metrics.spectrum_nrmse > worst_spectrum_nrmse) {
                worst_spectrum_nrmse =
                    metrics.spectrum_nrmse;

                worst_stop =
                    stop;
            }

            worst_spectrum_max_rel =
                std::max(
                    worst_spectrum_max_rel,
                    metrics.spectrum_max_rel);

            worst_forward_rgb_nrmse =
                std::max(
                    worst_forward_rgb_nrmse,
                    metrics.forward_rgb_nrmse);

            if (scale < 1.0f) {
                worst_below_one_nrmse =
                    std::max(
                        worst_below_one_nrmse,
                        metrics.spectrum_nrmse);
            }
            else {
                worst_above_one_nrmse =
                    std::max(
                        worst_above_one_nrmse,
                        metrics.spectrum_nrmse);
            }

            csv
                << color.name << ','
                << stop << ','
                << scale << ','
                << input[0] << ','
                << input[1] << ','
                << input[2] << ','
                << direct.scale << ','
                << reference.scale << ','
                << direct_forward[0] << ','
                << direct_forward[1] << ','
                << direct_forward[2] << ','
                << reference_forward[0] << ','
                << reference_forward[1] << ','
                << reference_forward[2] << ','
                << metrics.spectrum_nrmse << ','
                << metrics.spectrum_max_rel << ','
                << metrics.forward_rgb_nrmse
                << '\n';
        }

        std::cout
            << std::setw(10)
            << color.name
            << "  spectrum NRMSE worst="
            << worst_spectrum_nrmse
            << " @ "
            << std::showpos
            << worst_stop
            << std::noshowpos
            << " stops"
            << "  below1="
            << worst_below_one_nrmse
            << "  above1="
            << worst_above_one_nrmse
            << "  max_rel="
            << worst_spectrum_max_rel
            << "  forwardRGB NRMSE="
            << worst_forward_rgb_nrmse
            << '\n';
    }

    std::cout
        << "CSV: "
        << csv_path
        << '\n';

    print_result(
        "spectral scaling diagnostics are finite",
        structural_pass);

    // Diagnostic test for now: do not make the current known non-homogeneous
    // reconstruction fail CI. Once the reconstruction semantics are fixed,
    // tighten this test to assert a small NRMSE across all chromaticities.
    return structural_pass
        ? 0
        : 1;
}
