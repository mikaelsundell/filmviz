// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct FilmVizOfxTransformKey
{
    std::string negative_profile;
    int input_profile = 0;
    int output_profile = 1;
    int lut_size = 33;

    float push_pull_stops = 0.0f;
    float middle_gray = 0.18f;
    float printer_temperature = 3200.0f;
    float negative_bleach_bypass = 0.0f;
    float print_bleach_bypass = 0.0f;
    float printer_light_red = 25.0f;
    float printer_light_green = 25.0f;
    float printer_light_blue = 25.0f;

    bool operator==(const FilmVizOfxTransformKey& other) const;
};

using FilmVizOfxCachedRGB = std::array<float, 3>;
using FilmVizOfxCachedGrain = std::array<float, 6>;

struct FilmVizOfxPrebakedData
{
    int lut_size = 0;
    std::vector<FilmVizOfxCachedRGB> color_lut;
    std::vector<FilmVizOfxCachedGrain> grain_field;
    std::vector<FilmVizOfxCachedRGB> negative_exposure_lut;
    std::array<float, 3> development_log_min = {{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> development_log_max = {{1.0f, 1.0f, 1.0f}};
};

std::uint64_t filmviz_ofx_transform_hash(
    const FilmVizOfxTransformKey& key);

std::string filmviz_ofx_transform_name(
    const FilmVizOfxTransformKey& key);

std::string filmviz_ofx_prebaked_filename(
    const std::string& directory,
    const FilmVizOfxTransformKey& key);

bool filmviz_ofx_load_prebaked(
    const std::string& filename,
    const FilmVizOfxTransformKey& key,
    FilmVizOfxPrebakedData& data,
    std::string& error);

bool filmviz_ofx_save_prebaked(
    const std::string& filename,
    const FilmVizOfxTransformKey& key,
    const FilmVizOfxPrebakedData& data,
    std::string& error);
