// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "spectralilluminant.h"
#include "spectralreconstructor.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SensitivityRow
{
    double wavelength = 0.0;
    double yellow = -9999.0;
    double magenta = -9999.0;
    double cyan = -9999.0;
};

struct Family
{
    const char* name;
    std::array<float, 3> ap0;
};

struct BandStats
{
    double e380_450 = 0.0;
    double e450_500 = 0.0;
    double e500_600 = 0.0;
    double e600_700 = 0.0;
    double total = 0.0;
};

bool parse_double(const std::string& text,
                  double& value)
{
    if (text.empty())
        return false;

    const char* begin = text.c_str();
    char* end = nullptr;

    errno = 0;

    const double parsed =
        std::strtod(
            begin,
            &end);

    if (begin == end
        || errno == ERANGE
        || !std::isfinite(parsed))
        return false;

    while (*end != '\0') {
        if (*end != ' '
            && *end != '\t'
            && *end != '\r'
            && *end != '\n')
            return false;

        ++end;
    }

    value = parsed;
    return true;
}

bool read_sensitivity(const std::filesystem::path& filename,
                      std::vector<SensitivityRow>& rows)
{
    rows.clear();

    std::ifstream file(filename);
    if (!file)
        return false;

    std::string line;

    // Header.
    if (!std::getline(file, line))
        return false;

    std::size_t line_number = 1;

    while (std::getline(file, line)) {
        ++line_number;

        if (line.empty())
            continue;

        std::stringstream stream(line);
        std::array<std::string, 4> field;

        for (std::size_t i = 0;
             i < field.size();
             ++i) {

            if (!std::getline(
                    stream,
                    field[i],
                    ','))
                field[i].clear();
        }

        double wavelength = 0.0;

        // Be deliberately tolerant of comments, repeated headers, metadata,
        // BOMs, or malformed non-data rows. A diagnostic test should report
        // the spectral result rather than abort inside std::stod().
        if (!parse_double(
                field[0],
                wavelength))
            continue;

        SensitivityRow row;
        row.wavelength = wavelength;

        double value = 0.0;

        if (parse_double(
                field[1],
                value))
            row.yellow = value;

        if (parse_double(
                field[2],
                value))
            row.magenta = value;

        if (parse_double(
                field[3],
                value))
            row.cyan = value;

        rows.push_back(row);
    }

    std::sort(
        rows.begin(),
        rows.end(),
        [](const SensitivityRow& a,
           const SensitivityRow& b) {
            return a.wavelength < b.wavelength;
        });

    return !rows.empty();
}

double sample_sensitivity(const std::vector<SensitivityRow>& rows,
                          double wavelength,
                          int channel)
{
    auto value = [channel](const SensitivityRow& row) {
        return channel == 0 ? row.yellow : channel == 1 ? row.magenta : row.cyan;
    };

    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (wavelength > rows[i].wavelength)
            continue;

        const double y0 = value(rows[i - 1]);
        const double y1 = value(rows[i]);
        if (y0 < -9000.0 || y1 < -9000.0)
            return 0.0;

        const double x0 = rows[i - 1].wavelength;
        const double x1 = rows[i].wavelength;
        const double t = (wavelength - x0) / (x1 - x0);
        return std::pow(10.0, y0 + t * (y1 - y0));
    }

    return 0.0;
}

void add_band(BandStats& stats, double wavelength, double value)
{
    stats.total += value;
    if (wavelength < 450.0) stats.e380_450 += value;
    else if (wavelength < 500.0) stats.e450_500 += value;
    else if (wavelength < 600.0) stats.e500_600 += value;
    else if (wavelength <= 700.0) stats.e600_700 += value;
}

double fraction(double value, double total)
{
    return total > 1e-30 ? value / total : 0.0;
}

double nrmse(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    double se = 0.0;
    double ref = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        se += d * d;
        ref += static_cast<double>(b[i]) * b[i];
    }
    return std::sqrt(se / std::max(ref, 1e-30));
}

const char* method_name(SpectralReconstructor::Method method)
{
    return method == SpectralReconstructor::Method::Lookup ? "lookup" : "optimized";
}

} // namespace

int main()
{
    namespace fs = std::filesystem;

    const fs::path resources(FILMVIZ_TEST_RESOURCE_DIR);
    const fs::path spec = resources / "spectral/reconstruction/ACES2065_1.spec";
    const fs::path sensitivity_file = resources / "profiles/kodak_50d/kodak_50d_spectral_sensitivity_curves.csv";

    SpectralReconstructor reconstructor(spec.string());
    SpectralIlluminant d60(SpectralIlluminant::Standard::D60);
    std::vector<SensitivityRow> sensitivity;

    bool passed = true;
    passed &= test::check(reconstructor.valid(), "ACES2065-1 rgb2spec table loads");
    passed &= test::check(reconstructor.has_forward_model(), "rgb2spec forward model is available");
    passed &= test::check(d60.valid(), "D60 illuminant is available");
    passed &= test::check(read_sensitivity(sensitivity_file, sensitivity), "50D sensitivity CSV loads");
    if (!passed)
        return test::finish(false, "spectral metamer diagnostic");

    const fs::path output_dir = fs::path("tests") / "output" / "test_spectral_metamer_diagnostic";
    std::error_code error;
    fs::create_directories(output_dir, error);
    passed &= test::check(!error, "spectral metamer output directory exists");

    std::ofstream summary(output_dir / "summary.csv");
    std::ofstream spectra(output_dir / "spectra.csv");
    passed &= test::check(static_cast<bool>(summary) && static_cast<bool>(spectra), "diagnostic CSV files open");
    if (!passed)
        return test::finish(false, "spectral metamer diagnostic");

    summary << "family,method,requested_r,requested_g,requested_b,forward_r,forward_g,forward_b,forward_nrmse,"
               "spectrum_380_450,spectrum_450_500,spectrum_500_600,spectrum_600_700,short_long_ratio,"
               "film_yellow_h,film_magenta_h,film_cyan_h,yellow_over_cyan\n";
    spectra << "family,method,wavelength_nm,spectral_factor,d60_illuminated,yellow_contribution,magenta_contribution,cyan_contribution\n";
    summary << std::setprecision(10);
    spectra << std::setprecision(10);

    const std::array<Family, 5> families = {{
        {"neutral",    {{0.18f, 0.18f, 0.18f}}},
        {"orange_red", {{0.18f, 0.080f, 0.010f}}},
        {"warm_red",   {{0.18f, 0.045f, 0.018f}}},
        {"red",        {{0.18f, 0.018f, 0.009f}}},
        {"deep_red",   {{0.18f, 0.0036f, 0.0018f}}}
    }};

    const std::array<SpectralReconstructor::Method, 2> methods = {{
        SpectralReconstructor::Method::Lookup,
        SpectralReconstructor::Method::Optimized
    }};

    const std::array<float, 11> report_wavelengths = {{
        400.0f, 420.0f, 440.0f, 460.0f, 500.0f, 550.0f,
        600.0f, 620.0f, 640.0f, 660.0f, 680.0f
    }};

    for (const Family& family : families) {
        std::cout << "\n" << family.name << " requested AP0=("
                  << family.ap0[0] << ", " << family.ap0[1] << ", " << family.ap0[2] << ")\n";

        for (const auto method : methods) {
            const auto spectrum = reconstructor.reconstruct(family.ap0, method);
            const auto forward = reconstructor.forward_rgb(spectrum);
            const auto sampled = reconstructor.sample(spectrum, 380.0f, 700.0f, 5.0f);
            const auto illuminated = d60.illuminate(sampled);

            BandStats spectral_bands;
            std::array<double, 3> film_h = {{0.0, 0.0, 0.0}};

            for (std::size_t i = 0; i < sampled.x.size(); ++i) {
                const double wavelength = sampled.x[i];
                const double factor = sampled.y[i];
                const double power = illuminated.y[i];
                add_band(spectral_bands, wavelength, factor * 5.0);

                std::array<double, 3> contribution = {{0.0, 0.0, 0.0}};
                for (int channel = 0; channel < 3; ++channel) {
                    contribution[channel] = power * sample_sensitivity(sensitivity, wavelength, channel) * 5.0;
                    film_h[channel] += contribution[channel];
                }

                spectra << family.name << ',' << method_name(method) << ',' << wavelength << ','
                        << factor << ',' << power << ','
                        << contribution[0] << ',' << contribution[1] << ',' << contribution[2] << '\n';
            }

            std::cout << "  " << std::setw(9) << method_name(method)
                      << " forward=(" << forward[0] << ", " << forward[1] << ", " << forward[2] << ")"
                      << " NRMSE=" << nrmse(forward, family.ap0)
                      << " spectrum bands=("
                      << fraction(spectral_bands.e380_450, spectral_bands.total) << ", "
                      << fraction(spectral_bands.e450_500, spectral_bands.total) << ", "
                      << fraction(spectral_bands.e500_600, spectral_bands.total) << ", "
                      << fraction(spectral_bands.e600_700, spectral_bands.total) << ")"
                      << " short/long=" << spectral_bands.e380_450 / std::max(spectral_bands.e600_700, 1e-30)
                      << " film H[Y/M/C]=(" << film_h[0] << ", " << film_h[1] << ", " << film_h[2] << ")\n";

            std::cout << "            S(lambda):";
            for (float wavelength : report_wavelengths)
                std::cout << ' ' << static_cast<int>(wavelength) << '=' << reconstructor.evaluate(spectrum, wavelength);
            std::cout << '\n';

            summary << family.name << ',' << method_name(method) << ','
                    << family.ap0[0] << ',' << family.ap0[1] << ',' << family.ap0[2] << ','
                    << forward[0] << ',' << forward[1] << ',' << forward[2] << ','
                    << nrmse(forward, family.ap0) << ','
                    << fraction(spectral_bands.e380_450, spectral_bands.total) << ','
                    << fraction(spectral_bands.e450_500, spectral_bands.total) << ','
                    << fraction(spectral_bands.e500_600, spectral_bands.total) << ','
                    << fraction(spectral_bands.e600_700, spectral_bands.total) << ','
                    << spectral_bands.e380_450 / std::max(spectral_bands.e600_700, 1e-30) << ','
                    << film_h[0] << ',' << film_h[1] << ',' << film_h[2] << ','
                    << film_h[0] / std::max(film_h[2], 1e-30) << '\n';
        }
    }

    std::cout << "\nCSV: " << output_dir / "summary.csv" << '\n'
              << "CSV: " << output_dir / "spectra.csv" << '\n';

    return test::finish(passed, "spectral metamer diagnostic");
}
