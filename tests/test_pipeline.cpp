// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "test_common.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace {

bool
finite_density(
    const FilmDensity& density)
{
    return
        std::isfinite(density.red)
        && std::isfinite(density.green)
        && std::isfinite(density.blue);
}

bool
finite_rgb(
    const std::array<float, 3>& rgb)
{
    return
        std::isfinite(rgb[0])
        && std::isfinite(rgb[1])
        && std::isfinite(rgb[2]);
}

double
ap0_luminance(
    const std::array<float, 3>& ap0)
{
    // AP0 RGB to XYZ Y row.
    return
        0.34396645 * ap0[0]
        + 0.72816610 * ap0[1]
        - 0.07213255 * ap0[2];
}

} // namespace

int
main()
{
    bool passed = true;

    FilmPipeline::Settings settings;
    settings.resources_directory = FILMVIZ_TEST_RESOURCE_DIR;

    FilmPipeline pipeline;

    passed &= test::check(
        pipeline.initialize(
            settings),
        std::string("pipeline initializes: ") + pipeline.error());

    if (!passed) {
        return
            test::finish(
                false,
                "production spectral pipeline");
    }

    const std::vector<std::array<float, 3>> samples = {
        {{0.02f, 0.02f, 0.02f}},
        {{0.18f, 0.18f, 0.18f}},
        {{0.80f, 0.80f, 0.80f}},
        {{0.30f, 0.12f, 0.04f}},
        {{0.05f, 0.20f, 0.35f}}
    };

    std::vector<FilmPipeline::Result> results;

    for (const std::array<float, 3>& sample : samples) {
        const FilmPipeline::Result result =
            pipeline.process(
                sample);

        passed &= test::check(
            result.valid,
            "representative AP0 sample processes successfully");

        passed &= test::check(
            finite_density(result.negative_status_m_density)
            && finite_density(result.calibrated_negative_density)
            && finite_density(result.print_density)
            && finite_density(result.negative_granularity_sigma)
            && finite_density(result.print_granularity_sigma)
            && finite_rgb(result.ap0)
            && finite_rgb(result.rec709_gamma24),
            "every production stage returns finite values");

        results.push_back(
            result);
    }

    if (results.size() >= 3) {
        const double shadows =
            ap0_luminance(
                results[0].ap0);

        const double middle =
            ap0_luminance(
                results[1].ap0);

        const double highlights =
            ap0_luminance(
                results[2].ap0);

        passed &= test::check(
            shadows < middle
            && middle < highlights,
            "neutral scene exposure remains monotonic through negative and print");

        const FilmPipeline::Result& reference =
            results[1];

        passed &= test::near(
            reference.negative_status_m_density.red,
            1.17967,
            1e-4,
            "middle-gray Verita Status-M red density");

        passed &= test::near(
            reference.negative_status_m_density.green,
            1.56155,
            1e-4,
            "middle-gray Verita Status-M green density");

        passed &= test::near(
            reference.negative_status_m_density.blue,
            1.93969,
            1e-4,
            "middle-gray Verita Status-M blue density");

        const std::array<double, 3> expected_ap0 = {{
            0.0999474,
            0.0996959,
            0.0999869
        }};

        for (int channel = 0;
             channel < 3;
             ++channel) {

            passed &= test::near(
                reference.ap0[channel],
                expected_ap0[channel],
                5e-5,
                "middle-gray viewed-print AP0 regression");
        }

        FilmPipeline::Settings exposed_settings = settings;
        exposed_settings.exposure_stops = 1.0f;
        FilmPipeline exposed_pipeline;

        passed &= test::check(
            exposed_pipeline.initialize(exposed_settings),
            std::string("exposure-adjusted pipeline initializes: ")
                + exposed_pipeline.error());

        const FilmPipeline::Result exposed =
            exposed_pipeline.process(samples[1]);

        passed &= test::check(
            exposed.valid
            && exposed.negative_status_m_density.red
                > reference.negative_status_m_density.red
            && exposed.negative_status_m_density.green
                > reference.negative_status_m_density.green
            && exposed.negative_status_m_density.blue
                > reference.negative_status_m_density.blue,
            "+1 exposure stop increases all negative Status-M densities");

        FilmPipeline::Settings pushed_settings = settings;
        pushed_settings.push_pull_stops = 1.0f;
        FilmPipeline pushed_pipeline;

        passed &= test::check(
            pushed_pipeline.initialize(pushed_settings),
            std::string("push-adjusted pipeline initializes: ")
                + pushed_pipeline.error());

        const FilmPipeline::Result pushed_middle =
            pushed_pipeline.process(samples[1]);
        const FilmPipeline::Result pushed_highlight =
            pushed_pipeline.process(samples[2]);

        passed &= test::near(
            pushed_middle.negative_status_m_density.green,
            reference.negative_status_m_density.green,
            1e-5,
            "push/pull preserves the calibrated middle-gray pivot");

        passed &= test::check(
            pushed_highlight.valid
            && std::abs(
                pushed_highlight.negative_status_m_density.green
                - pushed_middle.negative_status_m_density.green)
                > std::abs(
                    results[2].negative_status_m_density.green
                    - reference.negative_status_m_density.green),
            "+1 push stop increases negative contrast about middle gray");
    }


    FilmPipeline::Settings kodak_50d_settings = settings;
    kodak_50d_settings.negative_profile = "kodak-50d";

    FilmPipeline kodak_50d_pipeline;

    passed &= test::check(
        kodak_50d_pipeline.initialize(
            kodak_50d_settings),
        std::string("Kodak Vision3 50D 5203/7203 pipeline initializes: ")
            + kodak_50d_pipeline.error());

    if (kodak_50d_pipeline.valid()) {
        const FilmPipeline::Result kodak_50d_shadow =
            kodak_50d_pipeline.process(
                {{0.02f, 0.02f, 0.02f}});
        const FilmPipeline::Result kodak_50d_middle =
            kodak_50d_pipeline.process(
                {{0.18f, 0.18f, 0.18f}});
        const FilmPipeline::Result kodak_50d_highlight =
            kodak_50d_pipeline.process(
                {{0.80f, 0.80f, 0.80f}});

        passed &= test::check(
            kodak_50d_shadow.valid
            && kodak_50d_middle.valid
            && kodak_50d_highlight.valid,
            "Kodak Vision3 50D 5203/7203 processes representative neutral exposures");

        passed &= test::check(
            finite_density(kodak_50d_middle.negative_status_m_density)
            && finite_density(kodak_50d_middle.calibrated_negative_density)
            && finite_density(kodak_50d_middle.negative_granularity_sigma)
            && finite_rgb(kodak_50d_middle.ap0)
            && finite_rgb(kodak_50d_middle.rec709_gamma24),
            "Kodak Vision3 50D 5203/7203 production stages return finite values");

        passed &= test::check(
            ap0_luminance(kodak_50d_shadow.ap0)
                < ap0_luminance(kodak_50d_middle.ap0)
            && ap0_luminance(kodak_50d_middle.ap0)
                < ap0_luminance(kodak_50d_highlight.ap0),
            "Kodak Vision3 50D 5203/7203 neutral exposure remains monotonic through print");
    }

    return
        test::finish(
            passed,
            "production spectral pipeline");
}
