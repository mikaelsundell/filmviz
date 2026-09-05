// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "test_common.h"

#include <array>
#include <cmath>
#include <fstream>
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

int
find_sample(
    const std::vector<Sample>& samples,
    double wavelength)
{
    for (int index = 0;
         index < static_cast<int>(samples.size());
         ++index) {

        if (samples[index].wavelength == wavelength) {
            return index;
        }
    }

    return -1;
}

std::array<double, 3>
measure_signal(
    const std::vector<Sample>& samples,
    const std::vector<double>& transmission)
{
    std::array<double, 3> signal = {{0.0, 0.0, 0.0}};

    if (samples.size() < 2
        || transmission.size() != samples.size()) {

        return signal;
    }

    const double wavelength_step =
        samples[1].wavelength
        - samples[0].wavelength;

    for (std::size_t index = 0;
         index < samples.size();
         ++index) {

        for (int channel = 0;
             channel < 3;
             ++channel) {

            signal[channel] +=
                transmission[index]
                * samples[index].response[channel]
                * wavelength_step;
        }
    }

    return signal;
}

std::array<double, 3>
density_from_signal(
    const std::array<double, 3>& signal)
{
    return {{
        -std::log10(signal[0]),
        -std::log10(signal[1]),
        -std::log10(signal[2])
    }};
}

std::array<double, 3>
measure_feature(
    const std::vector<Sample>& samples,
    double wavelength)
{
    std::vector<double> transmission(
        samples.size(),
        0.1);

    const int index =
        find_sample(
            samples,
            wavelength);

    if (index >= 0) {
        transmission[static_cast<std::size_t>(index)] = 1.0;
    }

    return
        density_from_signal(
            measure_signal(
                samples,
                transmission));
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
                "ACES APD spectral selectivity");
    }

    const std::vector<double> neutral_transmission(
        samples.size(),
        0.1);

    const std::array<double, 3> neutral_density =
        density_from_signal(
            measure_signal(
                samples,
                neutral_transmission));

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            neutral_density[channel],
            1.0,
            2e-14,
            "flat 10 percent transmission produces neutral density 1");
    }

    const std::array<double, 3> red_feature =
        measure_feature(
            samples,
            692.0);

    passed &= test::near(
        red_feature[0],
        0.8590666946469891,
        2e-14,
        "692 nm transmission feature changes red APD");

    passed &= test::near(
        red_feature[1],
        1.0,
        2e-14,
        "692 nm transmission feature leaves green APD unchanged");

    passed &= test::near(
        red_feature[2],
        1.0,
        2e-14,
        "692 nm transmission feature leaves blue APD unchanged");

    const std::array<double, 3> green_feature =
        measure_feature(
            samples,
            550.0);

    passed &= test::near(
        green_feature[0],
        1.0,
        2e-14,
        "550 nm transmission feature leaves red APD unchanged");

    passed &= test::near(
        green_feature[1],
        0.7300080897867159,
        2e-14,
        "550 nm transmission feature changes green APD");

    passed &= test::near(
        green_feature[2],
        1.0,
        2e-14,
        "550 nm transmission feature leaves blue APD unchanged");

    const std::array<double, 3> blue_feature =
        measure_feature(
            samples,
            466.0);

    passed &= test::near(
        blue_feature[0],
        1.0,
        2e-14,
        "466 nm transmission feature leaves red APD unchanged");

    passed &= test::near(
        blue_feature[1],
        0.9963093186567562,
        2e-14,
        "466 nm transmission feature produces small green APD response");

    passed &= test::near(
        blue_feature[2],
        0.7884739885349527,
        2e-14,
        "466 nm transmission feature changes blue APD");

    passed &= test::check(
        blue_feature[2] < blue_feature[1]
        && blue_feature[1] < neutral_density[1],
        "466 nm feature is predominantly blue with the expected green overlap");

    return
        test::finish(
            passed,
            "ACES APD spectral selectivity");
}
