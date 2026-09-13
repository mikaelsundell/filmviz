// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace {

struct CurveExtent
{
    double first_wavelength = std::numeric_limits<double>::infinity();
    double last_wavelength = -std::numeric_limits<double>::infinity();
    double peak_wavelength = 0.0;
    double peak_value = -std::numeric_limits<double>::infinity();
};

bool
near(
    double actual,
    double expected)
{
    return std::abs(actual - expected) < 1e-6;
}

} // namespace

int
main()
{
    const std::filesystem::path sensitivity_file =
        std::filesystem::path(FILMVIZ_TEST_RESOURCE_DIR)
        / "profiles/kodak_50d/kodak_50d_spectral_sensitivity_curves.csv";

    std::ifstream file(sensitivity_file);
    bool passed = test::check(
        static_cast<bool>(file),
        "Kodak 50D sensitivity resource opens");

    std::string line;
    std::getline(file, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    passed &= test::check(
        line == "wavelength_nm,yellow_forming_layer,magenta_forming_layer,cyan_forming_layer",
        "Kodak 50D sensitivity columns retain their layer mapping");

    std::array<CurveExtent, 3> curves;

    while (std::getline(file, line)) {
        std::stringstream stream(line);
        std::array<std::string, 4> fields;

        for (std::string& field : fields) {
            std::getline(stream, field, ',');
            if (!field.empty() && field.back() == '\r') {
                field.pop_back();
            }
        }

        if (fields[0].empty()) {
            continue;
        }

        const double wavelength = std::stod(fields[0]);

        for (std::size_t channel = 0; channel < curves.size(); ++channel) {
            if (fields[channel + 1].empty()) {
                continue;
            }

            const double value = std::stod(fields[channel + 1]);
            CurveExtent& curve = curves[channel];
            curve.first_wavelength = std::min(curve.first_wavelength, wavelength);
            curve.last_wavelength = std::max(curve.last_wavelength, wavelength);

            if (value > curve.peak_value) {
                curve.peak_value = value;
                curve.peak_wavelength = wavelength;
            }
        }
    }

    const std::array<CurveExtent, 3> expected = {{
        {360.0, 495.0, 465.0, 2.677431},
        {480.0, 585.0, 545.0, 2.456505},
        {575.0, 675.0, 645.0, 2.410890}
    }};

    for (std::size_t channel = 0; channel < curves.size(); ++channel) {
        passed &= test::check(
            near(curves[channel].first_wavelength, expected[channel].first_wavelength)
            && near(curves[channel].last_wavelength, expected[channel].last_wavelength)
            && near(curves[channel].peak_wavelength, expected[channel].peak_wavelength)
            && near(curves[channel].peak_value, expected[channel].peak_value),
            "Kodak 50D sensitivity support and calibrated peak match the paper trace");
    }

    return test::finish(
        passed,
        "Kodak 50D spectral sensitivity");
}
