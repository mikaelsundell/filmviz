// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "imageprocessor.h"

#include "filmpipeline.h"
#include "granularitymodel.h"
#include "halationmodel.h"
#include "lut3d.h"
#include "threading.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace OIIO;

namespace
{

using GrainSample = std::array<float, 6>;

GrainSample
sample_field(
    const std::vector<GrainSample>& values,
    int size,
    const Lut3D::RGB& input)
{
    GrainSample result = {{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
    const float scale = static_cast<float>(size - 1);
    float coordinate[3];
    int lower[3];
    float fraction[3];

    for (int i = 0; i < 3; ++i) {
        coordinate[i] =
            std::clamp(input[i], 0.0f, 1.0f)
            * scale;
        lower[i] =
            std::min(
                size - 2,
                static_cast<int>(
                    std::floor(coordinate[i])));
        fraction[i] =
            coordinate[i]
            - static_cast<float>(lower[i]);
    }

    for (int db = 0; db < 2; ++db) {
        for (int dg = 0; dg < 2; ++dg) {
            for (int dr = 0; dr < 2; ++dr) {
                const float weight =
                    (dr ? fraction[0] : 1.0f - fraction[0])
                    * (dg ? fraction[1] : 1.0f - fraction[1])
                    * (db ? fraction[2] : 1.0f - fraction[2]);

                const int r = lower[0] + dr;
                const int g = lower[1] + dg;
                const int b = lower[2] + db;
                const std::size_t index =
                    static_cast<std::size_t>(
                        (b * size + g) * size + r);

                for (int channel = 0; channel < 6; ++channel) {
                    result[channel] +=
                        weight
                        * values[index][channel];
                }
            }
        }
    }

    return result;
}

float
spatial_normal(
    std::uint32_t seed,
    int x,
    int y,
    int stage,
    int channel,
    float size_pixels)
{
    const float scale =
        std::max(
            1.0f,
            size_pixels);

    const float px =
        static_cast<float>(x)
        / scale;

    const float py =
        static_cast<float>(y)
        / scale;

    const int x0 =
        static_cast<int>(
            std::floor(px));

    const int y0 =
        static_cast<int>(
            std::floor(py));

    const float tx = px - static_cast<float>(x0);
    const float ty = py - static_cast<float>(y0);
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    const float weights[4] = {
        (1.0f - sx) * (1.0f - sy),
        sx * (1.0f - sy),
        (1.0f - sx) * sy,
        sx * sy
    };

    const float samples[4] = {
        GranularityModel::normal_sample(seed, x0, y0, stage, channel),
        GranularityModel::normal_sample(seed, x0 + 1, y0, stage, channel),
        GranularityModel::normal_sample(seed, x0, y0 + 1, stage, channel),
        GranularityModel::normal_sample(seed, x0 + 1, y0 + 1, stage, channel)
    };

    float value = 0.0f;
    float variance = 0.0f;

    for (int i = 0; i < 4; ++i) {
        value += weights[i] * samples[i];
        variance += weights[i] * weights[i];
    }

    return
        variance > 1e-10f
            ? value / std::sqrt(variance)
            : value;
}

float
to_linear(
    float value,
    ImageProcessor::Output output)
{
    return
        output == ImageProcessor::Output::Rec709Gamma24
            ? std::pow(
                std::max(0.0f, value),
                2.4f)
            : value;
}

float
from_linear(
    float value,
    ImageProcessor::Output output)
{
    return
        output == ImageProcessor::Output::Rec709Gamma24
            ? std::pow(
                std::max(0.0f, value),
                1.0f / 2.4f)
            : value;
}


} // namespace

std::array<float, 3>
ImageProcessor::mix_grain_chroma(
    const std::array<float, 3>& density_noise,
    float chroma)
{
    constexpr std::array<float, 3> luma = {{
        0.2126f,
        0.7152f,
        0.0722f
    }};

    const float neutral =
        luma[0] * density_noise[0]
        + luma[1] * density_noise[1]
        + luma[2] * density_noise[2];

    std::array<float, 3> result;

    for (int channel = 0; channel < 3; ++channel) {
        result[channel] =
            neutral
            + chroma
                * (density_noise[channel] - neutral);
    }

    return result;
}

bool
ImageProcessor::process(
    const std::string& input_filename,
    const std::string& output_filename,
    const FilmPipeline& pipeline,
    const InputTransform& input_transform,
    const Settings& settings,
    const Progress& progress,
    const Cancel& cancel)
{
    error_.clear();

    HalationModel::Settings halation_validation_settings;
    halation_validation_settings.strength = settings.halation_strength;
    halation_validation_settings.radius_pixels = settings.halation_radius_pixels;
    halation_validation_settings.threshold = settings.halation_threshold;

    if (!pipeline.valid()
        || settings.lut_size < 2
        || settings.negative_grain_strength < 0.0f
        || settings.print_grain_strength < 0.0f
        || settings.grain_size_pixels < 1.0f
        || settings.grain_chroma < 0.0f
        || !HalationModel::valid_settings(
            halation_validation_settings)) {

        error_ = "invalid image-processing settings";
        return false;
    }

    const int size = settings.lut_size;
    const bool halation_enabled =
        settings.halation_strength > 0.0f
        && settings.halation_radius_pixels > 0.0f;

    if (!settings.use_lut_acceleration
        && (halation_enabled
            || settings.negative_grain_strength > 0.0f
            || settings.print_grain_strength > 0.0f)) {

        error_ =
            "direct spectral mode requires grain and halation to be disabled";
        return false;
    }

    const std::size_t field_size =
        static_cast<std::size_t>(size * size * size);
    std::vector<GrainSample> grain_field(
        field_size);
    Lut3D lut;
    Lut3D negative_exposure_lut;
    std::atomic<bool> cancel_requested(false);

    const auto cancelled =
        [&]() {
            if (cancel_requested.load(
                    std::memory_order_relaxed)) {
                return true;
            }

            if (cancel && cancel()) {
                cancel_requested.store(
                    true,
                    std::memory_order_relaxed);
                return true;
            }

            return false;
        };

    if (settings.use_lut_acceleration
        && !halation_enabled) {
        const bool generated =
            lut.generate(
                size,
                [&](const Lut3D::RGB& lookup_input,
                    Lut3D::RGB& output) {

                    if (cancel_requested.load(
                            std::memory_order_relaxed)) {
                        return false;
                    }

                    const FilmPipeline::Result result =
                        pipeline.process(
                            input_transform.to_ap0(
                                lookup_input));

                    if (!result.valid) {
                        return false;
                    }

                    output =
                        settings.output == Output::Rec709Gamma24
                            ? result.rec709_gamma24
                            : result.ap0;

                    const int r =
                        static_cast<int>(
                            std::lround(lookup_input[0] * (size - 1)));
                    const int g =
                        static_cast<int>(
                            std::lround(lookup_input[1] * (size - 1)));
                    const int b =
                        static_cast<int>(
                            std::lround(lookup_input[2] * (size - 1)));
                    const std::size_t index =
                        static_cast<std::size_t>(
                            (b * size + g) * size + r);

                    grain_field[index] = {{
                        result.negative_granularity_sigma.red,
                        result.negative_granularity_sigma.green,
                        result.negative_granularity_sigma.blue,
                        result.print_granularity_sigma.red,
                        result.print_granularity_sigma.green,
                        result.print_granularity_sigma.blue
                    }};

                    return true;
                },
                [&](int completed,
                    int total) {

                    if (cancelled()) {
                        return;
                    }

                    if (progress) {
                        progress(
                            "LUT creation",
                            completed,
                            total);
                    }
                });

        if (!generated) {
            error_ =
                cancel_requested.load(
                    std::memory_order_relaxed)
                    ? "image processing cancelled"
                    : "could not generate image-processing LUT";
            return false;
        }
    }
    else if (settings.use_lut_acceleration) {
        const bool generated =
            negative_exposure_lut.generate(
                size,
                [&](const Lut3D::RGB& lookup_input,
                    Lut3D::RGB& output) {

                    if (cancel_requested.load(
                            std::memory_order_relaxed)) {
                        return false;
                    }

                    FilmExposure exposure;

                    if (!pipeline.negative_exposure(
                            input_transform.to_ap0(
                                lookup_input),
                            exposure)) {
                        return false;
                    }

                    output = {{
                        exposure.red,
                        exposure.green,
                        exposure.blue
                    }};

                    return true;
                },
                [&](int completed,
                    int total) {

                    if (cancelled()) {
                        return;
                    }

                    if (progress) {
                        progress(
                            "Negative exposure LUT",
                            completed,
                            total);
                    }
                });

        if (!generated) {
            error_ =
                cancel_requested.load(
                    std::memory_order_relaxed)
                    ? "image processing cancelled"
                    : "could not generate negative-exposure LUT";
            return false;
        }
    }

    if (cancelled()) {
        error_ = "image processing cancelled";
        return false;
    }

    ImageBuf input_image(input_filename);

    if (!input_image.read(0, 0, true, TypeDesc::FLOAT)) {
        error_ =
            "could not read input image: "
            + input_image.geterror();
        return false;
    }

    const ImageSpec& input_spec = input_image.spec();

    if (input_spec.nchannels < 3) {
        error_ = "input image must contain at least three channels";
        return false;
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(input_spec.width)
        * static_cast<std::size_t>(input_spec.height);
    std::vector<float> input_pixels(
        pixel_count
        * static_cast<std::size_t>(input_spec.nchannels));
    std::vector<float> output_pixels(
        pixel_count * 3u,
        0.0f);

    if (!input_image.get_pixels(
            input_image.roi(),
            TypeDesc::FLOAT,
            input_pixels.data())) {

        error_ =
            "could not read input pixels: "
            + input_image.geterror();
        return false;
    }

    std::vector<float> scene_ap0_pixels;
    std::vector<FilmExposure> negative_exposure_pixels;

    if (halation_enabled) {
        scene_ap0_pixels.assign(
            pixel_count * 3u,
            0.0f);

        negative_exposure_pixels.assign(
            pixel_count,
            FilmExposure());

        if (progress) {
            progress(
                "Halation exposure",
                0,
                input_spec.height);
        }

        std::atomic<int> exposure_next_row(0);
        std::atomic<int> exposure_completed_rows(0);
        std::mutex exposure_progress_mutex;
        const int exposure_worker_count =
            FilmVizThreading::effective_thread_count(
                input_spec.height);
        std::vector<std::thread> exposure_workers;
        exposure_workers.reserve(
            static_cast<std::size_t>(exposure_worker_count));

        for (int worker = 0;
             worker < exposure_worker_count;
             ++worker) {

            exposure_workers.emplace_back(
                [&]() {
                    while (!cancelled()) {
                        const int y =
                            exposure_next_row.fetch_add(
                                1,
                                std::memory_order_relaxed);

                        if (y >= input_spec.height) {
                            break;
                        }

                        for (int x = 0;
                             x < input_spec.width;
                             ++x) {

                            const std::size_t pixel =
                                static_cast<std::size_t>(y)
                                * static_cast<std::size_t>(input_spec.width)
                                + static_cast<std::size_t>(x);

                            const std::size_t input_offset =
                                pixel
                                * static_cast<std::size_t>(
                                    input_spec.nchannels);

                            const Lut3D::RGB encoded = {{
                                input_pixels[input_offset + 0],
                                input_pixels[input_offset + 1],
                                input_pixels[input_offset + 2]
                            }};

                            const std::array<float, 3> ap0 =
                                input_transform.to_ap0(
                                    encoded);

                            scene_ap0_pixels[pixel * 3u + 0] = ap0[0];
                            scene_ap0_pixels[pixel * 3u + 1] = ap0[1];
                            scene_ap0_pixels[pixel * 3u + 2] = ap0[2];

                            const Lut3D::RGB exposure =
                                negative_exposure_lut.sample_tetrahedral(
                                    encoded);

                            negative_exposure_pixels[pixel] = {
                                exposure[0],
                                exposure[1],
                                exposure[2]
                            };
                        }

                        const int finished =
                            exposure_completed_rows.fetch_add(
                                1,
                                std::memory_order_relaxed)
                            + 1;

                        if (progress) {
                            const std::lock_guard<std::mutex> lock(
                                exposure_progress_mutex);
                            progress(
                                "Halation exposure",
                                finished,
                                input_spec.height);
                        }
                    }
                });
        }

        for (std::thread& worker : exposure_workers) {
            worker.join();
        }

        if (cancelled()) {
            error_ = "image processing cancelled";
            return false;
        }

        HalationModel::Settings halation_settings;
        halation_settings.strength = settings.halation_strength;
        halation_settings.radius_pixels = settings.halation_radius_pixels;
        halation_settings.threshold = settings.halation_threshold;

        if (!HalationModel::apply(
                negative_exposure_pixels,
                scene_ap0_pixels,
                input_spec.width,
                input_spec.height,
                halation_settings,
                cancel,
                [&](int completed,
                    int total) {

                    if (progress) {
                        progress(
                            "Halation scatter",
                            completed,
                            total);
                    }
                })) {

            error_ =
                cancelled()
                    ? "image processing cancelled"
                    : "halation processing failed";
            return false;
        }

        if (cancelled()) {
            error_ = "image processing cancelled";
            return false;
        }

        std::array<float, 3> log_min = {{
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity()
        }};
        std::array<float, 3> log_max = {{
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()
        }};

        for (const FilmExposure& exposure : negative_exposure_pixels) {
            const float values[3] = {
                exposure.red,
                exposure.green,
                exposure.blue
            };

            for (int channel = 0;
                 channel < 3;
                 ++channel) {

                const float log_value =
                    std::log10(
                        std::max(
                            values[channel],
                            1e-20f));

                log_min[channel] =
                    std::min(
                        log_min[channel],
                        log_value);

                log_max[channel] =
                    std::max(
                        log_max[channel],
                        log_value);
            }
        }

        constexpr float log_padding = 0.02f;

        for (int channel = 0;
             channel < 3;
             ++channel) {

            if (!std::isfinite(log_min[channel])
                || !std::isfinite(log_max[channel])) {

                error_ = "invalid negative exposure range after halation";
                return false;
            }

            if (log_max[channel] - log_min[channel] < 1e-4f) {
                const float center =
                    0.5f
                    * (log_min[channel]
                       + log_max[channel]);

                log_min[channel] = center - 0.05f;
                log_max[channel] = center + 0.05f;
            }
            else {
                log_min[channel] -= log_padding;
                log_max[channel] += log_padding;
            }
        }

        lut = Lut3D();
        grain_field.assign(
            field_size,
            GrainSample());

        const bool generated =
            lut.generate(
                size,
                [&](const Lut3D::RGB& lookup_input,
                    Lut3D::RGB& output) {

                    if (cancel_requested.load(
                            std::memory_order_relaxed)) {
                        return false;
                    }

                    FilmExposure exposure;
                    float* channels[3] = {
                        &exposure.red,
                        &exposure.green,
                        &exposure.blue
                    };

                    for (int channel = 0;
                         channel < 3;
                         ++channel) {

                        const float log_value =
                            log_min[channel]
                            + lookup_input[channel]
                                * (log_max[channel]
                                   - log_min[channel]);

                        *channels[channel] =
                            std::pow(
                                10.0f,
                                log_value);
                    }

                    const FilmPipeline::Result result =
                        pipeline.process_negative_exposure(
                            exposure);

                    if (!result.valid) {
                        return false;
                    }

                    output =
                        settings.output == Output::Rec709Gamma24
                            ? result.rec709_gamma24
                            : result.ap0;

                    const int r =
                        static_cast<int>(
                            std::lround(lookup_input[0] * (size - 1)));
                    const int g =
                        static_cast<int>(
                            std::lround(lookup_input[1] * (size - 1)));
                    const int b =
                        static_cast<int>(
                            std::lround(lookup_input[2] * (size - 1)));
                    const std::size_t index =
                        static_cast<std::size_t>(
                            (b * size + g) * size + r);

                    grain_field[index] = {{
                        result.negative_granularity_sigma.red,
                        result.negative_granularity_sigma.green,
                        result.negative_granularity_sigma.blue,
                        result.print_granularity_sigma.red,
                        result.print_granularity_sigma.green,
                        result.print_granularity_sigma.blue
                    }};

                    return true;
                },
                [&](int completed,
                    int total) {

                    if (cancelled()) {
                        return;
                    }

                    if (progress) {
                        progress(
                            "Halation development LUT",
                            completed,
                            total);
                    }
                });

        if (!generated) {
            error_ =
                cancel_requested.load(
                    std::memory_order_relaxed)
                    ? "image processing cancelled"
                    : "could not generate halation development LUT";
            return false;
        }

        if (cancelled()) {
            error_ = "image processing cancelled";
            return false;
        }

        // Re-map the modified negative exposure into the normalized domain of
        // the second LUT. From this point onward the normal fast image path is
        // used; no full spectral evaluation occurs per image pixel.
        scene_ap0_pixels.clear();
        scene_ap0_pixels.shrink_to_fit();

        for (FilmExposure& exposure : negative_exposure_pixels) {
            float* channels[3] = {
                &exposure.red,
                &exposure.green,
                &exposure.blue
            };

            for (int channel = 0;
                 channel < 3;
                 ++channel) {

                const float log_value =
                    std::log10(
                        std::max(
                            *channels[channel],
                            1e-20f));

                *channels[channel] =
                    std::clamp(
                        (log_value - log_min[channel])
                        / (log_max[channel] - log_min[channel]),
                        0.0f,
                        1.0f);
            }
        }
    }

    if (progress) {
        progress("Image conversion", 0, input_spec.height);
    }

    std::atomic<int> next_row(0);
    std::atomic<int> completed_rows(0);
    std::atomic<bool> processing_failed(false);
    std::mutex progress_mutex;
    const int worker_count =
        FilmVizThreading::effective_thread_count(
            input_spec.height);
    std::vector<std::thread> workers;
    workers.reserve(
        static_cast<std::size_t>(worker_count));

    for (int worker = 0;
         worker < worker_count;
         ++worker) {

        workers.emplace_back(
            [&]() {
                while (!cancel_requested.load(
                           std::memory_order_relaxed)) {

                    const int y =
                        next_row.fetch_add(
                            1,
                            std::memory_order_relaxed);

                    if (y >= input_spec.height) {
                        break;
                    }

                    for (int x = 0;
                         x < input_spec.width;
                         ++x) {

                        const std::size_t pixel =
                            static_cast<std::size_t>(y)
                            * static_cast<std::size_t>(input_spec.width)
                            + static_cast<std::size_t>(x);
                        const std::size_t input_offset =
                            pixel
                            * static_cast<std::size_t>(input_spec.nchannels);

                        if (!settings.use_lut_acceleration) {
                            const Lut3D::RGB encoded = {{
                                input_pixels[input_offset + 0],
                                input_pixels[input_offset + 1],
                                input_pixels[input_offset + 2]
                            }};

                            const FilmPipeline::Result direct =
                                pipeline.process(
                                    input_transform.to_ap0(
                                        encoded));

                            if (!direct.valid) {
                                processing_failed.store(
                                    true,
                                    std::memory_order_relaxed);
                                cancel_requested.store(
                                    true,
                                    std::memory_order_relaxed);
                                break;
                            }

                            const Lut3D::RGB converted =
                                settings.output == Output::Rec709Gamma24
                                    ? direct.rec709_gamma24
                                    : direct.ap0;

                            for (int channel = 0;
                                 channel < 3;
                                 ++channel) {

                                output_pixels[pixel * 3u + channel] =
                                    std::clamp(
                                        converted[channel],
                                        0.0f,
                                        1.0f);
                            }

                            continue;
                        }

                        Lut3D::RGB lookup_input;

                        if (halation_enabled) {
                            const FilmExposure& normalized =
                                negative_exposure_pixels[pixel];

                            lookup_input = {{
                                normalized.red,
                                normalized.green,
                                normalized.blue
                            }};
                        }
                        else {
                            lookup_input = {{
                                input_pixels[input_offset + 0],
                                input_pixels[input_offset + 1],
                                input_pixels[input_offset + 2]
                            }};
                        }

                        const Lut3D::RGB converted =
                            lut.sample_tetrahedral(
                                lookup_input);

                        GrainSample sigma =
                            sample_field(
                                grain_field,
                                size,
                                lookup_input);

                        std::array<float, 3> density_noise;

                        for (int channel = 0;
                             channel < 3;
                             ++channel) {

                            const float negative_noise =
                                settings.negative_grain_strength
                                * sigma[channel]
                                * spatial_normal(
                                    settings.grain_seed,
                                    x,
                                    y,
                                    0,
                                    channel,
                                    settings.grain_size_pixels);

                            const float print_noise =
                                settings.print_grain_strength
                                * sigma[channel + 3]
                                * spatial_normal(
                                    settings.grain_seed,
                                    x,
                                    y,
                                    1,
                                    channel,
                                    settings.grain_size_pixels);

                            density_noise[channel] =
                                negative_noise - print_noise;
                        }

                        density_noise =
                            mix_grain_chroma(
                                density_noise,
                                settings.grain_chroma);

                        for (int channel = 0;
                             channel < 3;
                             ++channel) {

                            float linear =
                                to_linear(
                                    converted[channel],
                                    settings.output);

                            linear *=
                                std::pow(
                                    10.0f,
                                    density_noise[channel]);

                            output_pixels[pixel * 3u + channel] =
                                std::clamp(
                                    from_linear(
                                        linear,
                                        settings.output),
                                    0.0f,
                                    1.0f);
                        }
                    }

                    const int finished =
                        completed_rows.fetch_add(
                            1,
                            std::memory_order_relaxed)
                        + 1;

                    {
                        const std::lock_guard<std::mutex> lock(
                            progress_mutex);

                        if (cancelled()) {
                            break;
                        }

                        if (progress) {
                            progress(
                                "Image conversion",
                                finished,
                                input_spec.height);
                        }
                    }
                }
            });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    if (cancel_requested.load(
            std::memory_order_relaxed)) {
        error_ = "image processing cancelled";
        return false;
    }

    if (processing_failed.load(
            std::memory_order_relaxed)) {

        error_ = "spectral evaluation failed during image processing";
        return false;
    }

    const std::filesystem::path output_path(output_filename);
    std::error_code filesystem_error;

    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(
            output_path.parent_path(),
            filesystem_error);
    }

    if (filesystem_error) {
        error_ = "could not create output directory";
        return false;
    }

    ImageSpec output_spec(
        input_spec.width,
        input_spec.height,
        3,
        TypeDesc::UINT16);
    output_spec.x = input_spec.x;
    output_spec.y = input_spec.y;
    output_spec.channelnames = {"R", "G", "B"};
    output_spec.attribute(
        "oiio:ColorSpace",
        settings.output == Output::Rec709Gamma24
            ? "Rec.709 Gamma 2.4"
            : "ACES2065-1 linear");
    output_spec.attribute(
        "filmviz:negative_grain_strength",
        settings.negative_grain_strength);
    output_spec.attribute(
        "filmviz:print_grain_strength",
        settings.print_grain_strength);
    output_spec.attribute(
        "filmviz:grain_seed",
        static_cast<int>(settings.grain_seed));
    output_spec.attribute(
        "filmviz:grain_chroma",
        settings.grain_chroma);
    output_spec.attribute(
        "filmviz:halation_strength",
        settings.halation_strength);
    output_spec.attribute(
        "filmviz:halation_radius_pixels",
        settings.halation_radius_pixels);
    output_spec.attribute(
        "filmviz:halation_threshold",
        settings.halation_threshold);
    output_spec.attribute("compression", "zip");

    ImageBuf output_image(
        output_filename,
        output_spec);

    if (!output_image.set_pixels(
            output_image.roi(),
            TypeDesc::FLOAT,
            output_pixels.data())
        || !output_image.write(output_filename)) {

        error_ =
            "could not write output image: "
            + output_image.geterror();
        return false;
    }

    return true;
}

const std::string&
ImageProcessor::error() const
{
    return error_;
}
