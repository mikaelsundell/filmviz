// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"
#include "filmstock.h"
#include "filmprocessor.h"
#include "filmdyemodel.h"
#include "printfilmstock.h"
#include "printdyemodel.h"
#include "printfilmprocessor.h"
#include "printviewer.h"

#include <array>
#include <string>
#include <vector>

class DiagramWriter
{
public:
    struct Series
    {
        std::string name;
        const SampledCurve* curve = nullptr;
        std::array<float, 3> color = {{1.0f, 1.0f, 1.0f}};

        // Draw the interpolated line.
        bool show_line = true;

        // Draw the original source samples as small markers over the line.
        // The line itself is the piecewise-linear interpolation used by
        // SampledCurve::sample().
        bool show_markers = false;
        int marker_radius = 2;
    };

    struct PlotOptions
    {
        int width = 1200;
        int height = 800;

        // Main plot title.
        std::string title;

        // Optional subtitle / metadata line below the title.
        std::string subtitle;

        // Bottom x-axis and left y-axis labels.
        std::string x_label;
        std::string y_label;

        // Optional secondary x-axis at the top of the graph.
        std::string top_x_label;
        bool show_top_x_axis = false;

        // Axis label transforms.
        //
        // The plotted x coordinate always remains the curve's native x value.
        // These transforms affect labels only:
        //
        //   displayed = native_x * scale + offset
        //
        // This allows, for example:
        //
        //   native x  = log exposure
        //   top axis  = log exposure
        //   bottom    = camera stops
        //
        float bottom_x_scale = 1.0f;
        float bottom_x_offset = 0.0f;

        float top_x_scale = 1.0f;
        float top_x_offset = 0.0f;

        // Number of labelled divisions.
        int x_divisions = 10;
        int y_divisions = 8;
        int top_x_divisions = 2;

        // Range handling.
        bool include_zero_x = false;
        bool include_zero_y = false;

        // Optional explicit native-axis ranges. These are useful for source
        // validation plots where Kodak's published graph bounds are part of
        // the data provenance.
        bool has_x_range = false;
        float x_min = 0.0f;
        float x_max = 1.0f;

        bool has_y_range = false;
        float y_min = 0.0f;
        float y_max = 1.0f;

        // Logarithmic coordinate mapping. Source values remain in native
        // physical units; only their placement on the plot is logarithmic.
        bool log_x = false;
        bool log_y = false;

        // Normal plots get a little breathing room. Set x_padding_fraction to
        // zero for plots where axis endpoints have physical meaning.
        float x_padding_fraction = 0.05f;
        float y_padding_fraction = 0.05f;
    };

    static bool write_plot(
        const std::string& filename,
        const std::vector<Series>& series,
        const PlotOptions& options);

    static bool write_film_diagnostics(
        const std::string& output_image_filename,
        const FilmStock& stock,
        const SampledCurve& illuminant,
        const FilmProcessor::Settings& processor_settings,
        const FilmExposureBalance& balance);

    static bool write_dye_model_diagnostics(
        const std::string& output_image_filename,
        const FilmStock& stock,
        const FilmDyeModel& dye_model);


    // Write validation plots directly from the five digitized Kodak 2383
    // source datasets. No print simulation or inferred dye model is involved.
    static bool write_print_stock_source_diagnostics(
        const std::string& output_image_filename,
        const PrintFilmStock& stock);

    // Diagnostics for the first active Kodak 2383 print pipeline:
    // negative transmission -> printer light -> print exposure/development
    // -> measured 2383 C/M/Y spectral dye model.
    static bool write_print_pipeline_diagnostics(
        const std::string& output_image_filename,
        const FilmStock& negative_stock,
        const FilmDyeModel& negative_dye_model,
        const PrintFilmStock& print_stock,
        const PrintFilmProcessor& print_processor,
        const PrintDyeModel& print_dye_model);

    // Prototype 14 output-side viewing diagnostics: viewing SPD plus the
    // viewed neutral ladder in xy and linear ACES2065-1/AP0.
    static bool write_print_viewer_diagnostics(
        const std::string& output_image_filename,
        const FilmStock& negative_stock,
        const FilmDyeModel& negative_dye_model,
        const PrintFilmStock& print_stock,
        const PrintFilmProcessor& print_processor,
        const PrintDyeModel& print_dye_model,
        const PrintViewer& print_viewer);
};
