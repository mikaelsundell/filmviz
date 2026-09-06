// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "colortransform.h"
#include "test_common.h"

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
    std::array<float, 3> awg3;
};

struct GamutFlags
{
    bool below_zero = false;
    bool above_one = false;
};

GamutFlags
gamut_flags(
    const std::array<float, 3>& rgb)
{
    GamutFlags result;

    for (float value : rgb) {
        result.below_zero |= value < 0.0f;
        result.above_one |= value > 1.0f;
    }

    return result;
}

float
maximum_absolute_error(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b)
{
    return std::max({
        std::abs(a[0] - b[0]),
        std::abs(a[1] - b[1]),
        std::abs(a[2] - b[2])
    });
}

double
gray_axis_chroma(
    const std::array<float, 3>& rgb)
{
    const double mean =
        (static_cast<double>(rgb[0])
         + static_cast<double>(rgb[1])
         + static_cast<double>(rgb[2]))
        / 3.0;

    const double dr = static_cast<double>(rgb[0]) - mean;
    const double dg = static_cast<double>(rgb[1]) - mean;
    const double db = static_cast<double>(rgb[2]) - mean;

    return std::sqrt(
        dr * dr
        + dg * dg
        + db * db);
}

double
mean_value(
    const std::array<float, 3>& rgb)
{
    return
        (static_cast<double>(rgb[0])
         + static_cast<double>(rgb[1])
         + static_cast<double>(rgb[2]))
        / 3.0;
}

void
write_rgb(
    std::ofstream& csv,
    const std::array<float, 3>& rgb)
{
    csv
        << rgb[0] << ','
        << rgb[1] << ','
        << rgb[2];
}

} // namespace

int
main()
{
    namespace fs = std::filesystem;

    bool passed = true;

    const ColorTransform awg_to_ap0(
        ColorTransform::ColorSpace::AWG3,
        ColorTransform::TransferFunction::Linear,
        ColorTransform::ColorSpace::ACES2065_1,
        ColorTransform::TransferFunction::Linear);

    const ColorTransform ap0_to_awg(
        ColorTransform::ColorSpace::ACES2065_1,
        ColorTransform::TransferFunction::Linear,
        ColorTransform::ColorSpace::AWG3,
        ColorTransform::TransferFunction::Linear);

    const ColorTransform awg_to_rec709_linear(
        ColorTransform::ColorSpace::AWG3,
        ColorTransform::TransferFunction::Linear,
        ColorTransform::ColorSpace::Rec709,
        ColorTransform::TransferFunction::Linear);

    const ColorTransform ap0_to_rec709_linear(
        ColorTransform::ColorSpace::ACES2065_1,
        ColorTransform::TransferFunction::Linear,
        ColorTransform::ColorSpace::Rec709,
        ColorTransform::TransferFunction::Linear);

    const ColorTransform ap0_to_rec709_gamma24(
        ColorTransform::ColorSpace::ACES2065_1,
        ColorTransform::TransferFunction::Linear,
        ColorTransform::ColorSpace::Rec709,
        ColorTransform::TransferFunction::Gamma24);

    const std::vector<ColorCase> colors = {
        {"neutral",     {{0.50f, 0.50f, 0.50f}}},
        {"red",         {{1.00f, 0.00f, 0.00f}}},
        {"warm_red",    {{1.00f, 0.20f, 0.05f}}},
        {"orange_red",  {{1.00f, 0.35f, 0.05f}}},
        {"yellow",      {{1.00f, 1.00f, 0.00f}}},
        {"green",       {{0.00f, 1.00f, 0.00f}}},
        {"cyan",        {{0.00f, 1.00f, 1.00f}}},
        {"blue",        {{0.00f, 0.00f, 1.00f}}},
        {"magenta",     {{1.00f, 0.00f, 1.00f}}}
    };

    const fs::path output_directory =
        fs::path("tests")
        / "output"
        / "test_gamut_path";

    std::error_code error;

    fs::create_directories(
        output_directory,
        error);

    passed &= test::check(
        !error,
        "gamut-path output directory exists");

    if (error) {
        return 1;
    }

    const fs::path csv_path =
        output_directory
        / "gamut_path.csv";

    std::ofstream csv(
        csv_path);

    passed &= test::check(
        static_cast<bool>(csv),
        "gamut-path CSV opens");

    if (!csv) {
        return 1;
    }

    csv
        << "family,level,"
        << "awg_r,awg_g,awg_b,"
        << "ap0_r,ap0_g,ap0_b,"
        << "awg_roundtrip_r,awg_roundtrip_g,awg_roundtrip_b,"
        << "rec709_linear_direct_r,rec709_linear_direct_g,rec709_linear_direct_b,"
        << "rec709_linear_via_ap0_r,rec709_linear_via_ap0_g,rec709_linear_via_ap0_b,"
        << "rec709_gamma24_r,rec709_gamma24_g,rec709_gamma24_b,"
        << "ap0_below_zero,ap0_above_one,"
        << "rec709_below_zero,rec709_above_one,"
        << "roundtrip_max_abs,"
        << "rec709_direct_vs_via_ap0_max_abs,"
        << "ap0_gray_axis_chroma,"
        << "rec709_linear_gray_axis_chroma,"
        << "rec709_gamma24_gray_axis_chroma,"
        << "ap0_mean,rec709_linear_mean,rec709_gamma24_mean\n";

    csv << std::setprecision(10);

    std::cout << std::setprecision(9);

    double worst_roundtrip = 0.0;
    double worst_direct_vs_via = 0.0;

    std::size_t sample_count = 0;
    std::size_t ap0_oog_count = 0;
    std::size_t rec709_oog_count = 0;

    const std::array<float, 7> levels = {{
        0.05f,
        0.10f,
        0.18f,
        0.35f,
        0.50f,
        0.75f,
        1.00f
    }};

    for (const ColorCase& color : colors) {
        std::cout
            << "\n"
            << color.name
            << "\n";

        for (float level : levels) {
            const std::array<float, 3> awg = {{
                color.awg3[0] * level,
                color.awg3[1] * level,
                color.awg3[2] * level
            }};

            const std::array<float, 3> ap0 =
                awg_to_ap0.transform(
                    awg);

            const std::array<float, 3> awg_roundtrip =
                ap0_to_awg.transform(
                    ap0);

            const std::array<float, 3> rec709_direct =
                awg_to_rec709_linear.transform(
                    awg);

            const std::array<float, 3> rec709_via_ap0 =
                ap0_to_rec709_linear.transform(
                    ap0);

            const std::array<float, 3> rec709_gamma24 =
                ap0_to_rec709_gamma24.transform(
                    ap0);

            const GamutFlags ap0_flags =
                gamut_flags(
                    ap0);

            const GamutFlags rec709_flags =
                gamut_flags(
                    rec709_via_ap0);

            const float roundtrip_error =
                maximum_absolute_error(
                    awg,
                    awg_roundtrip);

            const float direct_vs_via_error =
                maximum_absolute_error(
                    rec709_direct,
                    rec709_via_ap0);

            worst_roundtrip =
                std::max(
                    worst_roundtrip,
                    static_cast<double>(
                        roundtrip_error));

            worst_direct_vs_via =
                std::max(
                    worst_direct_vs_via,
                    static_cast<double>(
                        direct_vs_via_error));

            ++sample_count;

            if (ap0_flags.below_zero
                || ap0_flags.above_one) {
                ++ap0_oog_count;
            }

            if (rec709_flags.below_zero
                || rec709_flags.above_one) {
                ++rec709_oog_count;
            }

            csv
                << color.name << ','
                << level << ',';

            write_rgb(csv, awg);
            csv << ',';
            write_rgb(csv, ap0);
            csv << ',';
            write_rgb(csv, awg_roundtrip);
            csv << ',';
            write_rgb(csv, rec709_direct);
            csv << ',';
            write_rgb(csv, rec709_via_ap0);
            csv << ',';
            write_rgb(csv, rec709_gamma24);

            csv
                << ','
                << (ap0_flags.below_zero ? 1 : 0)
                << ','
                << (ap0_flags.above_one ? 1 : 0)
                << ','
                << (rec709_flags.below_zero ? 1 : 0)
                << ','
                << (rec709_flags.above_one ? 1 : 0)
                << ','
                << roundtrip_error
                << ','
                << direct_vs_via_error
                << ','
                << gray_axis_chroma(ap0)
                << ','
                << gray_axis_chroma(rec709_via_ap0)
                << ','
                << gray_axis_chroma(rec709_gamma24)
                << ','
                << mean_value(ap0)
                << ','
                << mean_value(rec709_via_ap0)
                << ','
                << mean_value(rec709_gamma24)
                << '\n';

            std::cout
                << "  level="
                << std::setw(5)
                << level
                << " AP0=("
                << ap0[0] << ", "
                << ap0[1] << ", "
                << ap0[2] << ")"
                << " AP0_OOG="
                << (
                    ap0_flags.below_zero
                    || ap0_flags.above_one
                    ? "yes"
                    : "no")
                << " Rec709=("
                << rec709_via_ap0[0] << ", "
                << rec709_via_ap0[1] << ", "
                << rec709_via_ap0[2] << ")"
                << " Rec709_OOG="
                << (
                    rec709_flags.below_zero
                    || rec709_flags.above_one
                    ? "yes"
                    : "no")
                << " roundtrip="
                << roundtrip_error
                << '\n';
        }
    }

    passed &= test::check(
        worst_roundtrip < 2e-5,
        "AWG3 -> AP0 -> AWG3 roundtrip preserves values");

    passed &= test::check(
        worst_direct_vs_via < 2e-5,
        "AWG3 -> Rec709 equals AWG3 -> AP0 -> Rec709");

    std::cout
        << "\nSummary\n"
        << "  samples: "
        << sample_count
        << '\n'
        << "  AP0 positive-cube excursions: "
        << ap0_oog_count
        << '\n'
        << "  Rec709 positive-cube excursions: "
        << rec709_oog_count
        << '\n'
        << "  worst AWG/AP0/AWG roundtrip: "
        << worst_roundtrip
        << '\n'
        << "  worst direct-vs-via Rec709: "
        << worst_direct_vs_via
        << '\n'
        << "  CSV: "
        << csv_path
        << '\n';

    return
        test::finish(
            passed,
            "gamut path");
}
