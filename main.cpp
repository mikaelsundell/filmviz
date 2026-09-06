// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "filmformat.h"
#include "imageprocessor.h"
#include "inputtransform.h"
#include "lut3d.h"
#include "negativeprofile.h"
#include "printprofile.h"
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
    float negative_flash = 0.0f;
    float print_flash = 0.0f;
    float push_pull_stops = 0.0f;
    float negative_bleach_bypass = 0.0f;
    float print_bleach_bypass = 0.0f;
    float printer_light_red = 25.0f;
    float printer_light_green = 25.0f;
    float printer_light_blue = 25.0f;
    float printer_light_master = 0.0f;
    float negative_grain = 0.0f;
    float print_grain = 0.0f;
    float grain_size = 1.0f;
    float grain_chroma = 1.0f;
    float image_width_mm = 0.0f;
    float negative_mtf = 0.0f;
    float print_mtf = 0.0f;
    float halation_strength = 0.0f;
    float halation_radius = 12.0f;
    float halation_threshold = 0.7f;
    int grain_seed = 1;

    std::string resources;
    std::string input = "awg3-logc3-ei800";
    std::string output = "ap0-linear";
    std::string film_format =
        FilmFormatCatalog::default_format().identifier;
    std::string negative =
        NegativeProfileCatalog::default_profile().identifier;
    std::string print =
        PrintProfileCatalog::default_profile().identifier;
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
        << "  negative:\n";

    for (const auto& profile : NegativeProfileCatalog::profiles()) {
        std::cout
            << "    "
            << std::left
            << std::setw(20)
            << profile.identifier
            << profile.display_name
            << "\n";
    }

    std::cout
        << "  print:\n";

    for (const auto& profile : PrintProfileCatalog::profiles()) {
        std::cout
            << "    "
            << std::left
            << std::setw(20)
            << profile.identifier
            << profile.display_name
            << "\n";
    }

    std::cout
        << "    "
        << std::left
        << std::setw(20)
        << "none"
        << "View negative without print film\n"
        << "  film format:\n";

    for (const auto& format : FilmFormatCatalog::formats()) {
        std::cout
            << "    "
            << std::left
            << std::setw(20)
            << format.identifier
            << format.display_name
            << " (" << format.image_width_mm << " mm)\n";
    }

    std::cout
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

    if (!NegativeProfileCatalog::find(tool.negative)) {
        print_error(
            "unknown negative profile: ",
            tool.negative);

        return false;
    }

    if (!PrintProfileCatalog::find(tool.print)
        && tool.print != "none") {
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

    if (tool.negative_bleach_bypass < 0.0f
        || tool.negative_bleach_bypass > 1.0f
        || tool.print_bleach_bypass < 0.0f
        || tool.print_bleach_bypass > 1.0f) {

        print_error(
            "bleach-bypass controls must be in [0,1]: ",
            tool.negative_bleach_bypass);
        return false;
    }

    if (tool.negative_flash < 0.0f
        || tool.negative_flash > 25.0f
        || tool.print_flash < 0.0f
        || tool.print_flash > 25.0f) {
        print_error(
            "flash percentages must be in [0,25]: ",
            tool.negative_flash);
        return false;
    }

    if (tool.printer_light_red + tool.printer_light_master < 0.0f
        || tool.printer_light_red + tool.printer_light_master > 50.0f
        || tool.printer_light_green + tool.printer_light_master < 0.0f
        || tool.printer_light_green + tool.printer_light_master > 50.0f
        || tool.printer_light_blue + tool.printer_light_master < 0.0f
        || tool.printer_light_blue + tool.printer_light_master > 50.0f) {

        print_error(
            "printer lights must be in [0,50]: ",
            tool.printer_light_red);
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

    const FilmFormatCatalog::Format* film_format =
        FilmFormatCatalog::find(tool.film_format);

    if (!film_format) {
        print_error("unknown film format: ", tool.film_format);
        return false;
    }

    if (tool.image_width_mm <= 0.0f) {
        tool.image_width_mm = film_format->image_width_mm;
    }

    if (tool.negative_mtf < 0.0f
        || tool.negative_mtf > 2.0f
        || tool.print_mtf < 0.0f
        || tool.print_mtf > 2.0f) {
        print_error("MTF amounts must be in [0,2]: ", tool.negative_mtf);
        return false;
    }

    if (tool.input_image.empty()
        && (tool.negative_mtf > 0.0f
            || tool.print_mtf > 0.0f)) {
        print_error(
            "MTF is spatial and requires --input-image; "
            "it cannot be stored in a .cube LUT",
            "");
        return false;
    }

    if (tool.halation_strength < 0.0f
        || tool.halation_strength > 1.0f
        || tool.halation_radius < 0.0f
        || tool.halation_threshold < 0.0f) {

        print_error(
            "invalid halation settings: ",
            tool.halation_strength);
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
      .help("Negative profile identifier; use --profiles to list choices");

    ap.arg("--print %s:PROFILE", &tool.print)
      .help("Print profile identifier; use --profiles to list choices");

    ap.arg("--output %s:PROFILE", &tool.output)
      .help("Output: ap0-linear (default), rec709-gamma24");

    ap.arg("--middlegray %f:VALUE", &tool.middle_gray)
      .help("Scene middle-gray reference (default: 0.18)");

    ap.arg("--printer-temperature %f:KELVIN", &tool.printer_temperature)
      .help("Printer blackbody approximation in kelvin (default: 3200)");

    ap.arg("--exposure %f:STOPS", &tool.exposure_stops)
      .help("Camera exposure adjustment in stops (default: 0)");

    ap.arg("--negative-flash %f:PERCENT", &tool.negative_flash)
      .help("Neutral negative preflash as percent of middle-gray exposure");

    ap.arg("--print-flash %f:PERCENT", &tool.print_flash)
      .help("Neutral print flash as percent of reference printer exposure");

    ap.arg("--push-pull %f:STOPS", &tool.push_pull_stops)
      .help("Approximate negative-development push (+) or pull (-) (default: 0)");

    ap.arg("--negative-bleach-bypass %f:AMOUNT", &tool.negative_bleach_bypass)
      .help("Negative bleach bypass; 0 normal, 1 full modeled bypass (default: 0)");

    ap.arg("--print-bleach-bypass %f:AMOUNT", &tool.print_bleach_bypass)
      .help("Print bleach bypass; 0 normal, 1 full modeled bypass (default: 0)");

    ap.arg("--printer-light-r %f:POINTS", &tool.printer_light_red)
      .help("Red printer light on 0-50 scale; 25 is neutral (default: 25)");

    ap.arg("--printer-light-g %f:POINTS", &tool.printer_light_green)
      .help("Green printer light on 0-50 scale; 25 is neutral (default: 25)");

    ap.arg("--printer-light-b %f:POINTS", &tool.printer_light_blue)
      .help("Blue printer light on 0-50 scale; 25 is neutral (default: 25)");

    ap.arg("--printer-light-master %f:POINTS", &tool.printer_light_master)
      .help("Linked offset added to all three printer lights (default: 0)");

    ap.separator("Image processing flags:");

    ap.arg("--input-image %s:FILE", &tool.input_image)
      .help("Process an image instead of writing a .cube LUT");

    ap.arg("--output-image %s:FILE", &tool.output_image)
      .help("Output image filename (default: filmviz_output.tif)");

    ap.arg("--negative-grain %f:STRENGTH", &tool.negative_grain)
      .help("Measured negative-stock grain strength; 0 disables, 1 is measured RMS");

    ap.arg("--print-grain %f:STRENGTH", &tool.print_grain)
      .help("Measured print-stock grain strength; 0 disables, 1 is measured RMS");

    ap.arg("--grain-size %f:PIXELS", &tool.grain_size)
      .help("Artistic grain spatial scale in output pixels (default: 1)");

    ap.arg("--grain-chroma %f:AMOUNT", &tool.grain_chroma)
      .help("Grain chroma: 0 neutral, 1 measured per-channel result (default: 1)");

    ap.arg("--grain-seed %d:SEED", &tool.grain_seed)
      .help("Deterministic grain seed (default: 1)");

    ap.arg("--film-format %s:FORMAT", &tool.film_format)
      .help("regular-8, super-8, 16mm, super-16, 35mm, super-35, 65mm, or custom");

    ap.arg("--image-width-mm %f:MM", &tool.image_width_mm)
      .help("Override active film-image width used for cycles/mm MTF mapping");

    ap.arg("--negative-mtf %f:AMOUNT", &tool.negative_mtf)
      .help("Measured negative MTF: 0 bypass, 1 measured, 2 exaggerated");

    ap.arg("--print-mtf %f:AMOUNT", &tool.print_mtf)
      .help("Measured print MTF: 0 bypass, 1 measured, 2 exaggerated");

    ap.arg("--halation %f:STRENGTH", &tool.halation_strength)
      .help("Image-space halation strength; 0 disables, 1 full effect (default: 0)");

    ap.arg("--halation-radius %f:PIXELS", &tool.halation_radius)
      .help("Halation blur radius in pixels (default: 12)");

    ap.arg("--halation-threshold %f:AP0", &tool.halation_threshold)
      .help("Scene-linear AP0 luminance threshold for halation (default: 0.7)");

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
    print_info(
        "negative: ",
        NegativeProfileCatalog::find(
            tool.negative)->display_name);
    print_info(
        "print: ",
        tool.print == "none"
            ? std::string("None — view negative")
            : PrintProfileCatalog::find(
                tool.print)->display_name);
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
            "non-reference printer temperature; print C/M/Y calibration was validated at 3200 K: ",
            tool.printer_temperature);
    }

    if (std::abs(tool.middle_gray - 0.18f) > 1e-6f) {
        print_warning(
            "non-reference middle gray; negative zero-stop calibration uses 0.18: ",
            tool.middle_gray);
    }

    FilmPipeline::Settings pipeline_settings;

    pipeline_settings.resources_directory =
        tool.resources;

    pipeline_settings.negative_profile =
        tool.negative;

    pipeline_settings.print_profile =
        tool.print;

    pipeline_settings.middle_gray =
        tool.middle_gray;

    pipeline_settings.printer_temperature_kelvin =
        tool.printer_temperature;

    pipeline_settings.exposure_stops =
        tool.exposure_stops;

    pipeline_settings.negative_flash_percent =
        tool.negative_flash;

    pipeline_settings.print_flash_percent =
        tool.print_flash;

    pipeline_settings.push_pull_stops =
        tool.push_pull_stops;

    pipeline_settings.negative_bleach_bypass =
        tool.negative_bleach_bypass;

    pipeline_settings.print_bleach_bypass =
        tool.print_bleach_bypass;

    pipeline_settings.printer_light_red =
        tool.printer_light_red;

    pipeline_settings.printer_light_green =
        tool.printer_light_green;

    pipeline_settings.printer_light_blue =
        tool.printer_light_blue;

    pipeline_settings.printer_light_master =
        tool.printer_light_master;

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
        print_info("film format: ", tool.film_format);
        print_info("active image width mm: ", tool.image_width_mm);
        print_info("negative MTF: ", tool.negative_mtf);
        print_info("print MTF: ", tool.print_mtf);
        print_info("halation: ", tool.halation_strength);
        print_info("halation radius: ", tool.halation_radius);
        print_info("halation threshold: ", tool.halation_threshold);

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
        image_settings.film_format =
            tool.film_format;
        image_settings.image_width_mm =
            tool.image_width_mm;
        image_settings.negative_mtf_amount =
            tool.negative_mtf;
        image_settings.print_mtf_amount =
            tool.print_mtf;
        image_settings.halation_strength =
            tool.halation_strength;
        image_settings.halation_radius_pixels =
            tool.halation_radius;
        image_settings.halation_threshold =
            tool.halation_threshold;

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
        std::string("Negative bleach bypass: ") + std::to_string(tool.negative_bleach_bypass),
        std::string("Print bleach bypass: ") + std::to_string(tool.print_bleach_bypass),
        std::string("Printer lights R/G/B: ")
            + std::to_string(tool.printer_light_red) + "/"
            + std::to_string(tool.printer_light_green) + "/"
            + std::to_string(tool.printer_light_blue),
        std::string("Print: ") + tool.print + " / printer K=" + std::to_string(tool.printer_temperature),
        std::string("Output: ") + tool.output,
        "No ACES RRT/ODT is applied by FilmViz"
    };

    if (!lut.write_cube(
            tool.output_cube,
            std::string("FilmViz ") + tool.negative + " to " + tool.print,
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
