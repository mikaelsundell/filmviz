// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "test_common.h"

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

int
main()
{
    bool passed = true;

    const std::string filename =
        std::string(FILMVIZ_TEST_RESOURCE_DIR)
        + "/densitometry/apd/aces_apd_scanner_responsivities.csv";

    std::ifstream file(
        filename.c_str());

    passed &= test::check(
        static_cast<bool>(file),
        "ACES APD scanner resource opens");

    std::string line;
    std::getline(
        file,
        line);

    passed &= test::check(
        line == "wavelength_nm,red,green,blue",
        "ACES APD scanner resource has the canonical header");

    int samples = 0;
    double first_wavelength = 0.0;
    double previous_wavelength = 0.0;

    std::array<double, 3> peak = {{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    }};

    std::array<double, 3> peak_wavelength = {{0.0, 0.0, 0.0}};

    while (std::getline(
        file,
        line)) {

        if (line.empty()) {
            continue;
        }

        std::stringstream stream(
            line);

        double wavelength = 0.0;
        std::array<double, 3> response = {{0.0, 0.0, 0.0}};
        char comma = 0;

        const bool parsed =
            static_cast<bool>(
                stream
                >> wavelength
                >> comma
                >> response[0]
                >> comma
                >> response[1]
                >> comma
                >> response[2]);

        passed &= test::check(
            parsed,
            "every ACES APD row parses");

        if (!parsed) {
            continue;
        }

        if (samples == 0) {
            first_wavelength = wavelength;
        }
        else {
            passed &= test::near(
                wavelength - previous_wavelength,
                2.0,
                1e-12,
                "ACES APD wavelength spacing");
        }

        for (int channel = 0;
             channel < 3;
             ++channel) {

            passed &= test::check(
                std::isfinite(response[channel])
                && response[channel] >= 0.0,
                "ACES APD response is finite and nonnegative");

            if (response[channel] > peak[channel]) {
                peak[channel] = response[channel];
                peak_wavelength[channel] = wavelength;
            }
        }

        previous_wavelength = wavelength;
        ++samples;
    }

    passed &= test::check(
        samples == 181,
        "ACES APD resource contains 181 samples");

    passed &= test::near(
        first_wavelength,
        368.0,
        0.0,
        "ACES APD first wavelength");

    passed &= test::near(
        previous_wavelength,
        728.0,
        0.0,
        "ACES APD last wavelength");

    const std::array<double, 3> expected_peak_wavelength = {{
        692.0,
        550.0,
        466.0
    }};

    const std::array<double, 3> expected_peak = {{
        0.0212974400477063,
        0.0478918028390261,
        0.0348621550389759
    }};

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            peak_wavelength[channel],
            expected_peak_wavelength[channel],
            0.0,
            "ACES APD channel peak wavelength");

        passed &= test::near(
            peak[channel],
            expected_peak[channel],
            1e-15,
            "ACES APD calibrated peak amplitude");
    }

    return
        test::finish(
            passed,
            "ACES APD resource integrity");
}
