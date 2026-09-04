// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdensitycalibration.h"
#include "filmpipeline.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>

int
main(
    int argc,
    const char* argv[])
{
    FilmPipeline::Settings settings;

    if (argc > 1) {
        settings.resources_directory = argv[1];
    }

    FilmPipeline pipeline;

    if (!pipeline.initialize(
            settings)) {

        std::cerr
            << "Could not initialize FilmViz: "
            << pipeline.error()
            << "\n";

        return EXIT_FAILURE;
    }

    const FilmDensityCalibration* calibration =
        pipeline.negative_density_calibration();

    if (!calibration
        || !calibration->valid()) {

        std::cerr << "The Status-M calibration is unavailable.\n";
        return EXIT_FAILURE;
    }

    const FilmDensityCalibration::Vec3 zero =
        calibration->zero_target();

    const FilmDensity target = {
        static_cast<float>(zero[0] + 0.2),
        static_cast<float>(zero[1] + 0.2),
        static_cast<float>(zero[2] + 0.2)
    };

    const FilmDensityCalibration::Result result =
        calibration->solve(
            target);

    if (!result.valid) {
        std::cerr << "The Status-M closure solve failed.\n";
        return EXIT_FAILURE;
    }

    std::cout
        << std::setprecision(9)
        << "Status-M target = ("
        << result.desired_status_m[0] << ", "
        << result.desired_status_m[1] << ", "
        << result.desired_status_m[2] << ")\n"
        << "Status-M measured = ("
        << result.measured_status_m[0] << ", "
        << result.measured_status_m[1] << ", "
        << result.measured_status_m[2] << ")\n"
        << "closure residual = ("
        << result.residual[0] << ", "
        << result.residual[1] << ", "
        << result.residual[2] << ")\n"
        << "spectral coordinates = ("
        << result.calibrated_density.red << ", "
        << result.calibrated_density.green << ", "
        << result.calibrated_density.blue << ")\n"
        << "converged = "
        << (result.converged ? "yes" : "no")
        << "\n"
        << "D-min projected = "
        << (result.floor_projected ? "yes" : "no")
        << "\n";

    return EXIT_SUCCESS;
}
