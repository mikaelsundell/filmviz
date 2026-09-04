// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "diagramwriter.h"
#include "filmdyemodel.h"
#include "filmprocessor.h"
#include "filmstock.h"
#include "printfilmstock.h"
#include "spectralilluminant.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string
resource_path(
    const std::filesystem::path& resources,
    const std::string& relative_path)
{
    return
        (resources / relative_path)
        .string();
}

} // namespace

int
main(
    int argc,
    const char* argv[])
{
    const std::filesystem::path resources =
        argc > 1
            ? argv[1]
            : "Resources";

    const std::filesystem::path output_directory =
        argc > 2
            ? argv[2]
            : "profile_diagrams";

    std::error_code error;

    std::filesystem::create_directories(
        output_directory,
        error);

    if (error) {
        std::cerr
            << "Could not create output directory: "
            << output_directory
            << "\n";

        return EXIT_FAILURE;
    }

    FilmStock negative(
        "Kodak Verita 200D");

    if (!negative.load(
            resource_path(
                resources,
                "profiles/verita_200d/kodak_verita_200d_spectral_sensitivity_curves.csv"),
            resource_path(
                resources,
                "profiles/verita_200d/kodak_verita_200d_sensitometric_curves.csv"))) {

        std::cerr << "Could not load the Kodak Verita 200D profile.\n";
        return EXIT_FAILURE;
    }

    SpectralIlluminant d60(
        SpectralIlluminant::Standard::D60);

    FilmProcessor::Settings negative_settings;

    FilmProcessor negative_processor(
        nullptr,
        negative,
        d60.curve(),
        negative_settings);

    FilmDyeModel negative_dye_model;

    if (!negative_dye_model.load_and_estimate(
            resource_path(
                resources,
                "profiles/verita_200d/kodak_verita_200d_spectral_dye_density_curves.csv"),
            negative,
            negative_settings.wavelength_min_nm,
            negative_settings.wavelength_max_nm,
            negative_settings.wavelength_step_nm,
            -0.515f)) {

        std::cerr << "Could not build the Kodak Verita 200D dye model.\n";
        return EXIT_FAILURE;
    }

    PrintFilmStock print(
        "Kodak 2383 corrected production profile");

    if (!print.load(
            resource_path(
                resources,
                "profiles/kodak_2383/kodak_2383_spectral_sensitivity_curves.csv"),
            resource_path(
                resources,
                "profiles/kodak_2383/kodak_2383_sensitometric_curves.csv"),
            resource_path(
                resources,
                "profiles/kodak_2383/kodak_2383_corrected_spectral_dye_density_curves.csv"),
            resource_path(
                resources,
                "profiles/kodak_2383/kodak_2383_modulation_transfer_function_curves.csv"),
            resource_path(
                resources,
                "profiles/kodak_2383/kodak_2383_diffuse_rms_granularity_curves.csv"))) {

        std::cerr << "Could not load the Kodak 2383 profile.\n";
        return EXIT_FAILURE;
    }

    bool success = true;

    success =
        DiagramWriter::write_film_diagnostics(
            (output_directory / "verita_200d.png").string(),
            negative,
            d60.curve(),
            negative_processor.settings(),
            negative_processor.balance())
        && success;

    success =
        DiagramWriter::write_dye_model_diagnostics(
            (output_directory / "verita_200d.png").string(),
            negative,
            negative_dye_model)
        && success;

    success =
        DiagramWriter::write_print_stock_source_diagnostics(
            (output_directory / "kodak_2383.png").string(),
            print)
        && success;

    if (!success) {
        std::cerr << "One or more profile diagrams could not be written.\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "Wrote FilmViz profile diagrams to "
        << output_directory
        << "\n";

    return EXIT_SUCCESS;
}
