// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "imageprocessor.h"
#include "inputtransform.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace
{

void
report_progress(
    const char* stage,
    int completed,
    int total,
    std::string& previous_stage,
    int& previous_percent)
{
    if (total <= 0) {
        return;
    }

    const int percent = completed * 100 / total;

    if (previous_stage == stage
        && previous_percent == percent) {
        return;
    }

    std::cout
        << "\r"
        << stage
        << ": "
        << std::setw(3)
        << percent
        << "%"
        << std::flush;

    previous_stage = stage;
    previous_percent = percent;

    if (percent == 100) {
        std::cout << "\n";
    }
}

float
argument_float(
    int argc,
    const char* argv[],
    int index,
    float fallback)
{
    return argc > index
        ? std::stof(argv[index])
        : fallback;
}

} // namespace

int
main(
    int argc,
    const char* argv[])
{
    try {
        const std::filesystem::path resources =
            argc > 1 ? argv[1] : "resources";

        const std::filesystem::path input_filename =
            argc > 2
                ? argv[2]
                : resources
                    / "references/images/ARRI_Helen_John_ALEXA_Mini_LF_AWG3_LogC3.tif";

        const std::filesystem::path output_filename =
            argc > 3
                ? argv[3]
                : "build/ARRI_Helen_John_filmviz_rec709_gamma24_grain.tif";

        const int lut_size =
            argc > 4 ? std::stoi(argv[4]) : 33;
        const float exposure_stops =
            argument_float(argc, argv, 5, 0.0f);
        const float push_pull_stops =
            argument_float(argc, argv, 6, 0.0f);
        const float negative_grain =
            argument_float(argc, argv, 7, 1.0f);
        const float print_grain =
            argument_float(argc, argv, 8, 1.0f);
        const float grain_size =
            argument_float(argc, argv, 9, 1.0f);
        const int grain_seed =
            argc > 10 ? std::stoi(argv[10]) : 1;
        const float grain_chroma =
            argument_float(argc, argv, 11, 1.0f);
        const float negative_bleach_bypass =
            argument_float(argc, argv, 12, 0.0f);
        const float print_bleach_bypass =
            argument_float(argc, argv, 13, 0.0f);
        const float printer_light_red =
            argument_float(argc, argv, 14, 25.0f);
        const float printer_light_green =
            argument_float(argc, argv, 15, 25.0f);
        const float printer_light_blue =
            argument_float(argc, argv, 16, 25.0f);
        const float halation_strength =
            argument_float(argc, argv, 17, 0.0f);
        const float halation_radius =
            argument_float(argc, argv, 18, 12.0f);
        const float halation_threshold =
            argument_float(argc, argv, 19, 0.7f);

        FilmPipeline::Settings pipeline_settings;
        pipeline_settings.resources_directory = resources.string();
        pipeline_settings.exposure_stops = exposure_stops;
        pipeline_settings.push_pull_stops = push_pull_stops;
        pipeline_settings.negative_bleach_bypass = negative_bleach_bypass;
        pipeline_settings.print_bleach_bypass = print_bleach_bypass;
        pipeline_settings.printer_light_red = printer_light_red;
        pipeline_settings.printer_light_green = printer_light_green;
        pipeline_settings.printer_light_blue = printer_light_blue;

        FilmPipeline pipeline;

        if (!pipeline.initialize(pipeline_settings)) {
            std::cerr
                << "Could not initialize FilmViz: "
                << pipeline.error()
                << "\n";
            return EXIT_FAILURE;
        }

        const InputTransform input_transform(
            InputTransform::Encoding::AWG3_LogC3_EI800);

        ImageProcessor::Settings image_settings;
        image_settings.lut_size = lut_size;
        image_settings.output =
            ImageProcessor::Output::Rec709Gamma24;
        image_settings.negative_grain_strength = negative_grain;
        image_settings.print_grain_strength = print_grain;
        image_settings.grain_size_pixels = grain_size;
        image_settings.grain_chroma = grain_chroma;
        image_settings.grain_seed =
            static_cast<std::uint32_t>(grain_seed);
        image_settings.halation_strength = halation_strength;
        image_settings.halation_radius_pixels = halation_radius;
        image_settings.halation_threshold = halation_threshold;

        std::cout
            << "AWG3/LogC3 -> FilmViz -> Rec.709/Gamma 2.4\n"
            << "Exposure: " << exposure_stops << " stops\n"
            << "Push/pull: " << push_pull_stops << " stops\n"
            << "Negative/print grain: "
            << negative_grain << " / " << print_grain << "\n"
            << "Grain chroma: " << grain_chroma << "\n"
            << "Negative/print bleach bypass: "
            << negative_bleach_bypass << " / " << print_bleach_bypass << "\n"
            << "Printer lights R/G/B: "
            << printer_light_red << " / "
            << printer_light_green << " / "
            << printer_light_blue << "\n"
            << "Halation strength/radius/threshold: "
            << halation_strength << " / "
            << halation_radius << " / "
            << halation_threshold << "\n";

        std::string previous_stage;
        int previous_percent = -1;
        ImageProcessor image_processor;

        if (!image_processor.process(
                input_filename.string(),
                output_filename.string(),
                pipeline,
                input_transform,
                image_settings,
                [&](const char* stage,
                    int completed,
                    int total) {

                    report_progress(
                        stage,
                        completed,
                        total,
                        previous_stage,
                        previous_percent);
                })) {

            std::cerr
                << "Image conversion failed: "
                << image_processor.error()
                << "\n";
            return EXIT_FAILURE;
        }

        std::cout
            << "Wrote \""
            << output_filename.string()
            << "\"\n";
    }
    catch (const std::exception& error) {
        std::cerr
            << "Invalid argument: "
            << error.what()
            << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
