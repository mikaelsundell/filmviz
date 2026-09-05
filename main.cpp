// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "imageprocessor.h"
#include "inputtransform.h"
#include "lut3d.h"
#include "threading.h"

#include <OpenImageIO/argparse.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/sysutil.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace OIIO;

namespace {

struct FilmVizTool
{
    bool help = false;
    bool verbose = false;
    bool profiles = false;
    bool version = false;
    bool no_validate = false;

    int lut_size = 33;
    int threads = 0;
    float middle_gray = 0.18f;
    float printer_temperature = 3200.0f;
    float exposure_stops = 0.0f;
    float push_pull_stops = 0.0f;
    float negative_grain = 0.0f;
    float print_grain = 0.0f;
    float grain_size = 1.0f;
    float grain_chroma = 1.0f;
    int grain_seed = 1;

    std::string resources;
    std::string input = "awg3-logc3-ei800";
    std::string output = "ap0-linear";
    std::string negative = "verita-200d";
    std::string print = "kodak-2383";
    std::string output_cube = "filmviz.cube";
    std::string input_image;
    std::string output_image = "filmviz_output.tif";
};

FilmVizTool tool;

template <typename T>
void
print_info(
    const std::string& label,
    const T& value)
{
    std::cout
        << "info: "
        << label
        << value
        << "\n";
}

void
print_info(
    const std::string& message)
{
    std::cout
        << "info: "
        << message
        << "\n";
}

template <typename T>
void
print_warning(
    const std::string& label,
    const T& value)
{
    std::cout
        << "warning: "
        << label
        << value
        << "\n";
}

template <typename T>
void
print_error(
    const std::string& label,
    const T& value)
{
    std::cerr
        << "error: "
        << label
        << value
        << "\n";
}

void
print_help(
    ArgParse& ap)
{
    ap.print_help();
}

std::string
default_resources_directory()
{
    const std::filesystem::path executable =
        Sysutil::this_program_path();

    const std::filesystem::path beside_executable =
        executable.parent_path()
        / "resources";

    if (std::filesystem::exists(
            beside_executable)) {

        return
            beside_executable.string();
    }

    const std::filesystem::path cwd =
        std::filesystem::current_path()
        / "resources";

    if (std::filesystem::exists(cwd)) {
        return cwd.string();
    }

    // Keep the expected production layout visible in diagnostics even when
    // resources were deliberately omitted from a source archive.
    return
        beside_executable.string();
}

void
print_profiles()
{
    std::cout
        << "FilmViz production profiles:\n"
        << "  input:\n"
        << "    awg3-logc3-ei800   ARRI Wide Gamut 3 / LogC3 EI800\n"
        << "    ap0-linear         ACES2065-1 AP0 linear\n"
        << "  negative:\n"
        << "    verita-200d        Kodak Verita 200D\n"
        << "  print:\n"
        << "    kodak-2383         Kodak Vision Color Print Film 2383\n"
        << "  output:\n"
        << "    ap0-linear         viewed print as ACES2065-1 AP0 linear\n"
        << "    rec709-gamma24     direct Rec.709/Gamma 2.4 preview (no ACES RRT)\n";
}

bool
validate_profile_options(
    ArgParse& ap,
    InputTransform::Encoding& encoding)
{
    if (!InputTransform::parse_encoding(
            tool.input,
            encoding)) {

        print_error(
            "unknown input profile: ",
            tool.input);

        ap.briefusage();
        return false;
    }

    if (tool.negative != "verita-200d") {
        print_error(
            "unknown negative profile: ",
            tool.negative);

        return false;
    }

    if (tool.print != "kodak-2383") {
        print_error(
            "unknown print profile: ",
            tool.print);

        return false;
    }

    if (tool.output != "ap0-linear"
        && tool.output != "rec709-gamma24") {

        print_error(
            "unknown output profile: ",
            tool.output);

        return false;
    }

    if (tool.lut_size < 2
        || tool.lut_size > 129) {

        print_error(
            "invalid LUT size: ",
            tool.lut_size);

        return false;
    }

    if (tool.threads < 0) {
        print_error(
            "thread count must be zero or positive: ",
            tool.threads);
        return false;
    }

    if (!(tool.middle_gray > 0.0f)) {
        print_error(
            "invalid middle gray: ",
            tool.middle_gray);

        return false;
    }

    if (!(tool.printer_temperature > 0.0f)) {
        print_error(
            "invalid printer temperature: ",
            tool.printer_temperature);

        return false;
    }

    if (tool.negative_grain < 0.0f
        || tool.print_grain < 0.0f) {

        print_error(
            "grain strengths must be non-negative: ",
            tool.negative_grain);
        return false;
    }

    if (tool.grain_size < 1.0f) {
        print_error(
            "grain size must be at least one pixel: ",
            tool.grain_size);
        return false;
    }

    if (tool.grain_chroma < 0.0f) {
        print_error(
            "grain chroma must be non-negative: ",
            tool.grain_chroma);
        return false;
    }

    return true;
}

} // namespace

int
main(
    int argc,
    const char* argv[])
{
    Sysutil::setup_crash_stacktrace("stdout");
    Filesystem::convert_native_arguments(
        argc,
        (const char**)argv);

    tool.resources =
        default_resources_directory();

    ArgParse ap;

    ap.intro(
        "filmviz -- spectral negative + print-film image/LUT processor\n");

    ap.usage(
          "filmviz [options]")
      .add_help(false)
      .exit_on_error(true);

    ap.separator("General flags:");

    ap.arg("--help", &tool.help)
      .help("Print help message");

    ap.arg("-v", &tool.verbose)
      .help("Verbose status messages");

    ap.arg("--profiles", &tool.profiles)
      .help("List supported production profiles and transforms");

    ap.arg("--version", &tool.version)
      .help("Print FilmViz version");

    ap.arg("--transforms", &tool.profiles)
      .help("Alias for --profiles (LogCTool-style compatibility)");

    ap.arg("--resources %s:DIR", &tool.resources)
      .help("resources directory (default: resources beside executable)");

    ap.arg("--threads %d:COUNT", &tool.threads)
      .help("Global worker threads; 0 selects hardware concurrency (default: 0)");

    ap.separator("Pipeline flags:");

    ap.arg("--input %s:PROFILE", &tool.input)
      .help("Input profile: awg3-logc3-ei800 (default), ap0-linear");

    ap.arg("--negative %s:PROFILE", &tool.negative)
      .help("Negative profile: verita-200d (default)");

    ap.arg("--print %s:PROFILE", &tool.print)
      .help("Print profile: kodak-2383 (default)");

    ap.arg("--output %s:PROFILE", &tool.output)
      .help("Output: ap0-linear (default), rec709-gamma24");

    ap.arg("--middlegray %f:VALUE", &tool.middle_gray)
      .help("Scene middle-gray reference (default: 0.18)");

    ap.arg("--printer-temperature %f:KELVIN", &tool.printer_temperature)
      .help("Printer blackbody approximation in kelvin (default: 3200)");

    ap.arg("--exposure %f:STOPS", &tool.exposure_stops)
      .help("Camera exposure adjustment in stops (default: 0)");

    ap.arg("--push-pull %f:STOPS", &tool.push_pull_stops)
      .help("Approximate negative-development push (+) or pull (-) (default: 0)");

    ap.separator("Image processing flags:");

    ap.arg("--input-image %s:FILE", &tool.input_image)
      .help("Process an image instead of writing a .cube LUT");

    ap.arg("--output-image %s:FILE", &tool.output_image)
      .help("Output image filename (default: filmviz_output.tif)");

    ap.arg("--negative-grain %f:STRENGTH", &tool.negative_grain)
      .help("Measured Verita grain strength; 0 disables, 1 is measured RMS");

    ap.arg("--print-grain %f:STRENGTH", &tool.print_grain)
      .help("Measured 2383 grain strength; 0 disables, 1 is measured RMS");

    ap.arg("--grain-size %f:PIXELS", &tool.grain_size)
      .help("Spatial grain correlation size in output pixels (default: 1)");

    ap.arg("--grain-chroma %f:AMOUNT", &tool.grain_chroma)
      .help("Grain chroma: 0 neutral, 1 measured per-channel result (default: 1)");

    ap.arg("--grain-seed %d:SEED", &tool.grain_seed)
      .help("Deterministic grain seed (default: 1)");

    ap.separator("LUT output flags:");

    ap.arg("--lutsize %d:SIZE", &tool.lut_size)
      .help("3D LUT edge length (default: 33)");

    ap.arg("--outputcube %s:FILE", &tool.output_cube)
      .help("Output .cube filename (default: filmviz.cube)");

    ap.arg("--outputfilename %s:FILE", &tool.output_cube)
      .help("Alias for --outputcube (LogCTool-style compatibility)");

    ap.arg("--novalidate", &tool.no_validate)
      .help("Skip the 5x5x5 direct-vs-LUT validation pass");

    if (ap.parse_args(
            argc,
            (const char**)argv) < 0) {

        print_error(
            "could not parse arguments: ",
            ap.geterror());

        print_help(ap);
        ap.abort();
        return EXIT_FAILURE;
    }

    if (tool.help) {
        print_help(ap);
        ap.abort();
        return EXIT_SUCCESS;
    }

    if (tool.version) {
        std::cout << "filmviz 0.1.0\n";
        return EXIT_SUCCESS;
    }

    if (tool.profiles) {
        print_profiles();
        return EXIT_SUCCESS;
    }

    InputTransform::Encoding input_encoding;

    if (!validate_profile_options(
            ap,
            input_encoding)) {

        return EXIT_FAILURE;
    }

    FilmVizThreading::set_thread_count(
        tool.threads);

    print_info(
        "filmviz -- spectral negative + print-film image/LUT processor");

    print_info("resources: ", tool.resources);
    print_info("input: ", tool.input);
    print_info("negative: ", tool.negative);
    print_info("print: ", tool.print);
    print_info("output: ", tool.output);
    print_info("LUT size: ", tool.lut_size);
    print_info("exposure stops: ", tool.exposure_stops);
    print_info("push/pull stops: ", tool.push_pull_stops);
    print_info(
        "threads: ",
        FilmVizThreading::effective_thread_count(
            tool.lut_size));

    if (std::abs(tool.printer_temperature - 3200.0f) > 0.01f) {
        print_warning(
            "non-reference printer temperature; 2383 C/M/Y calibration was validated at 3200 K: ",
            tool.printer_temperature);
    }

    if (std::abs(tool.middle_gray - 0.18f) > 1e-6f) {
        print_warning(
            "non-reference middle gray; Verita zero-stop calibration was validated at 0.18: ",
            tool.middle_gray);
    }

    FilmPipeline::Settings pipeline_settings;

    pipeline_settings.resources_directory =
        tool.resources;

    pipeline_settings.middle_gray =
        tool.middle_gray;

    pipeline_settings.printer_temperature_kelvin =
        tool.printer_temperature;

    pipeline_settings.exposure_stops =
        tool.exposure_stops;

    pipeline_settings.push_pull_stops =
        tool.push_pull_stops;

    FilmPipeline pipeline;

    if (!pipeline.initialize(
            pipeline_settings)) {

        print_error(
            "pipeline initialization failed: ",
            pipeline.error());

        print_error(
            "resources directory: ",
            tool.resources);

        return EXIT_FAILURE;
    }

    InputTransform input_transform(
        input_encoding);

    if (!tool.input_image.empty()) {
        print_info("input image: ", tool.input_image);
        print_info("output image: ", tool.output_image);
        print_info("negative grain: ", tool.negative_grain);
        print_info("print grain: ", tool.print_grain);
        print_info("grain chroma: ", tool.grain_chroma);

        ImageProcessor::Settings image_settings;
        image_settings.lut_size = tool.lut_size;
        image_settings.output =
            tool.output == "rec709-gamma24"
                ? ImageProcessor::Output::Rec709Gamma24
                : ImageProcessor::Output::AP0Linear;
        image_settings.negative_grain_strength =
            tool.negative_grain;
        image_settings.print_grain_strength =
            tool.print_grain;
        image_settings.grain_size_pixels =
            tool.grain_size;
        image_settings.grain_chroma =
            tool.grain_chroma;
        image_settings.grain_seed =
            static_cast<std::uint32_t>(tool.grain_seed);

        int last_percent = -1;
        std::string last_stage;

        const ImageProcessor::Progress image_progress =
            [&](const char* stage,
                int completed,
                int total) {

                if (!tool.verbose || total <= 0) {
                    return;
                }

                const int percent =
                    completed * 100 / total;

                if (last_stage == stage
                    && last_percent == percent) {
                    return;
                }

                if (!last_stage.empty()
                    && last_stage != stage) {
                    std::cout << "\n";
                }

                std::cout
                    << "\rinfo: "
                    << stage
                    << " "
                    << percent
                    << "%"
                    << std::flush;

                last_stage = stage;
                last_percent = percent;

                if (percent == 100) {
                    std::cout << "\n";
                    last_stage.clear();
                }
            };

        ImageProcessor image_processor;

        if (!image_processor.process(
                tool.input_image,
                tool.output_image,
                pipeline,
                input_transform,
                image_settings,
                image_progress)) {

            print_error(
                "image processing failed: ",
                image_processor.error());
            return EXIT_FAILURE;
        }

        print_info(
            "writing output image: ",
            tool.output_image);
        return EXIT_SUCCESS;
    }

    const std::filesystem::path output_path =
        tool.output_cube;

    if (!output_path.parent_path().empty()) {
        std::error_code ec;

        std::filesystem::create_directories(
            output_path.parent_path(),
            ec);

        if (ec) {
            print_error(
                "could not create output directory: ",
                output_path.parent_path().string());

            return EXIT_FAILURE;
        }
    }

    print_info("output cube: ", tool.output_cube);

    Lut3D lut;

    std::array<float, 3> failed_input = {{0.0f, 0.0f, 0.0f}};
    std::atomic<bool> failed(false);
    std::mutex failure_mutex;

    const Lut3D::Evaluator evaluator =
        [&](const Lut3D::RGB& encoded,
            Lut3D::RGB& output) {

            const std::array<float, 3> ap0 =
                input_transform.to_ap0(
                    encoded);

            const FilmPipeline::Result result =
                pipeline.process(
                    ap0);

            if (!result.valid) {
                if (!failed.exchange(true)) {
                    const std::lock_guard<std::mutex> lock(
                        failure_mutex);
                    failed_input = encoded;
                }
                return false;
            }

            output =
                tool.output == "rec709-gamma24"
                    ? result.rec709_gamma24
                    : result.ap0;

            return true;
        };

    const Lut3D::Progress progress =
        [&](int completed,
            int total) {

            if (tool.verbose) {
                const double percent =
                    100.0
                    * static_cast<double>(completed)
                    / static_cast<double>(total);

                std::cout
                    << "\rinfo: LUT generation "
                    << std::fixed
                    << std::setprecision(1)
                    << percent
                    << "%"
                    << std::flush;
            }
        };

    if (!lut.generate(
            tool.lut_size,
            evaluator,
            progress)) {

        if (tool.verbose) {
            std::cout << "\n";
        }

        if (failed.load()) {
            std::cerr
                << "error: spectral evaluation failed at LUT input ("
                << failed_input[0]
                << ", "
                << failed_input[1]
                << ", "
                << failed_input[2]
                << ")\n";
        }
        else {
            print_error(
                "could not generate LUT: ",
                tool.output_cube);
        }

        return EXIT_FAILURE;
    }

    if (tool.verbose) {
        std::cout << "\n";
    }

    std::vector<std::string> comments = {
        "FilmViz production LUT",
        std::string("Input: ") + tool.input,
        std::string("Negative: ") + tool.negative + " / ISO Status-M density calibration",
        std::string("Exposure stops: ") + std::to_string(tool.exposure_stops),
        std::string("Push/pull stops: ") + std::to_string(tool.push_pull_stops) + " / approximate contrast",
        std::string("Print: ") + tool.print + " / printer K=" + std::to_string(tool.printer_temperature),
        std::string("Output: ") + tool.output,
        "No ACES RRT/ODT is applied by FilmViz"
    };

    if (!lut.write_cube(
            tool.output_cube,
            "FilmViz Verita 200D to Kodak 2383",
            comments)) {

        print_error(
            "could not write cube: ",
            tool.output_cube);

        return EXIT_FAILURE;
    }

    if (!tool.no_validate) {
        const Lut3D::Validation validation =
            lut.validate(
                evaluator,
                5);

        if (!validation.valid) {
            print_error(
                "LUT validation failed: ",
                tool.output_cube);

            return EXIT_FAILURE;
        }

        std::cout
            << "info: validation samples: "
            << validation.samples
            << "\n"
            << "info: mean/max absolute error: "
            << validation.mean_abs_error
            << " / "
            << validation.max_abs_error
            << "\n";
    }

    print_info(
        "writing output cube: ",
        tool.output_cube);

    return EXIT_SUCCESS;
}
