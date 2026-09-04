// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdensitycalibration.h"
#include "filmpipeline.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>

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
                "Status-M density calibration");
    }

    const FilmDensityCalibration* calibration =
        pipeline.negative_density_calibration();

    passed &= test::check(
        calibration != nullptr
        && calibration->valid(),
        "production density calibration is available");

    const FilmDensityCalibration::Vec3 zero =
        calibration->zero_target();

    const FilmDensity zero_density = {
        static_cast<float>(zero[0]),
        static_cast<float>(zero[1]),
        static_cast<float>(zero[2])
    };

    const FilmDensityCalibration::Result anchor =
        calibration->solve(
            zero_density);

    passed &= test::check(
        anchor.valid
        && anchor.converged,
        "zero-stop anchor converges");

    for (double residual : anchor.residual) {
        passed &= test::near(
            residual,
            0.0,
            2e-6,
            "zero-stop Status-M closure");
    }

    const FilmDensity raised = {
        zero_density.red + 0.20f,
        zero_density.green + 0.20f,
        zero_density.blue + 0.20f
    };

    const FilmDensityCalibration::Result interior =
        calibration->solve(
            raised);

    passed &= test::check(
        interior.valid
        && interior.converged
        && !interior.floor_projected,
        "achievable neutral density excursion converges without projection");

    for (double residual : interior.residual) {
        passed &= test::near(
            residual,
            0.0,
            2e-5,
            "interior Status-M closure");
    }

    const FilmDensity below_floor = {
        -10.0f,
        -10.0f,
        -10.0f
    };

    const FilmDensityCalibration::Result floor =
        calibration->solve(
            below_floor);

    passed &= test::check(
        floor.valid
        && floor.floor_projected,
        "unphysical low density is projected to measured D-min");

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::check(
            floor.desired_status_m[channel]
                >= calibration->minimum_status_m()[channel],
            "projected target respects the Status-M floor");
    }

    return
        test::finish(
            passed,
            "Status-M density calibration");
}
