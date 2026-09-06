// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizofxprocessor.h"
#include "negativeprofile.h"
#include "printprofile.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{

struct Options
{
    std::string resources = "resources";
    std::string output = "filmviz_ofx_cache";
    int lut_size = 33;
};

bool
parse_arguments(
    int argc,
    char** argv,
    Options& options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        auto value =
            [&](std::string& destination) -> bool {
                if (index + 1 >= argc) {
                    return false;
                }
                destination = argv[++index];
                return true;
            };

        if (argument == "--resources") {
            if (!value(options.resources)) {
                return false;
            }
        }
        else if (argument == "--output") {
            if (!value(options.output)) {
                return false;
            }
        }
        else if (argument == "--lut-size") {
            if (index + 1 >= argc) {
                return false;
            }
            options.lut_size = std::stoi(argv[++index]);
        }
        else if (argument == "--help" || argument == "-h") {
            std::cout
                << "usage: filmviz_ofx_pregenerate"
                << " --resources <dir> --output <dir> [--lut-size 33]\n";
            return false;
        }
        else {
            std::cerr << "unknown argument: " << argument << "\n";
            return false;
        }
    }

    return options.lut_size >= 2;
}

} // namespace

int
main(
    int argc,
    char** argv)
{
    Options options;

    if (!parse_arguments(argc, argv, options)) {
        return 2;
    }

    std::filesystem::create_directories(options.output);

    std::ofstream manifest(
        std::filesystem::path(options.output)
        / "manifest.txt");

    if (!manifest) {
        std::cerr << "could not create cache manifest\n";
        return 1;
    }

    int generated = 0;

    for (int input = 0; input < 2; ++input) {
        for (const auto& negative : NegativeProfileCatalog::profiles()) {
            for (const auto& print : PrintProfileCatalog::profiles()) {
                for (int output = 0; output < 2; ++output) {
                    FilmVizOfxRenderSettings settings;
                    settings.input_profile = input;
                    settings.negative_profile = negative.identifier;
                    settings.print_profile = print.identifier;
                    settings.output_profile = output;
                    settings.lut_size = options.lut_size;
                    settings.exposure_stops = 0.0f;
                    settings.push_pull_stops = 0.0f;
                    settings.color_density = 0.0f;
                    settings.warm_tone_separation =
                        FilmColorResponse::standard_warm_tone_separation;
                    settings.middle_gray = 0.18f;
                    settings.printer_temperature = 3200.0f;
                    settings.negative_bleach_bypass = 0.0f;
                    settings.print_bleach_bypass = 0.0f;
                    settings.printer_light_red = 25.0f;
                    settings.printer_light_green = 25.0f;
                    settings.printer_light_blue = 25.0f;
                    settings.grain_enabled = false;
                    settings.halation_enabled = false;

                    FilmVizOfxProcessor processor;
                    std::string error;

                    std::cout
                        << "prebake input=" << input
                        << " negative=" << negative.identifier
                        << " print=" << print.identifier
                        << " output=" << output
                        << " lut=" << options.lut_size
                        << " ... "
                        << std::flush;

                    if (!processor.configure(
                            settings,
                            options.resources,
                            error)) {
                        std::cerr << "FAILED\n" << error << "\n";
                        return 1;
                    }

                    if (!processor.write_prebaked_cache(
                            options.output,
                            true,
                            error)) {
                        std::cerr << "FAILED\n" << error << "\n";
                        return 1;
                    }

                    const std::string name =
                        processor.transform_cache_name();

                    manifest
                        << name
                        << " input=" << input
                        << " negative=" << negative.identifier
                        << " print=" << print.identifier
                        << " output=" << output
                        << " lut=" << options.lut_size
                        << " warm=" << settings.warm_tone_separation
                        << "\n";

                    ++generated;
                    std::cout << "OK\n";
                }
            }
        }
    }

    std::cout
        << "generated " << generated
        << " FilmViz OFX cache combinations in "
        << options.output
        << "\n";

    return 0;
}
