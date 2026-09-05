// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "halationmodel.h"
#include "test_common.h"
#include "threading.h"

#include <vector>

namespace {

float
red_delta(
    const std::vector<FilmExposure>& before,
    const std::vector<FilmExposure>& after,
    int index)
{
    return
        after[static_cast<std::size_t>(index)].red
        - before[static_cast<std::size_t>(index)].red;
}

} // namespace

int
main()
{
    bool passed = true;

    constexpr int width = 21;
    constexpr int height = 5;
    constexpr int y = 2;
    constexpr int bright_x = 4;
    constexpr int medium_x = 16;
    constexpr int bright = y * width + bright_x;
    constexpr int medium = y * width + medium_x;
    constexpr int bright_neighbour = y * width + bright_x + 1;
    constexpr int medium_neighbour = y * width + medium_x + 1;

    std::vector<FilmExposure> source_exposure(
        width * height,
        FilmExposure{1.0f, 1.0f, 1.0f});

    std::vector<float> ap0(
        width * height * 3,
        0.0f);

    for (int channel = 0; channel < 3; ++channel) {
        ap0[bright * 3 + channel] = 1.2f;
        ap0[medium * 3 + channel] = 0.45f;
    }

    std::vector<FilmExposure> disabled =
        source_exposure;

    HalationModel::Settings disabled_settings;
    disabled_settings.strength = 0.0f;
    disabled_settings.radius_pixels = 2.0f;
    disabled_settings.threshold = 0.6f;

    passed &= test::check(
        HalationModel::apply(
            disabled,
            ap0,
            width,
            height,
            disabled_settings),
        "disabled halation accepts valid negative exposure and AP0 input");

    bool disabled_unchanged =
        disabled.size() == source_exposure.size();

    for (std::size_t i = 0;
         disabled_unchanged && i < disabled.size();
         ++i) {

        disabled_unchanged =
            disabled[i].red == source_exposure[i].red
            && disabled[i].green == source_exposure[i].green
            && disabled[i].blue == source_exposure[i].blue;
    }

    passed &= test::check(
        disabled_unchanged,
        "zero halation strength preserves negative exposure exactly");

    std::vector<FilmExposure> processed =
        source_exposure;

    HalationModel::Settings settings;
    settings.strength = 1.0f;
    settings.radius_pixels = 2.0f;
    settings.threshold = 0.6f;

    passed &= test::check(
        HalationModel::apply(
            processed,
            ap0,
            width,
            height,
            settings),
        "halation model processes negative exposure before development");

    const float bright_red =
        red_delta(
            source_exposure,
            processed,
            bright_neighbour);

    const float medium_red =
        red_delta(
            source_exposure,
            processed,
            medium_neighbour);

    passed &= test::check(
        bright_red > 0.0f,
        "bright highlight scatters exposure into a neighbouring negative pixel");

    passed &= test::check(
        medium_red > 0.0f,
        "sub-threshold bright gray contributes gradually to halation");

    passed &= test::check(
        bright_red > medium_red,
        "halation contribution rises smoothly with scene brightness");

    const FilmExposure& neighbour =
        processed[static_cast<std::size_t>(bright_neighbour)];

    const FilmExposure& original_neighbour =
        source_exposure[static_cast<std::size_t>(bright_neighbour)];

    const float red =
        neighbour.red - original_neighbour.red;

    const float green =
        neighbour.green - original_neighbour.green;

    const float blue =
        neighbour.blue - original_neighbour.blue;

    passed &= test::check(
        red > green
        && green > blue
        && blue > 0.0f,
        "negative-stage halation preferentially exposes the red-sensitive record");

    passed &= test::check(
        processed[bright].red
            - source_exposure[bright].red
            < bright_red,
        "source suppression keeps halation concentrated outside the highlight");

    FilmVizThreading::set_thread_count(1);
    std::vector<FilmExposure> single_thread =
        source_exposure;

    passed &= test::check(
        HalationModel::apply(
            single_thread,
            ap0,
            width,
            height,
            settings),
        "single-thread halation completes");

    FilmVizThreading::set_thread_count(4);
    std::vector<FilmExposure> multi_thread =
        source_exposure;

    passed &= test::check(
        HalationModel::apply(
            multi_thread,
            ap0,
            width,
            height,
            settings),
        "multi-thread halation completes");

    bool deterministic =
        single_thread.size() == multi_thread.size();

    for (std::size_t i = 0;
         deterministic && i < single_thread.size();
         ++i) {

        deterministic =
            single_thread[i].red == multi_thread[i].red
            && single_thread[i].green == multi_thread[i].green
            && single_thread[i].blue == multi_thread[i].blue;
    }

    passed &= test::check(
        deterministic,
        "threaded halation matches single-thread output exactly");

    FilmVizThreading::set_thread_count(0);

    HalationModel::Settings invalid = settings;
    invalid.strength = 1.5f;

    passed &= test::check(
        !HalationModel::valid_settings(invalid),
        "halation strength outside 0..1 is rejected");

    return
        test::finish(
            passed,
            "halation model");
}
