// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "inputtransform.h"

#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace {

void
print_density(
    const char* label,
    const FilmDensity& density)
{
    std::cout
        << label
        << " = ("
        << density.red
        << ", "
        << density.green
        << ", "
        << density.blue
        << ")\n";
}

void
print_rgb(
    const char* label,
    const std::array<float, 3>& rgb)
{
    std::cout
        << label
        << " = ("
        << rgb[0]
        << ", "
        << rgb[1]
        << ", "
        << rgb[2]
        << ")\n";
}

} // namespace

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

    const InputTransform input(
        InputTransform::Encoding::AWG3_LogC3_EI800);

    const std::array<float, 3> encoded = {{
        0.49f,
        0.44f,
        0.42f
    }};

    const std::array<float, 3> ap0 =
        input.to_ap0(
            encoded);

    const FilmPipeline::Result result =
        pipeline.process(
            ap0);

    if (!result.valid) {
        std::cerr << "The spectral pipeline did not produce a valid result.\n";
        return EXIT_FAILURE;
    }

    std::cout << std::setprecision(8);
    print_rgb("input AP0", ap0);
    print_density("negative Status-M", result.negative_status_m_density);
    print_density("negative spectral coordinates", result.calibrated_negative_density);
    print_density("print records", result.print_density);
    print_rgb("viewed print AP0", result.ap0);
    print_rgb("Rec.709/Gamma 2.4 preview", result.rec709_gamma24);

    return EXIT_SUCCESS;
}
