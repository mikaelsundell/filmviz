// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "bleachbypass.h"
#include "filmpipeline.h"
#include "test_common.h"

#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>

namespace {

bool
same_curve(
    const SampledCurve& a,
    const SampledCurve& b,
    double tolerance)
{
    if (!a.valid()
        || !b.valid()
        || a.x.size() != b.x.size()
        || a.y.size() != b.y.size()) {
        return false;
    }

    for (std::size_t i = 0;
         i < a.x.size();
         ++i) {

        if (std::abs(
                static_cast<double>(a.x[i])
                - static_cast<double>(b.x[i])) > tolerance
            || std::abs(
                static_cast<double>(a.y[i])
                - static_cast<double>(b.y[i])) > tolerance) {
            return false;
        }
    }

    return true;
}

float
ap0_luminance(
    const std::array<float, 3>& ap0)
{
    return
        0.34396645f * ap0[0]
        + 0.72816610f * ap0[1]
        - 0.07213255f * ap0[2];
}

FilmPipeline::Result
process_gray(
    FilmPipeline& pipeline,
    float stops)
{
    const float value =
        0.18f
        * std::pow(
            2.0f,
            stops);

    return
        pipeline.process({{
            value,
            value,
            value
        }});
}

} // namespace

int
main()
{
    bool passed = true;

    SampledCurve base_density;
    base_density.x = {400.0f, 500.0f, 600.0f, 700.0f};
    base_density.y = {0.40f, 0.80f, 1.20f, 1.60f};

    const BleachBypass::Result negative_zero =
        BleachBypass::apply_negative(
            base_density,
            0.0f);

    const BleachBypass::Result print_zero =
        BleachBypass::apply_print(
            base_density,
            0.0f);

    passed &= test::check(
        negative_zero.valid
        && print_zero.valid
        && same_curve(
            negative_zero.spectral_density,
            base_density,
            0.0)
        && same_curve(
            print_zero.spectral_density,
            base_density,
            0.0),
        "zero bleach bypass preserves spectral density exactly");

    const BleachBypass::Result negative_half =
        BleachBypass::apply_negative(
            base_density,
            0.5f);

    passed &= test::near(
        negative_half.mean_density_after,
        negative_half.mean_density_before,
        1e-6,
        "negative bypass preserves mean spectral density");

    passed &= test::check(
        negative_half.spectral_density.y.front()
            > base_density.y.front()
        && negative_half.spectral_density.y.back()
            < base_density.y.back(),
        "negative bypass applies the expected cool/cyan spectral tilt");

    const BleachBypass::Result print_half =
        BleachBypass::apply_print(
            base_density,
            0.5f);

    passed &= test::near(
        print_half.mean_density_after,
        print_half.mean_density_before,
        1e-6,
        "print bypass preserves mean spectral density");

    passed &= test::check(
        print_half.spectral_span_after
            < print_half.spectral_span_before,
        "print bypass contracts spectral colour differences");

    const BleachBypass::Result print_full =
        BleachBypass::apply_print(
            base_density,
            2.0f);

    passed &= test::check(
        print_full.spectral_span_after
            < print_half.spectral_span_after,
        "bleach bypass amount clamps to the full modeled print look");

    FilmPipeline::Settings baseline_settings;
    baseline_settings.resources_directory =
        FILMVIZ_TEST_RESOURCE_DIR;

    FilmPipeline::Settings negative_settings =
        baseline_settings;
    negative_settings.negative_bleach_bypass = 0.5f;

    FilmPipeline::Settings print_settings =
        baseline_settings;
    print_settings.print_bleach_bypass = 0.5f;

    FilmPipeline baseline_pipeline;
    FilmPipeline negative_pipeline;
    FilmPipeline print_pipeline;

    passed &= test::check(
        baseline_pipeline.initialize(
            baseline_settings),
        std::string("baseline pipeline initializes: ")
            + baseline_pipeline.error());

    passed &= test::check(
        negative_pipeline.initialize(
            negative_settings),
        std::string("negative-bypass pipeline initializes: ")
            + negative_pipeline.error());

    passed &= test::check(
        print_pipeline.initialize(
            print_settings),
        std::string("print-bypass pipeline initializes: ")
            + print_pipeline.error());

    if (passed) {
        std::cout
            << std::fixed
            << std::setprecision(6)
            << "Bleach-bypass neutral diagnostics\n"
            << "stop    baseline Y    negative Y    print Y\n";

        for (int stop = -2;
             stop <= 2;
             stop += 2) {

            const FilmPipeline::Result baseline =
                process_gray(
                    baseline_pipeline,
                    static_cast<float>(stop));

            const FilmPipeline::Result negative =
                process_gray(
                    negative_pipeline,
                    static_cast<float>(stop));

            const FilmPipeline::Result print =
                process_gray(
                    print_pipeline,
                    static_cast<float>(stop));

            passed &= test::check(
                baseline.valid
                && negative.valid
                && print.valid,
                "baseline and bypass pipelines process neutral diagnostic stop");

            passed &= test::near(
                negative.negative_bleach_mean_density_delta,
                0.0,
                2e-6,
                "negative bypass adds no neutral density veil");

            passed &= test::near(
                print.print_bleach_mean_density_delta,
                0.0,
                2e-6,
                "print bypass adds no neutral density veil");

            std::cout
                << std::setw(4)
                << stop
                << "    "
                << std::setw(10)
                << ap0_luminance(baseline.ap0)
                << "    "
                << std::setw(10)
                << ap0_luminance(negative.ap0)
                << "    "
                << std::setw(10)
                << ap0_luminance(print.ap0)
                << "\n";
        }
    }

    return
        test::finish(
            passed,
            "bleach bypass process model");
}
