// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdyemodel.h"
#include "filmpipeline.h"
#include "filmstock.h"
#include "test_common.h"

#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct APDSample
{
    double wavelength = 0.0;
    std::array<double, 3> response = {{0.0, 0.0, 0.0}};
};

bool
load_apd(
    const std::string& filename,
    std::vector<APDSample>& samples)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        return false;
    }

    std::string line;
    std::getline(file, line);

    if (line != "wavelength_nm,red,green,blue") {
        return false;
    }

    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        std::stringstream stream(line);
        APDSample sample;
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

        samples.push_back(sample);
    }

    return !samples.empty();
}

std::array<double, 3>
measure_apd(
    const SampledCurve& transmittance,
    const std::vector<APDSample>& samples)
{
    std::array<double, 3> signal = {{0.0, 0.0, 0.0}};

    if (!transmittance.valid()
        || samples.size() < 2) {
        return signal;
    }

    const double step =
        samples[1].wavelength
        - samples[0].wavelength;

    for (const APDSample& sample : samples) {
        const double t =
            static_cast<double>(
                transmittance.sample(
                    static_cast<float>(sample.wavelength),
                    0.0f));

        for (int channel = 0; channel < 3; ++channel) {
            signal[channel] +=
                t
                * sample.response[channel]
                * step;
        }
    }

    for (int channel = 0; channel < 3; ++channel) {
        signal[channel] =
            signal[channel] > 0.0
                ? -std::log10(signal[channel])
                : 0.0;
    }

    return signal;
}

bool
finite_density(
    const std::array<double, 3>& density)
{
    return
        std::isfinite(density[0])
        && std::isfinite(density[1])
        && std::isfinite(density[2]);
}

} // namespace

int
main()
{
    bool passed = true;

    const std::string resources =
        FILMVIZ_TEST_RESOURCE_DIR;

    std::vector<APDSample> apd;

    passed &= test::check(
        load_apd(
            resources
                + "/densitometry/apd/aces_apd_scanner_responsivities.csv",
            apd),
        "ACES APD scanner resource loads");

    FilmStock stock(
        "Kodak Verita 200D");

    passed &= test::check(
        stock.load(
            resources
                + "/profiles/verita_200d/kodak_verita_200d_spectral_sensitivity_curves.csv",
            resources
                + "/profiles/verita_200d/kodak_verita_200d_sensitometric_curves.csv"),
        "Kodak Verita 200D stock loads");

    FilmDyeModel dye_model;

    passed &= test::check(
        dye_model.load_and_estimate(
            resources
                + "/profiles/verita_200d/kodak_verita_200d_spectral_dye_density_curves.csv",
            stock,
            380.0f,
            700.0f,
            5.0f,
            -0.515f),
        "Kodak Verita 200D spectral dye model loads");

    FilmPipeline::Settings settings;
    settings.resources_directory = resources;

    FilmPipeline pipeline;

    passed &= test::check(
        pipeline.initialize(settings),
        "FilmViz production pipeline initializes");

    if (!passed) {
        return test::finish(
            false,
            "ACES APD Verita model");
    }

    const std::array<float, 3> stop_minus_one = {{
        0.09f, 0.09f, 0.09f
    }};

    const std::array<float, 3> stop_zero = {{
        0.18f, 0.18f, 0.18f
    }};

    const std::array<float, 3> stop_plus_one = {{
        0.36f, 0.36f, 0.36f
    }};

    const FilmPipeline::Result result_minus_one =
        pipeline.process(stop_minus_one);

    const FilmPipeline::Result result_zero =
        pipeline.process(stop_zero);

    const FilmPipeline::Result result_plus_one =
        pipeline.process(stop_plus_one);

    passed &= test::check(
        result_minus_one.valid
        && result_zero.valid
        && result_plus_one.valid,
        "production pipeline produces valid -1, 0, +1 stop neutral states");

    const SampledCurve transmittance_minus_one =
        dye_model.synthesize_transmittance(
            result_minus_one.calibrated_negative_density);

    const SampledCurve transmittance_zero =
        dye_model.synthesize_transmittance(
            result_zero.calibrated_negative_density);

    const SampledCurve transmittance_plus_one =
        dye_model.synthesize_transmittance(
            result_plus_one.calibrated_negative_density);

    passed &= test::check(
        transmittance_minus_one.valid()
        && transmittance_zero.valid()
        && transmittance_plus_one.valid(),
        "production calibrated negative densities synthesize valid spectra");

    const std::array<double, 3> apd_minus_one =
        measure_apd(
            transmittance_minus_one,
            apd);

    const std::array<double, 3> apd_zero =
        measure_apd(
            transmittance_zero,
            apd);

    const std::array<double, 3> apd_plus_one =
        measure_apd(
            transmittance_plus_one,
            apd);

    passed &= test::check(
        finite_density(apd_minus_one)
        && finite_density(apd_zero)
        && finite_density(apd_plus_one),
        "Verita production spectra produce finite APD measurements");

    for (int channel = 0; channel < 3; ++channel) {
        passed &= test::check(
            apd_minus_one[channel]
                < apd_zero[channel]
            && apd_zero[channel]
                < apd_plus_one[channel],
            "Verita APD density increases with scene exposure");
    }

    const FilmDyeModel::Diagnostics& diagnostics =
        dye_model.diagnostics();

    const SampledCurve calibration_transmittance =
        dye_model.synthesize_transmittance(
            diagnostics.calibration_record_density);

    const std::array<double, 3> calibration_apd =
        measure_apd(
            calibration_transmittance,
            apd);

    passed &= test::check(
        finite_density(calibration_apd),
        "Verita calibrated midscale reference produces finite APD measurement");

    return test::finish(
        passed,
        "ACES APD Verita model");
}
