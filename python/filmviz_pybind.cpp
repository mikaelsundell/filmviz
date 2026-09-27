// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "colorprofilecatalog.h"
#include "filmpipeline.h"
#include "filmformat.h"
#include "imageprocessor.h"
#include "inputtransform.h"
#include "lut3d.h"
#include "negativeprofile.h"
#include "printprofile.h"
#include "threading.h"

#if defined(FILMVIZ_PYTHON_HAS_METAL)
#include "filmviz_metal_preview.h"
#endif

#include <OpenImageIO/imagebuf.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

InputTransform::Encoding
input_encoding(
    const std::string& name,
    const std::string& resources = "")
{
    InputTransform::Encoding encoding;

    if (!InputTransform::parse_encoding(name, encoding, resources)) {
        throw std::invalid_argument(
            "unknown input profile: " + name);
    }

    return encoding;
}

ImageProcessor::Output
image_output(const std::string& name, const std::string& resources)
{
    OutputTransform::Encoding encoding;
    if (!OutputTransform::parse_encoding(name, encoding, resources))
        throw std::invalid_argument("unknown output profile: " + name);
    return encoding;
}

void
validate_stock_profiles(
    const std::string& negative,
    const std::string& print)
{
    if (!NegativeProfileCatalog::find(negative)) {
        throw std::invalid_argument(
            "unknown negative profile: " + negative);
    }

    if (!PrintProfileCatalog::find(print)
        && print != "none") {
        throw std::invalid_argument(
            "unknown print profile: " + print);
    }
}

FilmColorResponse::Tuning
color_response_tuning(const py::dict& values)
{
    FilmColorResponse::Tuning tuning;
    for (const auto& item : values) {
        const auto key = py::cast<std::string>(item.first);
        if (key == "response_amount") tuning.response_amount = py::cast<float>(item.second);
        else if (key == "chroma_compression") tuning.chroma_compression = py::cast<float>(item.second);
        else if (key == "chroma_knee") tuning.chroma_knee = py::cast<float>(item.second);
        else if (key == "density_center") tuning.density_center = py::cast<float>(item.second);
        else if (key == "density_width") tuning.density_width = py::cast<float>(item.second);
        else if (key == "warm_protection") tuning.warm_protection = py::cast<float>(item.second);
        else if (key == "warm_hue_center") tuning.warm_hue_center = py::cast<float>(item.second);
        else if (key == "warm_hue_width") tuning.warm_hue_width = py::cast<float>(item.second);
        else if (key == "warm_hue_shift") tuning.warm_hue_shift = py::cast<float>(item.second);
        else throw std::invalid_argument("Unknown color response parameter: " + key);
    }
    if (!FilmColorResponse::valid_tuning(tuning)) throw std::invalid_argument("Color response parameter outside its supported range");
    return tuning;
}

FilmPipeline::Settings
pipeline_settings(
    const std::string& resources,
    const std::string& negative,
    const std::string& print,
    float exposure,
    float negative_flash,
    float print_flash,
    float push_pull,
    float color_density,
    float color_depth,
    float negative_bleach_bypass,
    float print_bleach_bypass,
    float printer_light_red,
    float printer_light_green,
    float printer_light_blue,
    float printer_light_master,
    float middle_gray,
    float printer_temperature,
    const py::dict& color_response)
{
    FilmPipeline::Settings settings;
    settings.resources_directory = resources;
    settings.negative_profile = negative;
    settings.print_profile = print;
    settings.exposure_stops = exposure;
    settings.negative_flash_percent = negative_flash;
    settings.print_flash_percent = print_flash;
    settings.push_pull_stops = push_pull;
    settings.color_density = color_density;
    settings.color_depth = color_depth;
    settings.color_response = color_response_tuning(color_response);
    settings.negative_bleach_bypass = negative_bleach_bypass;
    settings.print_bleach_bypass = print_bleach_bypass;
    settings.printer_light_red = printer_light_red;
    settings.printer_light_green = printer_light_green;
    settings.printer_light_blue = printer_light_blue;
    settings.printer_light_master = printer_light_master;
    settings.middle_gray = middle_gray;
    settings.printer_temperature_kelvin = printer_temperature;
    return settings;
}

void
report_progress(
    const py::object& callback,
    bool enabled,
    const char* stage,
    int completed,
    int total)
{
    if (!enabled) {
        return;
    }

    py::gil_scoped_acquire acquire;

    try {
        callback(stage, completed, total);
    }
    catch (py::error_already_set& error) {
        error.discard_as_unraisable("FilmViz progress callback");
    }
}

bool
is_cancelled(
    const py::object& callback,
    bool enabled)
{
    if (!enabled) {
        return false;
    }

    py::gil_scoped_acquire acquire;

    try {
        return py::cast<bool>(callback());
    }
    catch (py::error_already_set& error) {
        error.discard_as_unraisable("FilmViz cancellation callback");
        return false;
    }
}

void
generate_lut(
    const std::string& resources,
    const std::string& output_filename,
    const std::string& input,
    const std::string& negative,
    const std::string& print,
    const std::string& output,
    int lut_size,
    float exposure,
    float negative_flash,
    float print_flash,
    float push_pull,
    float color_density,
    float color_depth,
    float negative_bleach_bypass,
    float print_bleach_bypass,
    float printer_light_red,
    float printer_light_green,
    float printer_light_blue,
    float printer_light_master,
    float middle_gray,
    float printer_temperature,
    int threads,
    const py::object& progress,
    const py::object& cancel,
    const py::dict& color_response)
{
    validate_stock_profiles(negative, print);

    if (lut_size < 2 || lut_size > 129) {
        throw std::invalid_argument("LUT size must be between 2 and 129");
    }

    const InputTransform::Encoding encoding =
        input_encoding(input, resources);
    image_output(output, resources);
    FilmVizThreading::set_thread_count(threads);

    FilmPipeline pipeline;

    if (!pipeline.initialize(
            pipeline_settings(
                resources,
                negative,
                print,
                exposure,
                negative_flash,
                print_flash,
                push_pull,
                color_density,
                color_depth,
                negative_bleach_bypass,
                print_bleach_bypass,
                printer_light_red,
                printer_light_green,
                printer_light_blue,
                printer_light_master,
                middle_gray,
                printer_temperature, color_response))) {

        throw std::runtime_error(
            "could not initialize FilmViz: "
            + pipeline.error());
    }

    const InputTransform transform(encoding, resources);
    const OutputTransform output_transform(image_output(output, resources), resources);
    Lut3D lut;
    const bool has_progress = !progress.is_none();
    const bool has_cancel = !cancel.is_none();
    std::atomic<bool> cancel_requested(false);
    bool generated = false;

    {
        py::gil_scoped_release release;
        generated =
            lut.generate(
                lut_size,
                [&](const Lut3D::RGB& encoded,
                    Lut3D::RGB& converted) {

                    if (cancel_requested.load(
                            std::memory_order_relaxed)) {
                        return false;
                    }

                    const FilmPipeline::Result result =
                        pipeline.process(
                            transform.to_ap0(encoded));

                    if (!result.valid) {
                        return false;
                    }

                    converted =
                        output_transform.from_ap0(result.ap0);
                    return true;
                },
                [&](int completed,
                    int total) {

                    if (is_cancelled(
                            cancel,
                            has_cancel)) {

                        cancel_requested.store(
                            true,
                            std::memory_order_relaxed);
                        return;
                    }

                    report_progress(
                        progress,
                        has_progress,
                        "LUT creation",
                        completed,
                        total);
                });
    }

    if (!generated) {
        throw std::runtime_error(
            cancel_requested.load(
                std::memory_order_relaxed)
                ? "operation cancelled"
                : "spectral LUT generation failed");
    }

    const std::filesystem::path output_path(output_filename);

    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(
            output_path.parent_path());
    }

    const std::vector<std::string> comments = {
        "FilmViz production LUT",
        "Input: " + input,
        "Negative: " + negative + " / ISO Status-M density calibration",
        "Exposure stops: " + std::to_string(exposure),
        "Negative flash percent: " + std::to_string(negative_flash),
        "Print flash percent: " + std::to_string(print_flash),
        "Push/pull stops: " + std::to_string(push_pull) + " / approximate contrast",
        "Color separation trim: " + std::to_string(color_density),
        "Color depth: " + std::to_string(color_depth),
        "Color response tuning: " + py::cast<std::string>(py::str(color_response)),
        "Negative bleach bypass: " + std::to_string(negative_bleach_bypass),
        "Print bleach bypass: " + std::to_string(print_bleach_bypass),
        "Printer lights R/G/B: "
            + std::to_string(printer_light_red) + " / "
            + std::to_string(printer_light_green) + " / "
            + std::to_string(printer_light_blue),
        "Printer light master: " + std::to_string(printer_light_master),
        "Print: " + print + " / printer K=" + std::to_string(printer_temperature),
        "Output: " + output,
        "No ACES RRT/ODT is applied by FilmViz"
    };

    if (!lut.write_cube(
            output_filename,
            "FilmViz " + negative + " to " + print,
            comments)) {

        throw std::runtime_error(
            "could not write LUT: " + output_filename);
    }
}

void
process_image(
    const std::string& resources,
    const std::string& input_filename,
    const std::string& output_filename,
    const std::string& input,
    const std::string& negative,
    const std::string& print,
    const std::string& output,
    int lut_size,
    bool use_lut_acceleration,
    float exposure,
    float negative_flash,
    float print_flash,
    float push_pull,
    float color_density,
    float color_depth,
    float negative_bleach_bypass,
    float print_bleach_bypass,
    float printer_light_red,
    float printer_light_green,
    float printer_light_blue,
    float printer_light_master,
    float middle_gray,
    float printer_temperature,
    float negative_grain,
    float print_grain,
    float grain_size,
    float grain_chroma,
    unsigned int grain_seed,
    const std::string& film_format,
    float image_width_mm,
    float negative_mtf,
    float print_mtf,
    float halation_strength,
    float halation_radius,
    float halation_threshold,
    int threads,
    const py::object& progress,
    const py::object& cancel,
    const py::dict& color_response,
    bool grain_tonal_enabled, float grain_shadows, float grain_midtones, float grain_highlights)
{
    validate_stock_profiles(negative, print);
    const InputTransform::Encoding encoding = input_encoding(input, resources);
    const ImageProcessor::Output output_encoding = image_output(output, resources);
    FilmVizThreading::set_thread_count(threads);

    FilmPipeline pipeline;

    if (!pipeline.initialize(
            pipeline_settings(
                resources,
                negative,
                print,
                exposure,
                negative_flash,
                print_flash,
                push_pull,
                color_density,
                color_depth,
                negative_bleach_bypass,
                print_bleach_bypass,
                printer_light_red,
                printer_light_green,
                printer_light_blue,
                printer_light_master,
                middle_gray,
                printer_temperature, color_response))) {

        throw std::runtime_error(
            "could not initialize FilmViz: "
            + pipeline.error());
    }

    ImageProcessor::Settings settings;
    settings.lut_size = lut_size;
    settings.use_lut_acceleration = use_lut_acceleration;
    settings.output = output_encoding;
    settings.negative_grain_strength = negative_grain;
    settings.print_grain_strength = print_grain;
    settings.grain_size_pixels = grain_size;
    settings.grain_chroma = grain_chroma;
    settings.grain_tonal_enabled = grain_tonal_enabled;
    settings.grain_shadows = grain_shadows;
    settings.grain_midtones = grain_midtones;
    settings.grain_highlights = grain_highlights;
    settings.grain_seed = grain_seed;
    settings.film_format = film_format;
    settings.image_width_mm = image_width_mm;
    settings.negative_mtf_amount = negative_mtf;
    settings.print_mtf_amount = print_mtf;
    settings.halation_strength = halation_strength;
    settings.halation_radius_pixels = halation_radius;
    settings.halation_threshold = halation_threshold;

    const InputTransform transform(encoding, resources);
    ImageProcessor processor;
    const bool has_progress = !progress.is_none();
    const bool has_cancel = !cancel.is_none();
    bool processed = false;

    {
        py::gil_scoped_release release;
        processed =
            processor.process(
                input_filename,
                output_filename,
                pipeline,
                transform,
                settings,
                [&](const char* stage,
                    int completed,
                    int total) {

                    report_progress(
                        progress,
                        has_progress,
                        stage,
                        completed,
                        total);
                },
                [&]() {
                    return
                        is_cancelled(
                            cancel,
                            has_cancel);
                });
    }

    if (!processed) {
        throw std::runtime_error(
            "image processing failed: "
            + processor.error());
    }
}


py::dict
read_image_preview(
    const std::string& filename,
    int max_dimension)
{
    OIIO::ImageBuf image(filename);

    if (!image.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        throw std::runtime_error(
            "could not read preview image: "
            + image.geterror());
    }

    const OIIO::ImageSpec& spec = image.spec();

    if (spec.nchannels < 3
        || spec.width <= 0
        || spec.height <= 0) {

        throw std::runtime_error(
            "preview image must contain RGB channels");
    }

    const int limit = std::max(64, max_dimension);
    const float scale =
        std::min(
            1.0f,
            static_cast<float>(limit)
                / static_cast<float>(
                    std::max(spec.width, spec.height)));

    const int width =
        std::max(
            1,
            static_cast<int>(
                std::lround(spec.width * scale)));

    const int height =
        std::max(
            1,
            static_cast<int>(
                std::lround(spec.height * scale)));

    std::vector<float> source(
        static_cast<std::size_t>(spec.width)
        * static_cast<std::size_t>(spec.height)
        * static_cast<std::size_t>(spec.nchannels));

    if (!image.get_pixels(
            image.roi(),
            OIIO::TypeDesc::FLOAT,
            source.data())) {

        throw std::runtime_error(
            "could not read preview pixels: "
            + image.geterror());
    }

    std::vector<std::uint8_t> rgb(
        static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height)
        * 3u);

    for (int y = 0; y < height; ++y) {
        const int source_y =
            std::min(
                spec.height - 1,
                static_cast<int>(
                    static_cast<double>(y)
                    * static_cast<double>(spec.height)
                    / static_cast<double>(height)));

        for (int x = 0; x < width; ++x) {
            const int source_x =
                std::min(
                    spec.width - 1,
                    static_cast<int>(
                        static_cast<double>(x)
                        * static_cast<double>(spec.width)
                        / static_cast<double>(width)));

            const std::size_t source_offset =
                (static_cast<std::size_t>(source_y)
                    * static_cast<std::size_t>(spec.width)
                    + static_cast<std::size_t>(source_x))
                * static_cast<std::size_t>(spec.nchannels);

            const std::size_t destination_offset =
                (static_cast<std::size_t>(y)
                    * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(x))
                * 3u;

            for (int channel = 0; channel < 3; ++channel) {
                const float value =
                    std::clamp(
                        source[source_offset
                            + static_cast<std::size_t>(channel)],
                        0.0f,
                        1.0f);

                rgb[destination_offset
                    + static_cast<std::size_t>(channel)] =
                    static_cast<std::uint8_t>(
                        std::lround(value * 255.0f));
            }
        }
    }

    py::dict result;
    result["width"] = width;
    result["height"] = height;
    result["rgb"] =
        py::bytes(
            reinterpret_cast<const char*>(rgb.data()),
            rgb.size());
    return result;
}


py::dict
probe_image_pixel(
    const std::string& resources,
    const std::string& input_filename,
    const std::string& input,
    const std::string& negative,
    const std::string& print,
    float exposure,
    float negative_flash,
    float print_flash,
    float push_pull,
    float color_density,
    float color_depth,
    float negative_bleach_bypass,
    float print_bleach_bypass,
    float printer_light_red,
    float printer_light_green,
    float printer_light_blue,
    float printer_light_master,
    float middle_gray,
    float printer_temperature,
    double u,
    double v,
    const py::dict& color_response)
{
    validate_stock_profiles(
        negative,
        print);

    OIIO::ImageBuf image(
        input_filename);

    if (!image.read(
            0,
            0,
            true,
            OIIO::TypeDesc::FLOAT)) {

        throw std::runtime_error(
            "could not read probe image: "
            + image.geterror());
    }

    const OIIO::ImageSpec& spec =
        image.spec();

    if (spec.width <= 0
        || spec.height <= 0
        || spec.nchannels < 3) {

        throw std::runtime_error(
            "probe image must contain RGB channels");
    }

    const double clamped_u =
        std::clamp(
            u,
            0.0,
            1.0);

    const double clamped_v =
        std::clamp(
            v,
            0.0,
            1.0);

    const int x =
        std::clamp(
            static_cast<int>(
                std::lround(
                    clamped_u
                    * static_cast<double>(
                        spec.width - 1))),
            0,
            spec.width - 1);

    const int y =
        std::clamp(
            static_cast<int>(
                std::lround(
                    clamped_v
                    * static_cast<double>(
                        spec.height - 1))),
            0,
            spec.height - 1);

    std::vector<float> pixel(
        static_cast<std::size_t>(
            spec.nchannels),
        0.0f);

    image.getpixel(
        x + spec.x,
        y + spec.y,
        pixel.data(),
        spec.nchannels);

    const std::array<float, 3> encoded = {{
        pixel[0],
        pixel[1],
        pixel[2]
    }};

    const InputTransform transform(
        input_encoding(
            input, resources), resources);

    const std::array<float, 3> ap0 =
        transform.to_ap0(
            encoded);

    FilmPipeline pipeline;

    if (!pipeline.initialize(
            pipeline_settings(
                resources,
                negative,
                print,
                exposure,
                negative_flash,
                print_flash,
                push_pull,
                color_density,
                color_depth,
                negative_bleach_bypass,
                print_bleach_bypass,
                printer_light_red,
                printer_light_green,
                printer_light_blue,
                printer_light_master,
                middle_gray,
                printer_temperature, color_response))) {

        throw std::runtime_error(
            "could not initialize FilmViz probe pipeline: "
            + pipeline.error());
    }

    SampledCurve factor;

    if (!pipeline.scene_factor(
            ap0,
            factor)) {

        throw std::runtime_error(
            "could not reconstruct probe spectrum");
    }

    const FilmPipeline::Result result =
        pipeline.process(
            ap0);

    if (!result.valid) {
        throw std::runtime_error(
            "probe pipeline result is invalid");
    }

    py::dict spectrum;

    const std::array<float, 11> wavelengths = {{
        400.0f,
        420.0f,
        440.0f,
        460.0f,
        500.0f,
        550.0f,
        600.0f,
        620.0f,
        640.0f,
        660.0f,
        680.0f
    }};

    for (float wavelength :
         wavelengths) {

        spectrum[py::str(
            std::to_string(
                static_cast<int>(
                    wavelength)))] =
            factor.sample(
                wavelength,
                0.0f);
    }

    py::dict probe;

    probe["x"] =
        x;

    probe["y"] =
        y;

    probe["width"] =
        spec.width;

    probe["height"] =
        spec.height;

    probe["encoded_rgb"] =
        py::make_tuple(
            encoded[0],
            encoded[1],
            encoded[2]);

    probe["ap0_input"] =
        py::make_tuple(
            ap0[0],
            ap0[1],
            ap0[2]);

    probe["spectrum"] =
        spectrum;

    probe["negative_exposure"] =
        py::make_tuple(
            result.negative_exposure.red,
            result.negative_exposure.green,
            result.negative_exposure.blue);

    probe["negative_status_m"] =
        py::make_tuple(
            result.negative_status_m_density.red,
            result.negative_status_m_density.green,
            result.negative_status_m_density.blue);

    probe["negative_calibrated"] =
        py::make_tuple(
            result.calibrated_negative_density.red,
            result.calibrated_negative_density.green,
            result.calibrated_negative_density.blue);

    probe["negative_color_response"] =
        py::make_tuple(
            result.color_response_negative_density.red,
            result.color_response_negative_density.green,
            result.color_response_negative_density.blue);

    probe["print_exposure"] =
        py::make_tuple(
            result.print_exposure.red,
            result.print_exposure.green,
            result.print_exposure.blue);

    probe["print_density"] =
        py::make_tuple(
            result.print_density.red,
            result.print_density.green,
            result.print_density.blue);

    probe["output_ap0"] =
        py::make_tuple(
            result.ap0[0],
            result.ap0[1],
            result.ap0[2]);

    probe["output_rec709_linear_unclamped"] =
        py::make_tuple(
            result.rec709_linear_unclamped[0],
            result.rec709_linear_unclamped[1],
            result.rec709_linear_unclamped[2]);

    probe["output_rec709_gamma24"] =
        py::make_tuple(
            result.rec709_gamma24[0],
            result.rec709_gamma24[1],
            result.rec709_gamma24[2]);

    return probe;
}

#if defined(FILMVIZ_PYTHON_HAS_METAL)
template<typename T>
T
dictionary_value(
    const py::dict& values,
    const char* name,
    const T& fallback)
{
    const py::str key(name);
    return values.contains(key)
        ? py::cast<T>(values[key])
        : fallback;
}

FilmVizOfxRenderSettings
metal_preview_settings(
    const py::dict& values,
    const std::string& resources)
{
    FilmVizOfxRenderSettings settings;
    const std::string input =
        dictionary_value<std::string>(
            values,
            "input",
            "awg3-logc3-ei800");
    const std::string output =
        dictionary_value<std::string>(
            values,
            "output",
            "rec709-gamma24");

    const InputTransform::Encoding encoding = input_encoding(input, resources);
    image_output(output, resources);

    settings.input_profile =
        static_cast<int>(encoding);
    settings.output_profile = static_cast<int>(image_output(output, resources));
    settings.color_response = color_response_tuning(values.contains("color_response")
        ? py::cast<py::dict>(values["color_response"]) : py::dict());
    settings.negative_profile =
        dictionary_value<std::string>(
            values,
            "negative",
            NegativeProfileCatalog::default_profile().identifier);
    settings.print_profile =
        dictionary_value<std::string>(
            values,
            "print",
            PrintProfileCatalog::default_profile().identifier);
    validate_stock_profiles(
        settings.negative_profile,
        settings.print_profile);

    settings.exposure_stops =
        dictionary_value<float>(values, "exposure", 0.0f);
    settings.negative_flash_percent =
        dictionary_value<float>(values, "negative_flash", 0.0f);
    settings.print_flash_percent =
        dictionary_value<float>(values, "print_flash", 0.0f);
    settings.push_pull_stops =
        dictionary_value<float>(values, "push_pull", 0.0f);
    settings.color_density =
        dictionary_value<float>(values, "color_density", 0.0f);
    settings.color_depth =
        dictionary_value<float>(
            values,
            "color_depth",
            FilmColorResponse::standard_color_depth);
    settings.negative_bleach_bypass =
        dictionary_value<float>(values, "negative_bleach_bypass", 0.0f);
    settings.print_bleach_bypass =
        dictionary_value<float>(values, "print_bleach_bypass", 0.0f);
    settings.printer_light_red =
        dictionary_value<float>(values, "printer_light_red", 25.0f);
    settings.printer_light_green =
        dictionary_value<float>(values, "printer_light_green", 25.0f);
    settings.printer_light_blue =
        dictionary_value<float>(values, "printer_light_blue", 25.0f);
    settings.printer_light_master =
        dictionary_value<float>(values, "printer_light_master", 0.0f);
    settings.middle_gray =
        dictionary_value<float>(values, "middle_gray", 0.18f);
    settings.printer_temperature =
        dictionary_value<float>(values, "printer_temperature", 3200.0f);

    settings.negative_grain =
        dictionary_value<float>(values, "negative_grain", 0.0f);
    settings.print_grain =
        dictionary_value<float>(values, "print_grain", 0.0f);
    settings.grain_enabled =
        settings.negative_grain > 0.0f || settings.print_grain > 0.0f;
    settings.grain_size =
        dictionary_value<float>(values, "grain_size", 1.0f);
    settings.grain_chroma =
        dictionary_value<float>(values, "grain_chroma", 1.0f);
    settings.grain_tonal_enabled = dictionary_value<bool>(values, "grain_tonal_enabled", true);
    settings.grain_shadows = dictionary_value<float>(values, "grain_shadows", 1.0f);
    settings.grain_midtones = dictionary_value<float>(values, "grain_midtones", 1.0f);
    settings.grain_highlights = dictionary_value<float>(values, "grain_highlights", 1.0f);
    for (float gain : {settings.grain_shadows, settings.grain_midtones, settings.grain_highlights})
        if (!std::isfinite(gain) || gain < 0.0f || gain > 2.0f)
            throw std::runtime_error("grain tonal multipliers must be finite and in 0..2");
    settings.grain_seed =
        dictionary_value<std::uint32_t>(values, "grain_seed", 1u);

    settings.film_format =
        dictionary_value<std::string>(values, "film_format", "super-35");
    settings.image_width_mm =
        dictionary_value<float>(values, "image_width_mm", 24.89f);
    settings.negative_mtf_amount =
        dictionary_value<float>(values, "negative_mtf", 0.0f);
    settings.print_mtf_amount =
        dictionary_value<float>(values, "print_mtf", 0.0f);
    settings.halation_strength =
        dictionary_value<float>(values, "halation_strength", 0.0f);
    settings.halation_radius =
        dictionary_value<float>(values, "halation_radius", 12.0f);
    settings.halation_threshold =
        dictionary_value<float>(values, "halation_threshold", 0.7f);
    settings.halation_enabled =
        settings.halation_strength > 0.0f
        && settings.halation_radius > 0.0f;
    return settings;
}
#endif

} // namespace

PYBIND11_MODULE(filmviz_python, module)
{
    module.doc() =
        "High-level Python bindings for the FilmViz spectral pipeline";

    module.def(
        "set_threads",
        &FilmVizThreading::set_thread_count,
        py::arg("count"),
        "Set the process-wide worker count; zero selects automatic mode.");

    module.def(
        "threads",
        &FilmVizThreading::thread_count,
        "Return the configured worker count; zero means automatic.");

    module.def(
        "effective_threads",
        [](int work_items) {
            return FilmVizThreading::effective_thread_count(work_items);
        },
        py::arg("work_items") = 1024);

#if defined(FILMVIZ_PYTHON_HAS_METAL)
    py::class_<FilmVizMetalPreview>(module, "MetalPreview")
        .def(py::init<>())
        .def_static("available", &FilmVizMetalPreview::available)
        .def("invalidate_profiles", &FilmVizMetalPreview::invalidate_profiles)
        .def("metal_allocated_bytes", &FilmVizMetalPreview::metal_allocated_bytes)
        .def(
            "render",
            [](FilmVizMetalPreview& preview,
                const std::string& input_filename,
                const std::string& resources,
                const py::dict& values,
                int max_dimension,
                double time) {

                const FilmVizOfxRenderSettings settings =
                    metal_preview_settings(values, resources);
                FilmVizMetalPreviewResult result;
                std::string error;
                bool rendered = false;

                {
                    py::gil_scoped_release release;
                    rendered = preview.render(
                        input_filename,
                        resources,
                        settings,
                        max_dimension,
                        time,
                        result,
                        error);
                }

                if (!rendered) {
                    throw std::runtime_error(error);
                }

                py::dict output;
                output["metal_bytes_before"] = result.metal_bytes_before;
                output["metal_bytes_configured"] = result.metal_bytes_configured;
                output["metal_bytes_submitted"] = result.metal_bytes_submitted;
                output["metal_bytes_fenced"] = result.metal_bytes_fenced;
                output["metal_bytes_after_pool"] = preview.metal_allocated_bytes();
                output["width"] = result.width;
                output["height"] = result.height;
                output["rgb"] = py::bytes(
                    reinterpret_cast<const char*>(result.display_rgb.data()),
                    result.display_rgb.size());
                output["scope_rgb"] = py::bytes(
                    reinterpret_cast<const char*>(result.scope_rgb.data()),
                    result.scope_rgb.size() * sizeof(float));
                return output;
            },
            py::arg("input_filename"),
            py::arg("resources"),
            py::arg("settings"),
            py::arg("max_dimension") = 1280,
            py::arg("time") = 0.0);

    module.attr("metal_preview_available") =
        FilmVizMetalPreview::available();
#else
    module.attr("metal_preview_available") = false;
#endif

    module.def("color_response_preview", [](const py::dict& values, float depth, float trim) {
        FilmColorResponse response({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
        FilmColorResponse::Settings standard, tuned;
        standard.amount = FilmColorResponse::standard_amount;
        tuned.amount = FilmColorResponse::amount_from_trim(trim);
        tuned.color_depth = depth;
        tuned.tuning = color_response_tuning(values);
        py::list bypass_points, standard_points, tuned_points;
        const auto point = [](const FilmDensity& d) {
            return py::make_tuple((d.red-d.green)*0.70710678f,
                (d.red+d.green-2.0f*d.blue)*0.40824829f);
        };
        for (int i = 0; i <= 96; ++i) {
            const float angle = i * 6.28318530718f / 96.0f;
            const float x = std::cos(angle)*0.346410162f;
            const float y = std::sin(angle)*0.346410162f;
            const FilmDensity input = {1.0f + x*0.70710678f+y*0.40824829f,
                1.0f-x*0.70710678f+y*0.40824829f, 1.0f-y*0.81649658f};
            bypass_points.append(point(input));
            standard_points.append(point(response.apply(input, standard)));
            tuned_points.append(point(response.apply(input, tuned)));
        }
        py::dict result;
        result["bypass"] = bypass_points;
        result["standard"] = standard_points;
        result["tuned"] = tuned_points;
        return result;
    }, py::arg("color_response") = py::dict(), py::arg("color_depth") = 1.0f, py::arg("color_density") = 0.0f);

    module.def("resolve_color_profile", &ColorProfileCatalog::resolve,
        py::arg("output"), py::arg("color_space"), py::arg("transfer_function"), py::arg("resources") = "");

    module.def(
        "profiles",
        [](const std::string& resources) {
            py::dict result;
            result["input"] = py::cast(InputTransform::profiles(resources));
            for (bool output : {false, true}) {
                py::list details;
                for (const auto& entry : ColorProfileCatalog::profiles(output, resources)) {
                    py::dict detail;
                    detail["color_space"] = entry.color_space;
                    detail["transfer_function"] = entry.transfer_function;
                    detail["profile"] = entry.profile;
                    details.append(detail);
                }
                result[output ? "output_details" : "input_details"] = details;
            }
            py::list negative_identifiers;
            py::list negative_details;

            for (const auto& profile : NegativeProfileCatalog::profiles()) {
                negative_identifiers.append(profile.identifier);

                py::dict detail;
                detail["identifier"] = profile.identifier;
                detail["display_name"] = profile.display_name;
                detail["resource_directory"] = profile.resource_directory;
                detail["resource_prefix"] = profile.resource_prefix;
                negative_details.append(detail);
            }

            result["negative"] = negative_identifiers;
            result["negative_details"] = negative_details;
            py::list print_identifiers;
            py::list print_details;

            for (const auto& profile : PrintProfileCatalog::profiles()) {
                print_identifiers.append(profile.identifier);

                py::dict detail;
                detail["identifier"] = profile.identifier;
                detail["display_name"] = profile.display_name;
                detail["resource_directory"] = profile.resource_directory;
                detail["sensitivity_filename"] =
                    profile.sensitivity_filename;
                detail["characteristic_filename"] =
                    profile.characteristic_filename;
                detail["dye_density_filename"] =
                    profile.dye_density_filename;
                detail["mtf_filename"] = profile.mtf_filename;
                detail["granularity_filename"] =
                    profile.granularity_filename;
                print_details.append(detail);
            }

            print_identifiers.append("none");
            result["print"] = print_identifiers;
            result["print_details"] = print_details;
            py::list film_formats;

            for (const auto& format : FilmFormatCatalog::formats()) {
                py::dict detail;
                detail["identifier"] = format.identifier;
                detail["display_name"] = format.display_name;
                detail["image_width_mm"] = format.image_width_mm;
                film_formats.append(detail);
            }

            result["film_formats"] = film_formats;
            result["output"] = py::cast(OutputTransform::profiles(resources));
            return result;
        }, py::arg("resources") = "");

    module.def(
        "generate_lut",
        &generate_lut,
        py::arg("resources") = "resources",
        py::arg("output_filename") = "filmviz.cube",
        py::arg("input") = "awg3-logc3-ei800",
        py::arg("negative") =
            NegativeProfileCatalog::default_profile().identifier,
        py::arg("print") =
            PrintProfileCatalog::default_profile().identifier,
        py::arg("output") = "ap0-linear",
        py::arg("lut_size") = 33,
        py::arg("exposure") = 0.0f,
        py::arg("negative_flash") = 0.0f,
        py::arg("print_flash") = 0.0f,
        py::arg("push_pull") = 0.0f,
        py::arg("color_density") = 0.0f,
        py::arg("color_depth") = FilmColorResponse::standard_color_depth,
        py::arg("negative_bleach_bypass") = 0.0f,
        py::arg("print_bleach_bypass") = 0.0f,
        py::arg("printer_light_red") = 25.0f,
        py::arg("printer_light_green") = 25.0f,
        py::arg("printer_light_blue") = 25.0f,
        py::arg("printer_light_master") = 0.0f,
        py::arg("middle_gray") = 0.18f,
        py::arg("printer_temperature") = 3200.0f,
        py::arg("threads") = 0,
        py::arg("progress") = py::none(),
        py::arg("cancel") = py::none(),
        py::arg("color_response") = py::dict());

    module.def(
        "process_image",
        &process_image,
        py::arg("resources") = "resources",
        py::arg("input_filename") = "",
        py::arg("output_filename") = "filmviz_output.tif",
        py::arg("input") = "awg3-logc3-ei800",
        py::arg("negative") =
            NegativeProfileCatalog::default_profile().identifier,
        py::arg("print") =
            PrintProfileCatalog::default_profile().identifier,
        py::arg("output") = "rec709-gamma24",
        py::arg("lut_size") = 33,
        py::arg("use_lut_acceleration") = true,
        py::arg("exposure") = 0.0f,
        py::arg("negative_flash") = 0.0f,
        py::arg("print_flash") = 0.0f,
        py::arg("push_pull") = 0.0f,
        py::arg("color_density") = 0.0f,
        py::arg("color_depth") = FilmColorResponse::standard_color_depth,
        py::arg("negative_bleach_bypass") = 0.0f,
        py::arg("print_bleach_bypass") = 0.0f,
        py::arg("printer_light_red") = 25.0f,
        py::arg("printer_light_green") = 25.0f,
        py::arg("printer_light_blue") = 25.0f,
        py::arg("printer_light_master") = 0.0f,
        py::arg("middle_gray") = 0.18f,
        py::arg("printer_temperature") = 3200.0f,
        py::arg("negative_grain") = 0.0f,
        py::arg("print_grain") = 0.0f,
        py::arg("grain_size") = 1.0f,
        py::arg("grain_chroma") = 1.0f,
        py::arg("grain_seed") = 1u,
        py::arg("film_format") =
            FilmFormatCatalog::default_format().identifier,
        py::arg("image_width_mm") =
            FilmFormatCatalog::default_format().image_width_mm,
        py::arg("negative_mtf") = 0.0f,
        py::arg("print_mtf") = 0.0f,
        py::arg("halation_strength") = 0.0f,
        py::arg("halation_radius") = 12.0f,
        py::arg("halation_threshold") = 0.7f,
        py::arg("threads") = 0,
        py::arg("progress") = py::none(),
        py::arg("cancel") = py::none(),
        py::arg("color_response") = py::dict(),
        py::arg("grain_tonal_enabled") = true,
        py::arg("grain_shadows") = 1.0f,
        py::arg("grain_midtones") = 1.0f,
        py::arg("grain_highlights") = 1.0f);
    module.def(
        "probe_image_pixel",
        &probe_image_pixel,
        py::arg("resources") = "resources",
        py::arg("input_filename") = "",
        py::arg("input") = "awg3-logc3-ei800",
        py::arg("negative") =
            NegativeProfileCatalog::default_profile().identifier,
        py::arg("print") =
            PrintProfileCatalog::default_profile().identifier,
        py::arg("exposure") = 0.0f,
        py::arg("negative_flash") = 0.0f,
        py::arg("print_flash") = 0.0f,
        py::arg("push_pull") = 0.0f,
        py::arg("color_density") = 0.0f,
        py::arg("color_depth") = FilmColorResponse::standard_color_depth,
        py::arg("negative_bleach_bypass") = 0.0f,
        py::arg("print_bleach_bypass") = 0.0f,
        py::arg("printer_light_red") = 25.0f,
        py::arg("printer_light_green") = 25.0f,
        py::arg("printer_light_blue") = 25.0f,
        py::arg("printer_light_master") = 0.0f,
        py::arg("middle_gray") = 0.18f,
        py::arg("printer_temperature") = 3200.0f,
        py::arg("u") = 0.5,
        py::arg("v") = 0.5,
        py::arg("color_response") = py::dict(),
        "Probe one source-image pixel through every major FilmViz pipeline stage.");

    module.def(
        "read_image_preview",
        &read_image_preview,
        py::arg("filename"),
        py::arg("max_dimension") = 1600,
        "Read an image through OpenImageIO and return an 8-bit RGB preview.");

}
