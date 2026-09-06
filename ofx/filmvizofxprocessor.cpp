// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizofxprocessor.h"

#include "filmvizofxcache.h"
#include "filmvizofxlog.h"
#include "filmpipeline.h"
#include "granularitymodel.h"
#include "halationmodel.h"
#include "imageprocessor.h"
#include "inputtransform.h"
#include "lut3d.h"
#include "negativeprofile.h"
#include "printprofile.h"
#include "spatialresponsemodel.h"
#include "threading.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
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

FilmVizOfxTransformKey
transform_key(
    const FilmVizOfxRenderSettings& settings)
{
    FilmVizOfxTransformKey key;
    key.negative_profile = settings.negative_profile;
    key.print_profile = settings.print_profile;
    key.input_profile = settings.input_profile;
    key.output_profile = settings.output_profile;
    key.lut_size = settings.lut_size;
    key.push_pull_stops = settings.push_pull_stops;
    key.negative_flash_percent = settings.negative_flash_percent;
    key.print_flash_percent = settings.print_flash_percent;
    key.middle_gray = settings.middle_gray;
    key.printer_temperature = settings.printer_temperature;
    key.negative_bleach_bypass = settings.negative_bleach_bypass;
    key.print_bleach_bypass = settings.print_bleach_bypass;
    key.printer_light_red = settings.printer_light_red;
    key.printer_light_green = settings.printer_light_green;
    key.printer_light_blue = settings.printer_light_blue;
    key.printer_light_master = settings.printer_light_master;
    return key;
}

std::string
settings_summary(
    const FilmVizOfxRenderSettings& settings)
{
    std::ostringstream stream;
    stream
        << "negative=" << settings.negative_profile
        << " print=" << settings.print_profile
        << " input=" << settings.input_profile
        << " output=" << settings.output_profile
        << " lut=" << settings.lut_size
        << " exposure=" << settings.exposure_stops
        << " negative_flash=" << settings.negative_flash_percent
        << " print_flash=" << settings.print_flash_percent
        << " push_pull=" << settings.push_pull_stops
        << " lights="
        << settings.printer_light_red << ','
        << settings.printer_light_green << ','
        << settings.printer_light_blue
        << " master=" << settings.printer_light_master
        << " temp=" << settings.printer_temperature
        << " neg_bypass=" << settings.negative_bleach_bypass
        << " print_bypass=" << settings.print_bleach_bypass;
    return stream.str();
}

std::string
runtime_cache_directory()
{
    if (const char* override_path = std::getenv("FILMVIZ_OFX_CACHE_DIR")) {
        if (*override_path) {
            return override_path;
        }
    }

#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return
            (std::filesystem::path(home)
             / "Library"
             / "Caches"
             / "FilmViz"
             / "ofx")
                .string();
    }
#elif defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        return
            (std::filesystem::path(local)
             / "FilmViz"
             / "Cache"
             / "ofx")
                .string();
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return
            (std::filesystem::path(home)
             / ".cache"
             / "filmviz"
             / "ofx")
                .string();
    }
#endif

    return std::string();
}

bool
load_lut_values(
    const std::vector<FilmVizOfxCachedRGB>& values,
    int size,
    std::unique_ptr<Lut3D>& lut)
{
    const std::size_t expected =
        static_cast<std::size_t>(size)
        * static_cast<std::size_t>(size)
        * static_cast<std::size_t>(size);

    if (values.size() != expected) {
        return false;
    }

    lut = std::make_unique<Lut3D>();

    return lut->assign(
        size,
        values);
}

} // namespace

struct FilmVizOfxProcessor::Cache
{
    FilmVizOfxTransformKey key;
    FilmVizOfxRenderSettings transform_settings;
    std::string resources_directory;
    std::shared_ptr<FilmPipeline> pipeline;
    std::unique_ptr<InputTransform> input_transform;
    std::unique_ptr<SpatialResponseModel> spatial_response;
    // Two-stage cached transform:
    // encoded input -> raw negative exposure -> developed/viewed output.
    // Exposure is applied between these two LUTs and therefore never changes
    // the expensive transform cache key.
    std::unique_ptr<Lut3D> negative_exposure_lut;
    std::unique_ptr<Lut3D> color_lut;
    std::vector<GrainSample> grain_field;
    std::array<float, 3> development_log_min = {{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> development_log_max = {{1.0f, 1.0f, 1.0f}};

};

bool
FilmVizOfxRenderSettings::operator==(
    const FilmVizOfxRenderSettings& other) const
{
    return
        negative_profile == other.negative_profile
        && print_profile == other.print_profile
        && input_profile == other.input_profile
        && output_profile == other.output_profile
        && lut_size == other.lut_size
        && threads == other.threads
        && exposure_stops == other.exposure_stops
        && negative_flash_percent == other.negative_flash_percent
        && print_flash_percent == other.print_flash_percent
        && push_pull_stops == other.push_pull_stops
        && middle_gray == other.middle_gray
        && printer_temperature == other.printer_temperature
        && negative_bleach_bypass == other.negative_bleach_bypass
        && print_bleach_bypass == other.print_bleach_bypass
        && printer_light_red == other.printer_light_red
        && printer_light_green == other.printer_light_green
        && printer_light_blue == other.printer_light_blue
        && printer_light_master == other.printer_light_master
        && film_format == other.film_format
        && image_width_mm == other.image_width_mm
        && negative_mtf_amount == other.negative_mtf_amount
        && print_mtf_amount == other.print_mtf_amount
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
FilmVizOfxProcessor::has_transform(
    const FilmVizOfxRenderSettings& settings,
    const std::string& resources_directory) const
{
    const FilmVizOfxTransformKey key =
        transform_key(settings);

    std::lock_guard<std::mutex> lock(mutex_);

    return
        cache_
        && cache_->resources_directory == resources_directory
        && cache_->key == key;
}

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
        || !finite_setting(settings.negative_flash_percent)
        || settings.negative_flash_percent < 0.0f
        || settings.negative_flash_percent > 25.0f
        || !finite_setting(settings.print_flash_percent)
        || settings.print_flash_percent < 0.0f
        || settings.print_flash_percent > 25.0f
        || !finite_setting(settings.middle_gray)
        || settings.middle_gray <= 0.0f
        || !finite_setting(settings.printer_temperature)
        || settings.printer_temperature <= 0.0f
        || settings.negative_bleach_bypass < 0.0f
        || settings.negative_bleach_bypass > 1.0f
        || settings.print_bleach_bypass < 0.0f
        || settings.print_bleach_bypass > 1.0f
        || settings.printer_light_red + settings.printer_light_master < 0.0f
        || settings.printer_light_red + settings.printer_light_master > 50.0f
        || settings.printer_light_green + settings.printer_light_master < 0.0f
        || settings.printer_light_green + settings.printer_light_master > 50.0f
        || settings.printer_light_blue + settings.printer_light_master < 0.0f
        || settings.printer_light_blue + settings.printer_light_master > 50.0f
        || !FilmFormatCatalog::find(settings.film_format)
        || !finite_setting(settings.image_width_mm)
        || settings.image_width_mm <= 0.0f
        || settings.negative_mtf_amount < 0.0f
        || settings.negative_mtf_amount > 2.0f
        || settings.print_mtf_amount < 0.0f
        || settings.print_mtf_amount > 2.0f
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

    FilmVizThreading::set_thread_count(settings.threads);

    const FilmVizOfxTransformKey key =
        transform_key(settings);

    const std::uint64_t key_hash =
        filmviz_ofx_transform_hash(key);

    FilmVizOfxLog::Scope configure_scope(
        "configure",
        settings_summary(settings)
            + " cache="
            + filmviz_ofx_transform_name(key));

    std::lock_guard<std::mutex> instance_lock(mutex_);

    // Runtime-only controls (Exposure, measured MTF, grain and halation) do
    // not change the shared transform assets or their Metal uploads.
    settings_ = settings;

    if (cache_
        && cache_->resources_directory == resources_directory
        && cache_->key == key) {

        configure_scope.finish("result=instance_hit");
        return true;
    }

    // Shared for every FilmViz node in the Resolve process. weak_ptr lets the
    // cache disappear automatically when the last node using it is destroyed.
    static std::mutex registry_mutex;
    static std::mutex registry_build_mutex;
    static std::unordered_map<
        std::uint64_t,
        std::weak_ptr<Cache>> registry;

    std::shared_ptr<Cache> shared;

    {
        const std::lock_guard<std::mutex> registry_lock(
            registry_mutex);

        const auto found =
            registry.find(key_hash);

        if (found != registry.end()) {
            shared = found->second.lock();

            if (shared
                && (shared->resources_directory != resources_directory
                    || !(shared->key == key))) {
                shared.reset();
            }
        }
    }

    // Only one node builds a missing transform at a time. Recheck after
    // entering the build gate so simultaneous Resolve nodes cannot duplicate
    // the expensive spectral/LUT generation work.
    std::unique_lock<std::mutex> build_lock;

    if (!shared) {
        build_lock =
            std::unique_lock<std::mutex>(registry_build_mutex);

        const std::lock_guard<std::mutex> registry_lock(
            registry_mutex);

        const auto found =
            registry.find(key_hash);

        if (found != registry.end()) {
            shared = found->second.lock();

            if (shared
                && (shared->resources_directory != resources_directory
                    || !(shared->key == key))) {
                shared.reset();
            }
        }
    }

    if (shared) {
        cache_ = shared;
        ++revision_;

        FilmVizOfxLog::write(
            "cache",
            "result=global_hit name="
                + filmviz_ofx_transform_name(key));
    }
    else {
        auto next =
            std::make_shared<Cache>();

        next->key = key;
        next->transform_settings = settings;
        next->transform_settings.exposure_stops = 0.0f;
        next->transform_settings.grain_enabled = false;
        next->transform_settings.halation_enabled = false;
        next->resources_directory = resources_directory;
        next->input_transform =
            std::make_unique<InputTransform>(
                input_encoding(settings.input_profile));

        const auto* negative_profile =
            NegativeProfileCatalog::find(settings.negative_profile);
        const auto* print_profile =
            PrintProfileCatalog::find(
                settings.print_profile == "none"
                    ? PrintProfileCatalog::default_profile().identifier
                    : settings.print_profile);

        if (!negative_profile || !print_profile) {
            error = "could not resolve OFX MTF profile metadata";
            configure_scope.finish("result=failed error=" + error);
            return false;
        }

        const std::filesystem::path resources(resources_directory);
        const std::filesystem::path negative_mtf =
            resources
            / negative_profile->resource_directory
            / (negative_profile->resource_prefix
               + "_modulation_transfer_function_curves.csv");
        const std::filesystem::path print_mtf =
            resources
            / print_profile->resource_directory
            / print_profile->mtf_filename;

        next->spatial_response =
            std::make_unique<SpatialResponseModel>();

        // MTF is an optional image-space control. A missing curve must not
        // prevent the zero-MTF production transform from loading; render()
        // reports the unavailable data only when MTF is actually enabled.
        if (!next->spatial_response->load(
                negative_mtf.string(),
                print_mtf.string())) {
            next->spatial_response.reset();
        }

        const int size = settings.lut_size;
        const std::size_t field_size =
            static_cast<std::size_t>(size * size * size);

        // A pre-generated/persistent cache contains every runtime transform
        // product needed by CPU and Metal. Do this lookup BEFORE initializing
        // FilmPipeline; a cache hit therefore skips profile parsing,
        // calibration, spectral setup and LUT generation entirely.
        bool loaded_prebaked = false;
        const std::string prebaked_directory =
            (std::filesystem::path(resources_directory)
             / "cache"
             / "ofx")
                .string();

        const std::string runtime_directory =
            runtime_cache_directory();

        const std::string candidates[] = {
            filmviz_ofx_prebaked_filename(
                prebaked_directory,
                key),
            runtime_directory.empty()
                ? std::string()
                : filmviz_ofx_prebaked_filename(
                    runtime_directory,
                    key)
        };

        for (int candidate_index = 0;
             candidate_index < 2;
             ++candidate_index) {

            const std::string& prebaked_file =
                candidates[candidate_index];

            if (prebaked_file.empty()) {
                continue;
            }

            FilmVizOfxPrebakedData prebaked;
            std::string prebaked_error;

            if (!filmviz_ofx_load_prebaked(
                    prebaked_file,
                    key,
                    prebaked,
                    prebaked_error)
                || prebaked.negative_exposure_lut.empty()) {
                continue;
            }

            next->grain_field.assign(
                prebaked.grain_field.begin(),
                prebaked.grain_field.end());

            loaded_prebaked =
                load_lut_values(
                    prebaked.color_lut,
                    size,
                    next->color_lut)
                && load_lut_values(
                    prebaked.negative_exposure_lut,
                    size,
                    next->negative_exposure_lut);

            if (loaded_prebaked) {
                next->development_log_min =
                    prebaked.development_log_min;
                next->development_log_max =
                    prebaked.development_log_max;

                FilmVizOfxLog::write(
                    "cache",
                    std::string("result=")
                        + (candidate_index == 0
                            ? "bundled_hit"
                            : "disk_hit")
                        + " file=" + prebaked_file);
                break;
            }
        }

        if (!loaded_prebaked) {
            next->pipeline =
                std::make_shared<FilmPipeline>();

            FilmPipeline::Settings pipeline_settings;
            pipeline_settings.resources_directory = resources_directory;
            pipeline_settings.negative_profile = settings.negative_profile;
            pipeline_settings.print_profile = settings.print_profile;
            pipeline_settings.negative_flash_percent =
                settings.negative_flash_percent;
            pipeline_settings.print_flash_percent =
                settings.print_flash_percent;

            // Exposure is intentionally NOT baked into the transform. The
            // first LUT ends at raw negative exposure; runtime exposure
            // multiplies FilmExposure by 2^stops before the cached development
            // LUT. This is mathematically identical to FilmPipeline's LogE
            // exposure offset.
            pipeline_settings.exposure_stops = 0.0f;
            pipeline_settings.push_pull_stops = settings.push_pull_stops;
            pipeline_settings.middle_gray = settings.middle_gray;
            pipeline_settings.printer_temperature_kelvin = settings.printer_temperature;
            pipeline_settings.negative_bleach_bypass = settings.negative_bleach_bypass;
            pipeline_settings.print_bleach_bypass = settings.print_bleach_bypass;
            pipeline_settings.printer_light_red = settings.printer_light_red;
            pipeline_settings.printer_light_green = settings.printer_light_green;
            pipeline_settings.printer_light_blue = settings.printer_light_blue;
            pipeline_settings.printer_light_master =
                settings.printer_light_master;

            const auto pipeline_start =
                std::chrono::steady_clock::now();

            if (!next->pipeline->initialize(pipeline_settings)) {
                error = next->pipeline->error();
                configure_scope.finish("result=pipeline_failed");
                return false;
            }

            const double pipeline_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - pipeline_start)
                    .count();

            FilmVizOfxLog::Scope lut_scope(
                "transform_generate",
                "cache=" + filmviz_ofx_transform_name(key));

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
                lut_scope.finish("result=exposure_lut_failed");
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

            for (int b = 0; b < size; ++b) {
                for (int g = 0; g < size; ++g) {
                    for (int r = 0; r < size; ++r) {
                        const Lut3D::RGB& exposure =
                            next->negative_exposure_lut->at(r, g, b);

                        for (int channel = 0; channel < 3; ++channel) {
                            const float log_value =
                                std::log10(
                                    std::max(
                                        exposure[channel],
                                        1e-20f));

                            log_min[channel] =
                                std::min(log_min[channel], log_value);
                            log_max[channel] =
                                std::max(log_max[channel], log_value);
                        }
                    }
                }
            }

            // Cover the full OFX Exposure control range and leave additional
            // headroom for halation scatter before logarithmic shaping.
            constexpr float exposure_stops = 8.0f;
            constexpr float log10_two = 0.3010299956639812f;
            constexpr float padding = 0.20f;

            for (int channel = 0; channel < 3; ++channel) {
                log_min[channel] -= exposure_stops * log10_two + padding;
                log_max[channel] += exposure_stops * log10_two + padding;

                if (!(log_max[channel] > log_min[channel])) {
                    error = "invalid FilmViz OFX development exposure domain";
                    lut_scope.finish("result=domain_failed");
                    return false;
                }
            }

            next->development_log_min = log_min;
            next->development_log_max = log_max;
            next->color_lut =
                std::make_unique<Lut3D>();
            next->grain_field.assign(
                field_size,
                GrainSample());

            const bool development_generated =
                next->color_lut->generate(
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
                                    * (log_max[channel] - log_min[channel]);

                            *channels[channel] =
                                std::pow(10.0f, log_value);
                        }

                        const FilmPipeline::Result result =
                            next->pipeline->process_negative_exposure(
                                exposure);

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

            if (!development_generated) {
                error = "could not generate FilmViz OFX development LUT";
                lut_scope.finish("result=development_lut_failed");
                return false;
            }

            lut_scope.finish("result=generated");

            std::ostringstream details;
            details
                << "result=generated"
                << " name=" << filmviz_ofx_transform_name(key)
                << " pipeline_ms=" << std::fixed << std::setprecision(3) << pipeline_ms
                << " model=two_stage";

            FilmVizOfxLog::write(
                "cache",
                details.str());

            if (!runtime_directory.empty()) {
                FilmVizOfxPrebakedData generated_data;
                generated_data.lut_size = size;
                generated_data.development_log_min =
                    next->development_log_min;
                generated_data.development_log_max =
                    next->development_log_max;
                generated_data.grain_field.assign(
                    next->grain_field.begin(),
                    next->grain_field.end());
                generated_data.color_lut.reserve(field_size);
                generated_data.negative_exposure_lut.reserve(field_size);

                for (int b = 0; b < size; ++b) {
                    for (int g = 0; g < size; ++g) {
                        for (int r = 0; r < size; ++r) {
                            generated_data.color_lut.push_back(
                                next->color_lut->at(r, g, b));
                            generated_data.negative_exposure_lut.push_back(
                                next->negative_exposure_lut->at(r, g, b));
                        }
                    }
                }

                std::string save_error;
                const std::string runtime_file =
                    filmviz_ofx_prebaked_filename(
                        runtime_directory,
                        key);

                if (filmviz_ofx_save_prebaked(
                        runtime_file,
                        key,
                        generated_data,
                        save_error)) {
                    FilmVizOfxLog::write(
                        "cache",
                        "result=disk_saved file=" + runtime_file);
                }
                else {
                    FilmVizOfxLog::write(
                        "cache",
                        "result=disk_save_failed error=" + save_error);
                }
            }

            // Runtime rendering uses only the cached transform products.
            // Release the heavyweight spectral/profile pipeline after a miss.
            next->pipeline.reset();
        }

        cache_ = next;
        ++revision_;

        {
            const std::lock_guard<std::mutex> registry_lock(
                registry_mutex);
            registry[key_hash] = next;
        }
    }

    configure_scope.finish(
        shared
            ? "result=global_hit"
            : "result=ready");

    return true;
}

std::uint64_t
FilmVizOfxProcessor::cache_revision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

std::string
FilmVizOfxProcessor::transform_cache_name() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!cache_) {
        return std::string();
    }

    return
        filmviz_ofx_transform_name(
            cache_->key);
}

bool
FilmVizOfxProcessor::write_prebaked_cache(
    const std::string& directory,
    bool include_negative_exposure,
    std::string& error)
{
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    if (!cache_
        || !cache_->color_lut
        || !cache_->negative_exposure_lut) {

        error = "FilmViz OFX processor is not configured";
        return false;
    }

    FilmVizOfxPrebakedData data;
    data.lut_size = cache_->key.lut_size;
    data.development_log_min = cache_->development_log_min;
    data.development_log_max = cache_->development_log_max;

    const int size = cache_->key.lut_size;
    const std::size_t count =
        static_cast<std::size_t>(size)
        * static_cast<std::size_t>(size)
        * static_cast<std::size_t>(size);

    data.color_lut.reserve(count);

    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                data.color_lut.push_back(
                    cache_->color_lut->at(r, g, b));
            }
        }
    }

    data.grain_field.assign(
        cache_->grain_field.begin(),
        cache_->grain_field.end());

    if (include_negative_exposure
        && cache_->negative_exposure_lut) {

        data.negative_exposure_lut.reserve(count);

        for (int b = 0; b < size; ++b) {
            for (int g = 0; g < size; ++g) {
                for (int r = 0; r < size; ++r) {
                    data.negative_exposure_lut.push_back(
                        cache_->negative_exposure_lut->at(r, g, b));
                }
            }
        }
    }

    const std::string filename =
        filmviz_ofx_prebaked_filename(
            directory,
            cache_->key);

    if (!filmviz_ofx_save_prebaked(
            filename,
            cache_->key,
            data,
            error)) {
        return false;
    }

    FilmVizOfxLog::write(
        "prebake",
        "file=" + filename
            + " cache="
            + filmviz_ofx_transform_name(cache_->key));

    return true;
}

bool
FilmVizOfxProcessor::gpu_snapshot(
    FilmVizOfxGpuSnapshot& snapshot,
    std::string& error)
{
    error.clear();

    std::lock_guard<std::mutex> lock(mutex_);

    if (!cache_
        || !cache_->color_lut
        || !cache_->color_lut->valid()
        || !cache_->negative_exposure_lut
        || !cache_->negative_exposure_lut->valid()) {

        error = "FilmViz OFX processor is not configured";
        return false;
    }

    const int size = cache_->key.lut_size;

    snapshot = FilmVizOfxGpuSnapshot();
    snapshot.revision = revision_;
    snapshot.transform_hash =
        filmviz_ofx_transform_hash(
            cache_->key);
    snapshot.lut_size = size;
    snapshot.input_profile = cache_->key.input_profile;
    snapshot.output_profile = cache_->key.output_profile;
    snapshot.halation_log_min = cache_->development_log_min;
    snapshot.halation_log_max = cache_->development_log_max;
    snapshot.halation_available = true;

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

    pack_lut(
        *cache_->color_lut,
        snapshot.color_lut_rgba);

    pack_lut(
        *cache_->negative_exposure_lut,
        snapshot.negative_exposure_lut_rgba);

    pack_grain(
        cache_->grain_field,
        snapshot.grain_negative_rgba,
        snapshot.grain_print_rgba);

    // Halation returns to the same negative-exposure boundary, so it reuses
    // the same development LUT and granularity field after spatial scatter.
    snapshot.halation_lut_rgba =
        snapshot.color_lut_rgba;
    snapshot.halation_grain_negative_rgba =
        snapshot.grain_negative_rgba;
    snapshot.halation_grain_print_rgba =
        snapshot.grain_print_rgba;

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

    std::shared_ptr<Cache> cache;
    FilmVizOfxRenderSettings settings;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache = cache_;
        settings = settings_;
    }

    if (!cache
        || !cache->color_lut
        || !cache->negative_exposure_lut) {

        error = "FilmViz OFX processor is not configured";
        return false;
    }

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
        && settings.halation_radius > 0.0f;

    const float exposure_scale =
        std::exp2(settings.exposure_stops);

    const auto exposure_lookup =
        [&](const FilmExposure& exposure) {
            const float values[3] = {
                exposure.red,
                exposure.green,
                exposure.blue
            };

            Lut3D::RGB lookup;

            for (int channel = 0; channel < 3; ++channel) {
                const float log_value =
                    std::log10(
                        std::max(values[channel], 1e-20f));

                lookup[channel] =
                    clamp01(
                        (log_value - cache->development_log_min[channel])
                        / (cache->development_log_max[channel]
                           - cache->development_log_min[channel]));
            }

            return lookup;
        };

    std::vector<FilmExposure> halation_exposure;
    std::vector<float> halation_ap0;

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

                            const Lut3D::RGB raw_exposure =
                                cache->negative_exposure_lut->sample_tetrahedral(
                                    encoded);

                            FilmExposure exposure;
                            exposure.red = raw_exposure[0] * exposure_scale;
                            exposure.green = raw_exposure[1] * exposure_scale;
                            exposure.blue = raw_exposure[2] * exposure_scale;

                            std::array<float, 3> ap0 =
                                cache->input_transform->to_ap0(encoded);

                            for (float& value : ap0) {
                                value *= exposure_scale;
                            }

                            const std::size_t pixel =
                                static_cast<std::size_t>(local_y)
                                * static_cast<std::size_t>(source_width)
                                + static_cast<std::size_t>(local_x);

                            halation_ap0[pixel * 3u + 0] = ap0[0];
                            halation_ap0[pixel * 3u + 1] = ap0[1];
                            halation_ap0[pixel * 3u + 2] = ap0[2];
                            halation_exposure[pixel] = exposure;
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

                        FilmExposure exposure;

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

                            exposure = halation_exposure[pixel];
                        }
                        else {
                            const Lut3D::RGB encoded = {{
                                src[0],
                                src[1],
                                src[2]
                            }};

                            const Lut3D::RGB raw_exposure =
                                cache->negative_exposure_lut->sample_tetrahedral(
                                    encoded);

                            exposure.red = raw_exposure[0] * exposure_scale;
                            exposure.green = raw_exposure[1] * exposure_scale;
                            exposure.blue = raw_exposure[2] * exposure_scale;
                        }

                        const Lut3D::RGB lookup_input =
                            exposure_lookup(exposure);

                        const Lut3D::RGB converted =
                            cache->color_lut->sample_tetrahedral(
                                lookup_input);

                        GrainSample sigma =
                            sample_field(
                                cache->grain_field,
                                size,
                                lookup_input);

                        std::array<float, 3> density_noise = {{
                            0.0f,
                            0.0f,
                            0.0f
                        }};

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

    const bool use_mtf =
        settings.negative_mtf_amount > 0.0f
        || settings.print_mtf_amount > 0.0f;

    if (use_mtf) {
        if (!cache->spatial_response
            || !cache->spatial_response->valid()) {
            error = "FilmViz OFX measured MTF response is unavailable";
            return false;
        }

        const int width = render_x2 - render_x1;
        const int height = render_y2 - render_y1;
        std::vector<float> rgb(
            static_cast<std::size_t>(width)
                * static_cast<std::size_t>(height)
                * 3u,
            0.0f);

        for (int y = render_y1; y < render_y2; ++y) {
            for (int x = render_x1; x < render_x2; ++x) {
                const float* pixel =
                    source_pixel(destination, x, y);

                if (!pixel) {
                    error = "FilmViz OFX MTF image access failed";
                    return false;
                }

                const std::size_t index =
                    (static_cast<std::size_t>(y - render_y1)
                         * static_cast<std::size_t>(width)
                     + static_cast<std::size_t>(x - render_x1))
                    * 3u;
                rgb[index + 0u] = pixel[0];
                rgb[index + 1u] = pixel[1];
                rgb[index + 2u] = pixel[2];
            }
        }

        SpatialResponseModel::Settings spatial_settings;
        spatial_settings.image_width_mm = settings.image_width_mm;
        spatial_settings.negative_amount = settings.negative_mtf_amount;
        spatial_settings.print_amount = settings.print_mtf_amount;
        spatial_settings.sampling_width_pixels = source_width;
        spatial_settings.gamma24_encoded = settings.output_profile == 1;

        if (!cache->spatial_response->apply(
                rgb,
                width,
                height,
                spatial_settings,
                aborted)) {
            error = aborted()
                ? "render aborted"
                : "FilmViz OFX measured MTF processing failed";
            return false;
        }

        for (int y = render_y1; y < render_y2; ++y) {
            for (int x = render_x1; x < render_x2; ++x) {
                float* pixel = destination_pixel(destination, x, y);
                const std::size_t index =
                    (static_cast<std::size_t>(y - render_y1)
                         * static_cast<std::size_t>(width)
                     + static_cast<std::size_t>(x - render_x1))
                    * 3u;
                pixel[0] = rgb[index + 0u];
                pixel[1] = rgb[index + 1u];
                pixel[2] = rgb[index + 2u];
            }
        }
    }

    return true;
}
