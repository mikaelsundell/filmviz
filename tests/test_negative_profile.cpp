// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SensitometricSample
{
    double log_exposure = 0.0;
    double high_density = 0.0;
    double mid_density = 0.0;
    double low_density = 0.0;
};

bool
load_sensitometric_curve(
    const std::string& filename,
    std::vector<SensitometricSample>& samples)
{
    std::ifstream file(
        filename.c_str());

    if (!file) {
        return false;
    }

    std::string line;

    if (!std::getline(file, line)) {
        return false;
    }

    if (!line.empty()
        && line.back() == '\r') {

        line.pop_back();
    }

    if (line
        != "camera_stops,log_exposure_lux_seconds,curve_high_density,curve_mid_density,curve_low_density") {

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

        std::string field;
        std::array<double, 5> values = {{0.0, 0.0, 0.0, 0.0, 0.0}};

        bool parsed = true;

        for (std::size_t index = 0;
             index < values.size();
             ++index) {

            if (!std::getline(
                    stream,
                    field,
                    ',')) {

                parsed = false;
                break;
            }

            try {
                values[index] =
                    std::stod(
                        field);
            }
            catch (...) {
                parsed = false;
                break;
            }
        }

        if (!parsed) {
            return false;
        }

        SensitometricSample sample;
        sample.log_exposure = values[1];
        sample.high_density = values[2];
        sample.mid_density = values[3];
        sample.low_density = values[4];

        samples.push_back(
            sample);
    }

    return
        samples.size() >= 2;
}

double
interpolate(
    const std::vector<SensitometricSample>& samples,
    double log_exposure,
    double SensitometricSample::* member)
{
    if (samples.empty()) {
        return 0.0;
    }

    if (log_exposure <= samples.front().log_exposure) {
        return samples.front().*member;
    }

    if (log_exposure >= samples.back().log_exposure) {
        return samples.back().*member;
    }

    const auto upper =
        std::lower_bound(
            samples.begin(),
            samples.end(),
            log_exposure,
            [](const SensitometricSample& sample,
               double value) {

                return
                    sample.log_exposure
                    < value;
            });

    if (upper == samples.begin()) {
        return (*upper).*member;
    }

    const auto lower =
        upper - 1;

    const double span =
        upper->log_exposure
        - lower->log_exposure;

    if (!(span > 0.0)) {
        return (*lower).*member;
    }

    const double t =
        (log_exposure - lower->log_exposure)
        / span;

    return
        (1.0 - t)
            * ((*lower).*member)
        + t
            * ((*upper).*member);
}

std::array<double, 3>
expected_density(
    const std::vector<SensitometricSample>& samples,
    double log_exposure)
{
    // The Kodak negative sensitometric CSV convention is:
    //   high density -> blue record
    //   mid density  -> green record
    //   low density  -> red record
    return {{
        interpolate(
            samples,
            log_exposure,
            &SensitometricSample::low_density),
        interpolate(
            samples,
            log_exposure,
            &SensitometricSample::mid_density),
        interpolate(
            samples,
            log_exposure,
            &SensitometricSample::high_density)
    }};
}

bool
run_profile(
    const std::string& profile_id,
    const std::string& directory,
    const std::string& prefix)
{
    bool passed = true;

    FilmPipeline::Settings settings;
    settings.resources_directory =
        FILMVIZ_TEST_RESOURCE_DIR;
    settings.negative_profile =
        profile_id;

    FilmPipeline pipeline;

    passed &= test::check(
        pipeline.initialize(
            settings),
        profile_id
            + " pipeline initializes: "
            + pipeline.error());

    const std::string curve_filename =
        std::string(FILMVIZ_TEST_RESOURCE_DIR)
        + "/profiles/"
        + directory
        + "/"
        + prefix
        + "_sensitometric_curves.csv";

    std::vector<SensitometricSample> samples;

    passed &= test::check(
        load_sensitometric_curve(
            curve_filename,
            samples),
        profile_id
            + " sensitometric CSV loads");

    if (!passed) {
        return false;
    }

    constexpr double log10_two =
        0.3010299956639812;

    constexpr double zero_stop_log_exposure =
        -0.515;

    std::cout
        << "\n"
        << profile_id
        << "\n"
        << "stop      LogE"
        << "      CSV R      CSV G      CSV B"
        << "     Film R     Film G     Film B"
        << "    max diff\n";

    for (int stop = -8;
         stop <= 8;
         ++stop) {

        const float scene_value =
            0.18f
            * std::pow(
                2.0f,
                static_cast<float>(stop));

        const FilmPipeline::Result result =
            pipeline.process(
                {{
                    scene_value,
                    scene_value,
                    scene_value
                }});

        passed &= test::check(
            result.valid,
            profile_id
                + " processes stop "
                + std::to_string(stop));

        if (!result.valid) {
            continue;
        }

        const double log_exposure =
            zero_stop_log_exposure
            + static_cast<double>(stop)
                * log10_two;

        const std::array<double, 3> expected =
            expected_density(
                samples,
                log_exposure);

        const std::array<double, 3> actual = {{
            result.negative_status_m_density.red,
            result.negative_status_m_density.green,
            result.negative_status_m_density.blue
        }};

        double max_difference = 0.0;

        for (int channel = 0;
             channel < 3;
             ++channel) {

            const double difference =
                std::abs(
                    actual[channel]
                    - expected[channel]);

            max_difference =
                std::max(
                    max_difference,
                    difference);

            passed &= test::near(
                actual[channel],
                expected[channel],
                2e-4,
                profile_id
                    + " Status-M density follows digitized sensitometric curve");
        }

        std::cout
            << std::fixed
            << std::setprecision(3)
            << std::setw(4)
            << stop
            << "  "
            << std::setw(8)
            << log_exposure
            << "  "
            << std::setprecision(6)
            << std::setw(10)
            << expected[0]
            << " "
            << std::setw(10)
            << expected[1]
            << " "
            << std::setw(10)
            << expected[2]
            << "  "
            << std::setw(10)
            << actual[0]
            << " "
            << std::setw(10)
            << actual[1]
            << " "
            << std::setw(10)
            << actual[2]
            << "  "
            << std::scientific
            << std::setprecision(2)
            << max_difference
            << "\n";
    }

    return passed;
}

} // namespace

int
main()
{
    bool passed = true;

    passed &=
        run_profile(
            "verita-200d",
            "verita_200d",
            "kodak_verita_200d");

    passed &=
        run_profile(
            "kodak-50d",
            "kodak_50d",
            "kodak_50d");

    return
        test::finish(
            passed,
            "negative profile sensitometric ramp");
}
