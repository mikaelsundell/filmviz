// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "granularitymodel.h"
#include "imageprocessor.h"
#include "test_common.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

int
main()
{
    bool passed = true;
    GranularityModel model;
    const std::string resources = FILMVIZ_TEST_RESOURCE_DIR;

    passed &= test::check(
        model.load(
            resources
                + "/profiles/verita_200d/kodak_verita_200d_diffuse_rms_granularity_curves.csv",
            resources
                + "/profiles/kodak_2383/kodak_2383_diffuse_rms_granularity_curves.csv"),
        "measured Verita and 2383 granularity curves load");

    const FilmDensity negative_density = {
        1.587837f,
        1.886565f,
        2.320899f
    };

    const FilmDensity negative_sigma =
        model.negative_sigma(
            negative_density);

    passed &= test::near(
        negative_sigma.red,
        0.007731,
        1e-6,
        "Verita red RMS lookup");

    passed &= test::near(
        negative_sigma.green,
        0.006458,
        1e-6,
        "Verita green RMS lookup");

    passed &= test::near(
        negative_sigma.blue,
        0.016148,
        1e-6,
        "Verita blue RMS lookup");

    const FilmDensity print_density = {
        0.171354f,
        0.229659f,
        0.279110f
    };

    const FilmDensity print_sigma =
        model.print_sigma(
            print_density);

    passed &= test::near(
        print_sigma.red,
        0.003765,
        1e-6,
        "2383 red RMS lookup");

    passed &= test::near(
        print_sigma.green,
        0.004553,
        1e-6,
        "2383 green RMS lookup");

    passed &= test::near(
        print_sigma.blue,
        0.023226,
        1e-6,
        "2383 blue RMS lookup");

    const float repeated_a =
        GranularityModel::normal_sample(
            42u,
            17,
            29,
            0,
            1);

    const float repeated_b =
        GranularityModel::normal_sample(
            42u,
            17,
            29,
            0,
            1);

    passed &= test::near(
        repeated_a,
        repeated_b,
        0.0,
        "grain is deterministic for a fixed seed and coordinate");

    constexpr int sample_count = 100000;
    double sum = 0.0;
    double sum_squared = 0.0;

    for (int i = 0; i < sample_count; ++i) {
        const double value =
            GranularityModel::normal_sample(
                91u,
                i,
                i * 7,
                1,
                2);

        sum += value;
        sum_squared += value * value;
    }

    const double mean = sum / sample_count;
    const double variance =
        sum_squared / sample_count
        - mean * mean;

    passed &= test::check(
        std::abs(mean) < 0.015,
        "grain generator has approximately zero mean");

    passed &= test::check(
        std::abs(std::sqrt(variance) - 1.0) < 0.02,
        "grain generator has approximately unit RMS");

    const std::array<float, 3> channel_noise = {{
        0.012f,
        -0.007f,
        0.031f
    }};
    const std::array<float, 3> neutral =
        ImageProcessor::mix_grain_chroma(
            channel_noise,
            0.0f);
    const std::array<float, 3> measured =
        ImageProcessor::mix_grain_chroma(
            channel_noise,
            1.0f);
    const std::array<float, 3> reduced =
        ImageProcessor::mix_grain_chroma(
            channel_noise,
            0.35f);

    passed &= test::near(
        neutral[0],
        neutral[1],
        1e-8,
        "zero chroma makes red and green grain identical");

    passed &= test::near(
        neutral[1],
        neutral[2],
        1e-8,
        "zero chroma makes green and blue grain identical");

    for (int channel = 0; channel < 3; ++channel) {
        passed &= test::near(
            measured[channel],
            channel_noise[channel],
            1e-8,
            "unit chroma preserves measured channel grain");
    }

    const auto luma =
        [](const std::array<float, 3>& value) {
            return
                0.2126 * value[0]
                + 0.7152 * value[1]
                + 0.0722 * value[2];
        };

    passed &= test::near(
        luma(reduced),
        luma(channel_noise),
        1e-8,
        "grain chroma reduction preserves weighted luminance noise");

    return
        test::finish(
            passed,
            "measured two-stage granularity model");
}
