// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmvizofxprocessor.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct FilmVizMetalPreviewResult
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> display_rgb;
    std::vector<float> scope_rgb;
};

class FilmVizMetalPreview
{
public:
    struct Impl;

    FilmVizMetalPreview();
    ~FilmVizMetalPreview();

    FilmVizMetalPreview(const FilmVizMetalPreview&) = delete;
    FilmVizMetalPreview& operator=(const FilmVizMetalPreview&) = delete;

    static bool available();

    void invalidate_profiles();

    bool render(
        const std::string& input_filename,
        const std::string& resources_directory,
        const FilmVizOfxRenderSettings& settings,
        int max_dimension,
        double time,
        FilmVizMetalPreviewResult& result,
        std::string& error);

private:
    std::unique_ptr<Impl> impl_;
};
