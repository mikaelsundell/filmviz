// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizofxprocessor.h"

#include "filmpipeline.h"
#include "granularitymodel.h"
#include "halationmodel.h"
#include "imageprocessor.h"
#include "inputtransform.h"
#include "lut3d.h"
#include "threading.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace
{

using GrainSample = std::array<float, 6>;

bool
finite_setting(
    float value)
{
    return std::isfinite(value);
}

float
clamp01(
    float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

const float*
source_pixel(
    const FilmVizOfxFrame& frame,
    int x,
    int y)
{
    if (!frame.data
        || x < frame.x1
        || x >= frame.x2
        || y < frame.y1
        || y >= frame.y2) {

        return nullptr;
    }

    const char* row =
        reinterpret_cast<const char*>(frame.data)
        + static_cast<std::ptrdiff_t>(y - frame.y1)
            * frame.row_bytes;

    return
        reinterpret_cast<const float*>(row)
        + static_cast<std::ptrdiff_t>(x - frame.x1) * 4;
}

float*
destination_pixel(
    const FilmVizOfxFrame& frame,
    int x,
    int y)
{
    if (!frame.data
        || x < frame.x1
        || x >= frame.x2
        || y < frame.y1
        || y >= frame.y2) {

        return nullptr;
    }

    char* row =
        reinterpret_cast<char*>(frame.data)
        + static_cast<std::ptrdiff_t>(y - frame.y1)
            * frame.row_bytes;

    return
        reinterpret_cast<float*>(row)
        + static_cast<std::ptrdiff_t>(x - frame.x1) * 4;
}

GrainSample
sample_field(
    const std::vector<GrainSample>& values,
    int size,
    const Lut3D::RGB& input)
{
    GrainSample result = {{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};

    if (size < 2
        || values.size()
            != static_cast<std::size_t>(size * size * size)) {

        return result;
    }

    const float scale = static_cast<float>(size - 1);
    float coordinate[3];
    int lower[3];
    float fraction[3];

    for (int i = 0; i < 3; ++i) {
        coordinate[i] =
            clamp01(input[i])
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
        std::max(1.0f, size_pixels);

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
    int output_profile)
{
    return
        output_profile == 1
            ? std::pow(
                std::max(0.0f, value),
                2.4f)
            : value;
}

float
from_linear(
    float value,
    int output_profile)
{
    return
        output_profile == 1
            ? std::pow(
                std::max(0.0f, value),
                1.0f / 2.4f)
            : value;
}

InputTransform::Encoding
input_encoding(
    int input_profile)
{
    return
        input_profile == 0
            ? InputTransform::Encoding::AWG3_LogC3_EI800
            : InputTransform::Encoding::ACES2065_1_Linear;
}

bool
same_transform_settings(
    const FilmVizOfxRenderSettings& a,
    const FilmVizOfxRenderSettings& b)
{
    return
        a.negative_profile == b.negative_profile
        && a.input_profile == b.input_profile
        && a.output_profile == b.output_profile
        && a.lut_size == b.lut_size
        && a.exposure_stops == b.exposure_stops
        && a.push_pull_stops == b.push_pull_stops
        && a.middle_gray == b.middle_gray
        && a.printer_temperature == b.printer_temperature
        && a.negative_bleach_bypass == b.negative_bleach_bypass
        && a.print_bleach_bypass == b.print_bleach_bypass
        && a.printer_light_red == b.printer_light_red
        && a.printer_light_green == b.printer_light_green
        && a.printer_light_blue == b.printer_light_blue;
}

} // namespace

struct FilmVizOfxProcessor::Cache
{
    FilmVizOfxRenderSettings settings;
    std::string resources_directory;
    std::shared_ptr<FilmPipeline> pipeline;
    std::unique_ptr<InputTransform> input_transform;
    std::unique_ptr<Lut3D> color_lut;
    std::unique_ptr<Lut3D> negative_exposure_lut;
    std::vector<GrainSample> grain_field;

    std::unique_ptr<Lut3D> halation_development_lut;
    std::vector<GrainSample> halation_grain_field;
    std::array<float, 3> halation_log_min = {{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> halation_log_max = {{1.0f, 1.0f, 1.0f}};
};

bool
FilmVizOfxRenderSettings::operator==(
    const FilmVizOfxRenderSettings& other) const
{
    return
        negative_profile == other.negative_profile
        && input_profile == other.input_profile
        && output_profile == other.output_profile
        && lut_size == other.lut_size
        && threads == other.threads
        && exposure_stops == other.exposure_stops
        && push_pull_stops == other.push_pull_stops
        && middle_gray == other.middle_gray
        && printer_temperature == other.printer_temperature
        && negative_bleach_bypass == other.negative_bleach_bypass
        && print_bleach_bypass == other.print_bleach_bypass
        && printer_light_red == other.printer_light_red
        && printer_light_green == other.printer_light_green
        && printer_light_blue == other.printer_light_blue
        && grain_enabled == other.grain_enabled
        && negative_grain == other.negative_grain
        && print_grain == other.print_grain
        && grain_size == other.grain_size
        && grain_chroma == other.grain_chroma
        && grain_seed == other.grain_seed
        && halation_enabled == other.halation_enabled
        && halation_strength == other.halation_strength
        && halation_radius == other.halation_radius
        && halation_threshold == other.halation_threshold;
}

FilmVizOfxProcessor::FilmVizOfxProcessor() = default;
FilmVizOfxProcessor::~FilmVizOfxProcessor() = default;

bool
FilmVizOfxProcessor::configure(
    const FilmVizOfxRenderSettings& settings,
    const std::string& resources_directory,
    std::string& error)
{
    error.clear();

    if (settings.lut_size < 2
        || settings.lut_size > 129
        || !finite_setting(settings.exposure_stops)
        || !finite_setting(settings.push_pull_stops)
        || !finite_setting(settings.middle_gray)
        || settings.middle_gray <= 0.0f
        || !finite_setting(settings.printer_temperature)
        || settings.printer_temperature <= 0.0f
        || settings.negative_bleach_bypass < 0.0f
        || settings.negative_bleach_bypass > 1.0f
        || settings.print_bleach_bypass < 0.0f
        || settings.print_bleach_bypass > 1.0f
        || settings.printer_light_red < 0.0f
        || settings.printer_light_red > 50.0f
        || settings.printer_light_green < 0.0f
        || settings.printer_light_green > 50.0f
        || settings.printer_light_blue < 0.0f
        || settings.printer_light_blue > 50.0f
        || settings.negative_grain < 0.0f
        || settings.print_grain < 0.0f
        || settings.grain_size < 1.0f
        || settings.grain_chroma < 0.0f
        || settings.halation_strength < 0.0f
        || settings.halation_strength > 1.0f
        || settings.halation_radius < 0.0f
        || settings.halation_threshold < 0.0f) {

        error = "invalid FilmViz OFX settings";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    FilmVizThreading::set_thread_count(settings.threads);

    if (cache_
        && cache_->resources_directory == resources_directory
        && same_transform_settings(cache_->settings, settings)) {

        cache_->settings = settings;

        const bool needs_exposure_lut =
            settings.halation_enabled
            && settings.halation_strength > 0.0f
            && settings.halation_radius > 0.0f
            && !cache_->negative_exposure_lut;

        if (!needs_exposure_lut) {
            return true;
        }

        cache_->negative_exposure_lut =
            std::make_unique<Lut3D>();

        const bool exposure_generated =
            cache_->negative_exposure_lut->generate(
                settings.lut_size,
                [&](const Lut3D::RGB& encoded,
                    Lut3D::RGB& exposure_rgb) {

                    FilmExposure exposure;

                    if (!cache_->pipeline->negative_exposure(
                            cache_->input_transform->to_ap0(encoded),
                            exposure)) {

                        return false;
                    }

                    exposure_rgb = {{
                        exposure.red,
                        exposure.green,
                        exposure.blue
                    }};

                    return true;
                });

        if (!exposure_generated) {
            cache_->negative_exposure_lut.reset();
            error = "could not generate FilmViz OFX negative-exposure LUT";
            return false;
        }

        cache_->halation_development_lut.reset();
        cache_->halation_grain_field.clear();
        ++revision_;

        return true;
    }

    auto next = std::make_unique<Cache>();
    next->settings = settings;
    next->resources_directory = resources_directory;
    next->pipeline = std::make_shared<FilmPipeline>();
    next->input_transform =
        std::make_unique<InputTransform>(
            input_encoding(settings.input_profile));

    FilmPipeline::Settings pipeline_settings;
    pipeline_settings.resources_directory = resources_directory;
    pipeline_settings.negative_profile = settings.negative_profile;
    pipeline_settings.exposure_stops = settings.exposure_stops;
    pipeline_settings.push_pull_stops = settings.push_pull_stops;
    pipeline_settings.middle_gray = settings.middle_gray;
    pipeline_settings.printer_temperature_kelvin = settings.printer_temperature;
    pipeline_settings.negative_bleach_bypass = settings.negative_bleach_bypass;
    pipeline_settings.print_bleach_bypass = settings.print_bleach_bypass;
    pipeline_settings.printer_light_red = settings.printer_light_red;
    pipeline_settings.printer_light_green = settings.printer_light_green;
    pipeline_settings.printer_light_blue = settings.printer_light_blue;

    if (!next->pipeline->initialize(pipeline_settings)) {
        error = next->pipeline->error();
        return false;
    }

    const int size = settings.lut_size;
    const std::size_t field_size =
        static_cast<std::size_t>(size * size * size);

    next->color_lut = std::make_unique<Lut3D>();
    next->grain_field.assign(field_size, GrainSample());

    const bool color_generated =
        next->color_lut->generate(
            size,
            [&](const Lut3D::RGB& encoded,
                Lut3D::RGB& converted) {

                const FilmPipeline::Result result =
                    next->pipeline->process(
                        next->input_transform->to_ap0(encoded));

                if (!result.valid) {
                    return false;
                }

                converted =
                    settings.output_profile == 1
                        ? result.rec709_gamma24
                        : result.ap0;

                const int r =
                    static_cast<int>(
                        std::lround(encoded[0] * (size - 1)));

                const int g =
                    static_cast<int>(
                        std::lround(encoded[1] * (size - 1)));

                const int b =
                    static_cast<int>(
                        std::lround(encoded[2] * (size - 1)));

                const std::size_t index =
                    static_cast<std::size_t>(
                        (b * size + g) * size + r);

                next->grain_field[index] = {{
                    result.negative_granularity_sigma.red,
                    result.negative_granularity_sigma.green,
                    result.negative_granularity_sigma.blue,
                    result.print_granularity_sigma.red,
                    result.print_granularity_sigma.green,
                    result.print_granularity_sigma.blue
                }};

                return true;
            });

    if (!color_generated) {
        error = "could not generate FilmViz OFX color LUT";
        return false;
    }

    if (settings.halation_enabled
        && settings.halation_strength > 0.0f
        && settings.halation_radius > 0.0f) {

        next->negative_exposure_lut =
            std::make_unique<Lut3D>();

        const bool exposure_generated =
            next->negative_exposure_lut->generate(
                size,
                [&](const Lut3D::RGB& encoded,
                    Lut3D::RGB& exposure_rgb) {

                    FilmExposure exposure;

                    if (!next->pipeline->negative_exposure(
                            next->input_transform->to_ap0(encoded),
                            exposure)) {

                        return false;
                    }

                    exposure_rgb = {{
                        exposure.red,
                        exposure.green,
                        exposure.blue
                    }};

                    return true;
                });

        if (!exposure_generated) {
            error = "could not generate FilmViz OFX negative-exposure LUT";
            return false;
        }
    }

    cache_ = std::move(next);
    ++revision_;
    return true;
}

std::uint64_t
FilmVizOfxProcessor::cache_revision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

bool
FilmVizOfxProcessor::gpu_snapshot(
    FilmVizOfxGpuSnapshot& snapshot,
    std::string& error)
{
    error.clear();

    std::lock_guard<std::mutex> lock(mutex_);

    if (!cache_
        || !cache_->pipeline
        || !cache_->color_lut
        || !cache_->color_lut->valid()) {

        error = "FilmViz OFX processor is not configured";
        return false;
    }

    const int size = cache_->settings.lut_size;
    const std::size_t count =
        static_cast<std::size_t>(size)
        * static_cast<std::size_t>(size)
        * static_cast<std::size_t>(size);

    const bool wants_halation =
        cache_->settings.halation_enabled
        && cache_->settings.halation_strength > 0.0f
        && cache_->settings.halation_radius > 0.0f;

    if (wants_halation
        && cache_->negative_exposure_lut
        && !cache_->halation_development_lut) {

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

        for (int b = 0; b < size; ++b) {
            for (int g = 0; g < size; ++g) {
                for (int r = 0; r < size; ++r) {
                    const Lut3D::RGB& exposure =
                        cache_->negative_exposure_lut->at(r, g, b);

                    for (int channel = 0; channel < 3; ++channel) {
                        const float value =
                            std::log10(
                                std::max(
                                    exposure[channel],
                                    1e-20f));

                        log_min[channel] =
                            std::min(log_min[channel], value);

                        log_max[channel] =
                            std::max(log_max[channel], value);
                    }
                }
            }
        }

        // Halation only adds scattered exposure. The configured record-scatter
        // maximum is 0.22, so a modest upper pad safely covers the global
        // exposure increase while keeping the development LUT well resolved.
        constexpr float low_padding = 0.02f;
        constexpr float high_padding = 0.15f;

        for (int channel = 0; channel < 3; ++channel) {
            log_min[channel] -= low_padding;
            log_max[channel] += high_padding;

            if (!(log_max[channel] > log_min[channel])) {
                error = "invalid FilmViz OFX Metal halation exposure range";
                return false;
            }
        }

        cache_->halation_log_min = log_min;
        cache_->halation_log_max = log_max;
        cache_->halation_development_lut =
            std::make_unique<Lut3D>();
        cache_->halation_grain_field.assign(
            count,
            GrainSample());

        const bool generated =
            cache_->halation_development_lut->generate(
                size,
                [&](const Lut3D::RGB& lookup_input,
                    Lut3D::RGB& converted) {

                    FilmExposure exposure;
                    float* channels[3] = {
                        &exposure.red,
                        &exposure.green,
                        &exposure.blue
                    };

                    for (int channel = 0; channel < 3; ++channel) {
                        const float log_value =
                            log_min[channel]
                            + lookup_input[channel]
                                * (log_max[channel]
                                   - log_min[channel]);

                        *channels[channel] =
                            std::pow(10.0f, log_value);
                    }

                    const FilmPipeline::Result result =
                        cache_->pipeline->process_negative_exposure(exposure);

                    if (!result.valid) {
                        return false;
                    }

                    converted =
                        cache_->settings.output_profile == 1
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

                    cache_->halation_grain_field[index] = {{
                        result.negative_granularity_sigma.red,
                        result.negative_granularity_sigma.green,
                        result.negative_granularity_sigma.blue,
                        result.print_granularity_sigma.red,
                        result.print_granularity_sigma.green,
                        result.print_granularity_sigma.blue
                    }};

                    return true;
                });

        if (!generated) {
            cache_->halation_development_lut.reset();
            cache_->halation_grain_field.clear();
            error = "could not generate FilmViz OFX Metal halation development LUT";
            return false;
        }

        ++revision_;
    }

    snapshot = FilmVizOfxGpuSnapshot();
    snapshot.revision = revision_;
    snapshot.lut_size = size;
    snapshot.input_profile = cache_->settings.input_profile;
    snapshot.output_profile = cache_->settings.output_profile;

    const auto pack_lut =
        [size](const Lut3D& lut,
               std::vector<float>& packed) {
            packed.resize(
                static_cast<std::size_t>(size)
                * static_cast<std::size_t>(size)
                * static_cast<std::size_t>(size)
                * 4u);

            std::size_t index = 0;
            for (int b = 0; b < size; ++b) {
                for (int g = 0; g < size; ++g) {
                    for (int r = 0; r < size; ++r) {
                        const Lut3D::RGB& value = lut.at(r, g, b);
                        packed[index++] = value[0];
                        packed[index++] = value[1];
                        packed[index++] = value[2];
                        packed[index++] = 0.0f;
                    }
                }
            }
        };

    const auto pack_grain =
        [](const std::vector<GrainSample>& field,
           std::vector<float>& negative,
           std::vector<float>& print) {
            negative.resize(field.size() * 4u);
            print.resize(field.size() * 4u);

            for (std::size_t i = 0; i < field.size(); ++i) {
                const std::size_t base = i * 4u;
                negative[base + 0u] = field[i][0];
                negative[base + 1u] = field[i][1];
                negative[base + 2u] = field[i][2];
                negative[base + 3u] = 0.0f;
                print[base + 0u] = field[i][3];
                print[base + 1u] = field[i][4];
                print[base + 2u] = field[i][5];
                print[base + 3u] = 0.0f;
            }
        };

    pack_lut(*cache_->color_lut, snapshot.color_lut_rgba);
    pack_grain(
        cache_->grain_field,
        snapshot.grain_negative_rgba,
        snapshot.grain_print_rgba);

    if (cache_->negative_exposure_lut) {
        pack_lut(
            *cache_->negative_exposure_lut,
            snapshot.negative_exposure_lut_rgba);
    }

    if (cache_->halation_development_lut) {
        pack_lut(
            *cache_->halation_development_lut,
            snapshot.halation_lut_rgba);

        pack_grain(
            cache_->halation_grain_field,
            snapshot.halation_grain_negative_rgba,
            snapshot.halation_grain_print_rgba);

        snapshot.halation_log_min = cache_->halation_log_min;
        snapshot.halation_log_max = cache_->halation_log_max;
        snapshot.halation_available = true;
    }

    return true;
}

bool
FilmVizOfxProcessor::render(
    const FilmVizOfxFrame& source,
    const FilmVizOfxFrame& destination,
    int render_x1,
    int render_y1,
    int render_x2,
    int render_y2,
    double time,
    const Abort& abort,
    std::string& error)
{
    error.clear();

    std::lock_guard<std::mutex> lock(mutex_);

    if (!cache_
        || !cache_->pipeline
        || !cache_->input_transform
        || !cache_->color_lut) {

        error = "FilmViz OFX processor is not configured";
        return false;
    }

    const FilmVizOfxRenderSettings settings = cache_->settings;
    const int size = settings.lut_size;

    render_x1 = std::max(render_x1, destination.x1);
    render_y1 = std::max(render_y1, destination.y1);
    render_x2 = std::min(render_x2, destination.x2);
    render_y2 = std::min(render_y2, destination.y2);

    if (render_x1 >= render_x2
        || render_y1 >= render_y2) {

        return true;
    }

    const auto aborted =
        [&]() {
            return abort && abort();
        };

    const std::uint32_t frame_seed =
        settings.grain_seed
        ^ static_cast<std::uint32_t>(
            std::llround(time * 1000.0));

    const bool use_grain =
        settings.grain_enabled
        && (settings.negative_grain > 0.0f
            || settings.print_grain > 0.0f);

    const bool use_halation =
        settings.halation_enabled
        && settings.halation_strength > 0.0f
        && settings.halation_radius > 0.0f
        && cache_->negative_exposure_lut;

    std::vector<FilmExposure> halation_exposure;
    std::vector<float> halation_ap0;
    std::unique_ptr<Lut3D> halation_lut;
    std::vector<GrainSample> halation_grain_field;
    std::array<float, 3> log_min = {{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> log_max = {{1.0f, 1.0f, 1.0f}};

    const int source_width = source.x2 - source.x1;
    const int source_height = source.y2 - source.y1;

    if (use_halation) {
        if (source_width <= 0
            || source_height <= 0) {

            error = "invalid OFX source bounds for halation";
            return false;
        }

        const std::size_t pixel_count =
            static_cast<std::size_t>(source_width)
            * static_cast<std::size_t>(source_height);

        halation_exposure.assign(pixel_count, FilmExposure());
        halation_ap0.assign(pixel_count * 3u, 0.0f);

        std::atomic<int> next_row(0);
        const int worker_count =
            FilmVizThreading::effective_thread_count(source_height);

        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(worker_count));

        for (int worker = 0;
             worker < worker_count;
             ++worker) {

            workers.emplace_back(
                [&]() {
                    while (!aborted()) {
                        const int local_y =
                            next_row.fetch_add(
                                1,
                                std::memory_order_relaxed);

                        if (local_y >= source_height) {
                            break;
                        }

                        const int y = source.y1 + local_y;

                        for (int local_x = 0;
                             local_x < source_width;
                             ++local_x) {

                            const int x = source.x1 + local_x;
                            const float* src = source_pixel(source, x, y);

                            if (!src) {
                                continue;
                            }

                            const Lut3D::RGB encoded = {{
                                src[0],
                                src[1],
                                src[2]
                            }};

                            const std::array<float, 3> ap0 =
                                cache_->input_transform->to_ap0(encoded);

                            const std::size_t pixel =
                                static_cast<std::size_t>(local_y)
                                * static_cast<std::size_t>(source_width)
                                + static_cast<std::size_t>(local_x);

                            halation_ap0[pixel * 3u + 0] = ap0[0];
                            halation_ap0[pixel * 3u + 1] = ap0[1];
                            halation_ap0[pixel * 3u + 2] = ap0[2];

                            const Lut3D::RGB exposure =
                                cache_->negative_exposure_lut->sample_tetrahedral(
                                    encoded);

                            halation_exposure[pixel] = {
                                exposure[0],
                                exposure[1],
                                exposure[2]
                            };
                        }
                    }
                });
        }

        for (std::thread& worker : workers) {
            worker.join();
        }

        if (aborted()) {
            error = "render aborted";
            return false;
        }

        HalationModel::Settings halation_settings;
        halation_settings.strength = settings.halation_strength;
        halation_settings.radius_pixels = settings.halation_radius;
        halation_settings.threshold = settings.halation_threshold;

        if (!HalationModel::apply(
                halation_exposure,
                halation_ap0,
                source_width,
                source_height,
                halation_settings,
                aborted)) {

            error =
                aborted()
                    ? "render aborted"
                    : "FilmViz OFX halation failed";

            return false;
        }

        log_min = {{
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity()
        }};

        log_max = {{
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()
        }};

        for (const FilmExposure& exposure : halation_exposure) {
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
                        std::max(values[channel], 1e-20f));

                log_min[channel] =
                    std::min(log_min[channel], log_value);

                log_max[channel] =
                    std::max(log_max[channel], log_value);
            }
        }

        constexpr float log_padding = 0.02f;

        for (int channel = 0;
             channel < 3;
             ++channel) {

            if (!std::isfinite(log_min[channel])
                || !std::isfinite(log_max[channel])) {

                error = "invalid halation exposure range";
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

        halation_lut = std::make_unique<Lut3D>();
        halation_grain_field.assign(
            static_cast<std::size_t>(size * size * size),
            GrainSample());

        const bool generated =
            halation_lut->generate(
                size,
                [&](const Lut3D::RGB& lookup_input,
                    Lut3D::RGB& converted) {

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
                            std::pow(10.0f, log_value);
                    }

                    const FilmPipeline::Result result =
                        cache_->pipeline->process_negative_exposure(exposure);

                    if (!result.valid) {
                        return false;
                    }

                    converted =
                        settings.output_profile == 1
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

                    halation_grain_field[index] = {{
                        result.negative_granularity_sigma.red,
                        result.negative_granularity_sigma.green,
                        result.negative_granularity_sigma.blue,
                        result.print_granularity_sigma.red,
                        result.print_granularity_sigma.green,
                        result.print_granularity_sigma.blue
                    }};

                    return true;
                });

        if (!generated) {
            error = "could not generate FilmViz OFX halation development LUT";
            return false;
        }
    }

    std::atomic<int> next_render_row(render_y1);
    std::atomic<bool> failed(false);

    const int render_height = render_y2 - render_y1;
    const int worker_count =
        FilmVizThreading::effective_thread_count(render_height);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(worker_count));

    for (int worker = 0;
         worker < worker_count;
         ++worker) {

        workers.emplace_back(
            [&]() {
                while (!failed.load(std::memory_order_relaxed)
                       && !aborted()) {

                    const int y =
                        next_render_row.fetch_add(
                            1,
                            std::memory_order_relaxed);

                    if (y >= render_y2) {
                        break;
                    }

                    for (int x = render_x1;
                         x < render_x2;
                         ++x) {

                        const float* src = source_pixel(source, x, y);
                        float* dst = destination_pixel(destination, x, y);

                        if (!dst) {
                            failed.store(true, std::memory_order_relaxed);
                            break;
                        }

                        if (!src) {
                            dst[0] = 0.0f;
                            dst[1] = 0.0f;
                            dst[2] = 0.0f;
                            dst[3] = 0.0f;
                            continue;
                        }

                        Lut3D::RGB lookup_input;
                        const Lut3D* render_lut = cache_->color_lut.get();
                        const std::vector<GrainSample>* grain_field =
                            &cache_->grain_field;

                        if (use_halation) {
                            const int local_x = x - source.x1;
                            const int local_y = y - source.y1;

                            if (local_x < 0
                                || local_x >= source_width
                                || local_y < 0
                                || local_y >= source_height) {

                                dst[0] = 0.0f;
                                dst[1] = 0.0f;
                                dst[2] = 0.0f;
                                dst[3] = src[3];
                                continue;
                            }

                            const std::size_t pixel =
                                static_cast<std::size_t>(local_y)
                                * static_cast<std::size_t>(source_width)
                                + static_cast<std::size_t>(local_x);

                            const FilmExposure& exposure =
                                halation_exposure[pixel];

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
                                        std::max(values[channel], 1e-20f));

                                lookup_input[channel] =
                                    clamp01(
                                        (log_value - log_min[channel])
                                        / (log_max[channel] - log_min[channel]));
                            }

                            render_lut = halation_lut.get();
                            grain_field = &halation_grain_field;
                        }
                        else {
                            lookup_input = {{
                                src[0],
                                src[1],
                                src[2]
                            }};
                        }

                        const Lut3D::RGB converted =
                            render_lut->sample_tetrahedral(lookup_input);

                        GrainSample sigma =
                            sample_field(
                                *grain_field,
                                size,
                                lookup_input);

                        std::array<float, 3> density_noise = {{0.0f, 0.0f, 0.0f}};

                        if (use_grain) {
                            for (int channel = 0;
                                 channel < 3;
                                 ++channel) {

                                const float negative_noise =
                                    settings.negative_grain
                                    * sigma[channel]
                                    * spatial_normal(
                                        frame_seed,
                                        x,
                                        y,
                                        0,
                                        channel,
                                        settings.grain_size);

                                const float print_noise =
                                    settings.print_grain
                                    * sigma[channel + 3]
                                    * spatial_normal(
                                        frame_seed,
                                        x,
                                        y,
                                        1,
                                        channel,
                                        settings.grain_size);

                                density_noise[channel] =
                                    negative_noise
                                    - print_noise;
                            }

                            density_noise =
                                ImageProcessor::mix_grain_chroma(
                                    density_noise,
                                    settings.grain_chroma);
                        }

                        for (int channel = 0;
                             channel < 3;
                             ++channel) {

                            float linear =
                                to_linear(
                                    converted[channel],
                                    settings.output_profile);

                            if (use_grain) {
                                linear *=
                                    std::pow(
                                        10.0f,
                                        density_noise[channel]);
                            }

                            dst[channel] =
                                clamp01(
                                    from_linear(
                                        linear,
                                        settings.output_profile));
                        }

                        dst[3] = src[3];
                    }
                }
            });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    if (aborted()) {
        error = "render aborted";
        return false;
    }

    if (failed.load(std::memory_order_relaxed)) {
        error = "FilmViz OFX image access failed";
        return false;
    }

    return true;
}
