// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "test_common.h"

#include <array>

int
main()
{
    bool passed = true;

    FilmPipeline::Settings base_settings;
    base_settings.resources_directory = FILMVIZ_TEST_RESOURCE_DIR;
    base_settings.negative_profile = "verita-200d";
    base_settings.exposure_stops = 0.0f;

    FilmPipeline base_pipeline;

    passed &= test::check(
        base_pipeline.initialize(base_settings),
        "zero-stop pipeline initializes");

    FilmPipeline::Settings exposed_settings = base_settings;
    exposed_settings.exposure_stops = 1.0f;

    FilmPipeline exposed_pipeline;

    passed &= test::check(
        exposed_pipeline.initialize(exposed_settings),
        "+1-stop pipeline initializes");

    if (!passed) {
        return test::finish(passed, "exposure boundary");
    }

    const std::array<std::array<float, 3>, 3> samples = {{
        {{0.18f, 0.18f, 0.18f}},
        {{0.42f, 0.16f, 0.07f}},
        {{0.04f, 0.12f, 0.48f}}
    }};

    for (const std::array<float, 3>& ap0 : samples) {
        FilmExposure exposure;

        passed &= test::check(
            base_pipeline.negative_exposure(ap0, exposure),
            "raw negative exposure is available");

        FilmExposure scaled = exposure;
        scaled.red *= 2.0f;
        scaled.green *= 2.0f;
        scaled.blue *= 2.0f;

        const FilmPipeline::Result decomposed =
            base_pipeline.process_negative_exposure(scaled);

        const FilmPipeline::Result direct =
            exposed_pipeline.process(ap0);

        passed &= test::check(
            decomposed.valid && direct.valid,
            "both exposure paths are valid");

        if (!decomposed.valid || !direct.valid) {
            continue;
        }

        for (int channel = 0; channel < 3; ++channel) {
            passed &= test::near(
                decomposed.ap0[channel],
                direct.ap0[channel],
                2e-6,
                "negative-exposure boundary matches direct +1 stop AP0");

            passed &= test::near(
                decomposed.rec709_gamma24[channel],
                direct.rec709_gamma24[channel],
                2e-6,
                "negative-exposure boundary matches direct +1 stop Rec709");
        }
    }

    return test::finish(passed, "exposure boundary");
}
