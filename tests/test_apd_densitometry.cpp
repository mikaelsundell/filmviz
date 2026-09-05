// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "test_common.h"

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Sample {
    double wavelength = 0.0;
    std::array<double, 3> response = {{0.0, 0.0, 0.0}};
};

bool
load_apd(
    const std::string& filename,
    std::vector<Sample>& samples)
{
    std::ifstream file(
        filename.c_str());

    if (!file) {
        return false;
    }

    std::string line;
    std::getline(
        file,
        line);

    if (line != "wavelength_nm,red,green,blue") {
        return false;
    }

    while (std::getline(
        file,
        line)) {

        if (line.empty()) {
            continue;
        }

        std::stringstream stream(
            line);

        Sample sample;
        char comma = 0;

        if (!(stream
              >> sample.wavelength
              >> comma
              >> sample.response[0]
              >> comma
              >> sample.response[1]
              >> comma
              >> sample.response[2])) {

            return false;
        }

        samples.push_back(
            sample);
    }

    return !samples.empty();
}

std::array<double, 3>
integrate_response(
    const std::vector<Sample>& samples,
    double transmission)
{
    std::array<double, 3> signal = {{0.0, 0.0, 0.0}};

    if (samples.size() < 2) {
        return signal;
    }

    const double wavelength_step =
        samples[1].wavelength
        - samples[0].wavelength;

    for (const Sample& sample : samples) {
        for (int channel = 0;
             channel < 3;
             ++channel) {

            signal[channel] +=
                transmission
                * sample.response[channel]
                * wavelength_step;
        }
    }

    return signal;
}

std::array<double, 3>
density_from_signal(
    const std::array<double, 3>& signal)
{
    std::array<double, 3> density = {{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()
    }};

    for (int channel = 0;
         channel < 3;
         ++channel) {

        if (signal[channel] > 0.0) {
            density[channel] =
                -std::log10(
                    signal[channel]);
        }
    }

    return density;
}

} // namespace

int
main()
{
    bool passed = true;

    const std::string filename =
        std::string(FILMVIZ_TEST_RESOURCE_DIR)
        + "/densitometry/apd/aces_apd_scanner_responsivities.csv";

    std::vector<Sample> samples;

    passed &= test::check(
        load_apd(
            filename,
            samples),
        "ACES APD scanner resource loads");

    if (!passed) {
        return
            test::finish(
                false,
                "ACES APD densitometry");
    }

    passed &= test::check(
        samples.size() == 181,
        "ACES APD densitometry uses the complete scanner grid");

    const double wavelength_step =
        samples[1].wavelength
        - samples[0].wavelength;

    passed &= test::near(
        wavelength_step,
        2.0,
        0.0,
        "ACES APD scanner wavelength spacing is 2 nm");

    std::array<double, 3> response_integral = {{0.0, 0.0, 0.0}};

    for (const Sample& sample : samples) {
        for (int channel = 0;
             channel < 3;
             ++channel) {

            response_integral[channel] +=
                sample.response[channel]
                * wavelength_step;
        }
    }

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            response_integral[channel],
            1.0,
            2e-14,
            "ACES APD scanner channel has unit integrated responsivity");
    }

    const std::array<double, 3> clear_signal =
        integrate_response(
            samples,
            1.0);

    const std::array<double, 3> clear_density =
        density_from_signal(
            clear_signal);

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            clear_signal[channel],
            1.0,
            2e-14,
            "100 percent flat transmission produces unit APD signal");

        passed &= test::near(
            clear_density[channel],
            0.0,
            2e-14,
            "100 percent flat transmission produces zero APD");
    }

    const std::array<double, 3> ten_percent_signal =
        integrate_response(
            samples,
            0.1);

    const std::array<double, 3> ten_percent_density =
        density_from_signal(
            ten_percent_signal);

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            ten_percent_signal[channel],
            0.1,
            2e-15,
            "10 percent flat transmission produces 0.1 APD signal");

        passed &= test::near(
            ten_percent_density[channel],
            1.0,
            2e-14,
            "10 percent flat transmission produces density 1");
    }

    const std::array<double, 3> one_percent_signal =
        integrate_response(
            samples,
            0.01);

    const std::array<double, 3> one_percent_density =
        density_from_signal(
            one_percent_signal);

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            one_percent_signal[channel],
            0.01,
            2e-16,
            "1 percent flat transmission produces 0.01 APD signal");

        passed &= test::near(
            one_percent_density[channel],
            2.0,
            2e-14,
            "1 percent flat transmission produces density 2");
    }

    return
        test::finish(
            passed,
            "ACES APD densitometry");
}
