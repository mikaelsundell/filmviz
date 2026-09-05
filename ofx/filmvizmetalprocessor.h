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

class FilmVizMetalProcessor
{
public:
    FilmVizMetalProcessor();
    ~FilmVizMetalProcessor();

    FilmVizMetalProcessor(const FilmVizMetalProcessor&) = delete;
    FilmVizMetalProcessor& operator=(const FilmVizMetalProcessor&) = delete;

    bool configure(
        FilmVizOfxProcessor& cpu_processor,
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

    bool copy(
        void* command_queue,
        const FilmVizOfxMetalFrame& source,
        const FilmVizOfxMetalFrame& destination,
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
