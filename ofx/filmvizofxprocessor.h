// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmcolorresponse.h"
#include "filmdata.h"
#include "filmformat.h"
#include "negativeprofile.h"
#include "printprofile.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class FilmPipeline;
class InputTransform;
class Lut3D;

struct FilmVizOfxFrame
{
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    std::ptrdiff_t row_bytes = 0;
    float* data = nullptr;
};

struct FilmVizOfxRenderSettings
{
    std::string negative_profile =
        NegativeProfileCatalog::default_profile().identifier;
    std::string print_profile =
        PrintProfileCatalog::default_profile().identifier;

    int input_profile = 0;
    int output_profile = 1;
    int lut_size = 33;
    int threads = 0;

    float exposure_stops = 0.0f;
    float negative_flash_percent = 0.0f;
    float print_flash_percent = 0.0f;
    float push_pull_stops = 0.0f;
    float color_density = 0.0f;
    float warm_tone_separation =
        FilmColorResponse::standard_warm_tone_separation;
    float middle_gray = 0.18f;
    float printer_temperature = 3200.0f;

    float negative_bleach_bypass = 0.0f;
    float print_bleach_bypass = 0.0f;

    float printer_light_red = 25.0f;
    float printer_light_green = 25.0f;
    float printer_light_blue = 25.0f;
    float printer_light_master = 0.0f;

    std::string film_format =
        FilmFormatCatalog::default_format().identifier;
    float image_width_mm =
        FilmFormatCatalog::default_format().image_width_mm;
    float negative_mtf_amount = 0.0f;
    float print_mtf_amount = 0.0f;

    bool grain_enabled = false;
    float negative_grain = 0.0f;
    float print_grain = 0.0f;
    float grain_size = 1.0f;
    float grain_chroma = 1.0f;
    std::uint32_t grain_seed = 1u;

    bool halation_enabled = false;
    float halation_strength = 0.0f;
    float halation_radius = 12.0f;
    float halation_threshold = 0.7f;

    bool operator==(const FilmVizOfxRenderSettings& other) const;
    bool operator!=(const FilmVizOfxRenderSettings& other) const
    {
        return !(*this == other);
    }
};


struct FilmVizOfxGpuSnapshot
{
    std::uint64_t revision = 0;
    std::uint64_t transform_hash = 0;
    int lut_size = 0;
    int input_profile = 0;
    int output_profile = 1;

    std::vector<float> color_lut_rgba;
    std::vector<float> grain_negative_rgba;
    std::vector<float> grain_print_rgba;

    std::vector<float> negative_exposure_lut_rgba;
    std::vector<float> halation_lut_rgba;
    std::vector<float> halation_grain_negative_rgba;
    std::vector<float> halation_grain_print_rgba;

    std::array<float, 3> halation_log_min = {{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> halation_log_max = {{1.0f, 1.0f, 1.0f}};
    bool halation_available = false;
};

class FilmVizOfxProcessor
{
public:
    using Abort = std::function<bool()>;

    FilmVizOfxProcessor();
    ~FilmVizOfxProcessor();

    FilmVizOfxProcessor(const FilmVizOfxProcessor&) = delete;
    FilmVizOfxProcessor& operator=(const FilmVizOfxProcessor&) = delete;

    bool configure(
        const FilmVizOfxRenderSettings& settings,
        const std::string& resources_directory,
        std::string& error);

    // True when this instance can use the requested immutable transform
    // without a cache lookup or rebuild. Runtime-only controls are ignored.
    bool has_transform(
        const FilmVizOfxRenderSettings& settings,
        const std::string& resources_directory) const;

    bool render(
        const FilmVizOfxFrame& source,
        const FilmVizOfxFrame& destination,
        int render_x1,
        int render_y1,
        int render_x2,
        int render_y2,
        double time,
        const Abort& abort,
        std::string& error);

    std::uint64_t cache_revision() const;

    bool gpu_snapshot(
        FilmVizOfxGpuSnapshot& snapshot,
        std::string& error);

    // Write the currently configured transform to a persistent OFX cache.
    // Used by the build-time pregenerator and available for diagnostics.
    bool write_prebaked_cache(
        const std::string& directory,
        bool include_negative_exposure,
        std::string& error);

    std::string transform_cache_name() const;

private:
    struct Cache;

    mutable std::mutex mutex_;
    std::shared_ptr<Cache> cache_;
    FilmVizOfxRenderSettings settings_;
    std::uint64_t revision_ = 0;
};
