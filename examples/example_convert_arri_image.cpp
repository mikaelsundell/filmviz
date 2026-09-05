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

        FilmPipeline::Settings pipeline_settings;
        pipeline_settings.resources_directory = resources.string();
        pipeline_settings.exposure_stops = exposure_stops;
        pipeline_settings.push_pull_stops = push_pull_stops;

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

        std::cout
            << "AWG3/LogC3 -> FilmViz -> Rec.709/Gamma 2.4\n"
            << "Exposure: " << exposure_stops << " stops\n"
            << "Push/pull: " << push_pull_stops << " stops\n"
            << "Negative/print grain: "
            << negative_grain << " / " << print_grain << "\n"
            << "Grain chroma: " << grain_chroma << "\n";

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
