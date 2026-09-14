// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmvizofxprocessor.h"

#include <memory>
#include <string>

struct FilmVizOfxMetalFrame
{
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    std::ptrdiff_t row_bytes = 0;
    void* buffer = nullptr;
};

// Direct spectral Metal backend. This renderer does not sample a transform
// LUT: measured tables are uploaded once and the film pipeline is evaluated
// for each pixel with live controls.
class FilmVizDirectMetalProcessor
{
public:
    FilmVizDirectMetalProcessor();
    ~FilmVizDirectMetalProcessor();

    FilmVizDirectMetalProcessor(const FilmVizDirectMetalProcessor&) = delete;
    FilmVizDirectMetalProcessor& operator=(const FilmVizDirectMetalProcessor&) = delete;

    bool configure(
        const FilmVizOfxRenderSettings& settings,
        const std::string& resources_directory,
        void* command_queue,
        std::string& error);

    bool render(
        const FilmVizOfxRenderSettings& settings,
        void* command_queue,
        const FilmVizOfxMetalFrame& source,
        const FilmVizOfxMetalFrame& destination,
        int render_x1,
        int render_y1,
        int render_x2,
        int render_y2,
        double time,
        std::string& error);

    bool render_cpu_bridge(
        FilmVizOfxProcessor& cpu_processor,
        void* command_queue,
        const FilmVizOfxMetalFrame& source,
        const FilmVizOfxMetalFrame& destination,
        int render_x1,
        int render_y1,
        int render_x2,
        int render_y2,
        double time,
        const FilmVizOfxProcessor::Abort& abort,
        std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
