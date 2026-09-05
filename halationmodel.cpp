// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "halationmodel.h"
#include "threading.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {

float
channel_value(
    const FilmExposure& exposure,
    int channel)
{
    switch (channel) {
    case 0:
        return exposure.red;
    case 1:
        return exposure.green;
    default:
        return exposure.blue;
    }
}

float&
channel_value(
    FilmExposure& exposure,
    int channel)
{
    switch (channel) {
    case 0:
        return exposure.red;
    case 1:
        return exposure.green;
    default:
        return exposure.blue;
    }
}

float
scatter_value(
    const FilmExposure& exposure,
    int channel)
{
    return channel_value(
        exposure,
        channel);
}

bool
parallel_for_items(
    int item_count,
    const std::function<void(int)>& operation,
    const HalationModel::Cancel& cancel,
    const std::function<void()>& work_completed)
{
    if (item_count <= 0) {
        return true;
    }

    const int worker_count =
        FilmVizThreading::effective_thread_count(
            item_count);

    std::atomic<int> next_item(0);
    std::atomic<bool> cancelled(false);

    auto worker =
        [&]() {
            while (!cancelled.load(
                std::memory_order_relaxed)) {

                const int item =
                    next_item.fetch_add(
                        1,
                        std::memory_order_relaxed);

                if (item >= item_count) {
                    break;
                }

                if ((item & 31) == 0
                    && cancel
                    && cancel()) {
                    cancelled.store(
                        true,
                        std::memory_order_relaxed);
                    break;
                }

                operation(item);

                if (work_completed) {
                    work_completed();
                }
            }
        };

    if (worker_count <= 1) {
        worker();
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(
            static_cast<std::size_t>(worker_count));

        for (int worker_index = 0;
             worker_index < worker_count;
             ++worker_index) {

            workers.emplace_back(worker);
        }

        for (std::thread& thread : workers) {
            thread.join();
        }
    }

    return
        !cancelled.load(
            std::memory_order_relaxed);
}

void
box_blur_horizontal(
    const std::vector<float>& source,
    std::vector<float>& destination,
    int width,
    int height,
    int radius,
    const HalationModel::Cancel& cancel,
    const std::function<void()>& work_completed,
    std::atomic<bool>& success)
{
    const int window =
        2 * radius + 1;

    if (!parallel_for_items(
            height,
            [&](int y) {
                const std::size_t row =
                    static_cast<std::size_t>(y)
                    * static_cast<std::size_t>(width);

                double sum = 0.0;

                for (int k = -radius;
                     k <= radius;
                     ++k) {

                    const int sx =
                        std::clamp(
                            k,
                            0,
                            width - 1);

                    sum +=
                        static_cast<double>(
                            source[
                                row
                                + static_cast<std::size_t>(sx)]);
                }

                for (int x = 0;
                     x < width;
                     ++x) {

                    destination[
                        row
                        + static_cast<std::size_t>(x)] =
                        static_cast<float>(
                            sum
                            / static_cast<double>(window));

                    const int remove_x =
                        std::clamp(
                            x - radius,
                            0,
                            width - 1);

                    const int add_x =
                        std::clamp(
                            x + radius + 1,
                            0,
                            width - 1);

                    sum -=
                        static_cast<double>(
                            source[
                                row
                                + static_cast<std::size_t>(remove_x)]);

                    sum +=
                        static_cast<double>(
                            source[
                                row
                                + static_cast<std::size_t>(add_x)]);
                }
            },
            cancel,
            work_completed)) {

        success.store(
            false,
            std::memory_order_relaxed);
    }
}

void
box_blur_vertical(
    const std::vector<float>& source,
    std::vector<float>& destination,
    int width,
    int height,
    int radius,
    const HalationModel::Cancel& cancel,
    const std::function<void()>& work_completed,
    std::atomic<bool>& success)
{
    const int window =
        2 * radius + 1;

    if (!parallel_for_items(
            width,
            [&](int x) {
                double sum = 0.0;

                for (int k = -radius;
                     k <= radius;
                     ++k) {

                    const int sy =
                        std::clamp(
                            k,
                            0,
                            height - 1);

                    sum +=
                        static_cast<double>(
                            source[
                                static_cast<std::size_t>(sy)
                                    * static_cast<std::size_t>(width)
                                + static_cast<std::size_t>(x)]);
                }

                for (int y = 0;
                     y < height;
                     ++y) {

                    destination[
                        static_cast<std::size_t>(y)
                            * static_cast<std::size_t>(width)
                        + static_cast<std::size_t>(x)] =
                        static_cast<float>(
                            sum
                            / static_cast<double>(window));

                    const int remove_y =
                        std::clamp(
                            y - radius,
                            0,
                            height - 1);

                    const int add_y =
                        std::clamp(
                            y + radius + 1,
                            0,
                            height - 1);

                    sum -=
                        static_cast<double>(
                            source[
                                static_cast<std::size_t>(remove_y)
                                    * static_cast<std::size_t>(width)
                                + static_cast<std::size_t>(x)]);

                    sum +=
                        static_cast<double>(
                            source[
                                static_cast<std::size_t>(add_y)
                                    * static_cast<std::size_t>(width)
                                + static_cast<std::size_t>(x)]);
                }
            },
            cancel,
            work_completed)) {

        success.store(
            false,
            std::memory_order_relaxed);
    }
}

} // namespace

bool
HalationModel::valid_settings(
    const Settings& settings)
{
    return
        std::isfinite(settings.strength)
        && settings.strength >= 0.0f
        && settings.strength <= 1.0f
        && std::isfinite(settings.radius_pixels)
        && settings.radius_pixels >= 0.0f
        && std::isfinite(settings.threshold)
        && settings.threshold >= 0.0f
        && std::isfinite(settings.record_scatter.red)
        && std::isfinite(settings.record_scatter.green)
        && std::isfinite(settings.record_scatter.blue)
        && settings.record_scatter.red >= 0.0f
        && settings.record_scatter.green >= 0.0f
        && settings.record_scatter.blue >= 0.0f;
}

bool
HalationModel::apply(
    std::vector<FilmExposure>& negative_exposure,
    const std::vector<float>& ap0_pixels,
    int width,
    int height,
    const Settings& settings,
    const Cancel& cancel,
    const Progress& progress)
{
    if (!valid_settings(settings)
        || width <= 0
        || height <= 0) {
        return false;
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);

    if (negative_exposure.size() != pixel_count
        || ap0_pixels.size() != pixel_count * 3u) {
        return false;
    }

    if (settings.strength <= 0.0f
        || settings.radius_pixels <= 0.0f) {
        return true;
    }

    // Work is counted in completed rows/columns rather than pixels so progress
    // remains inexpensive even for large images. Each Gaussian approximation
    // uses three separable box passes.
    const int total_work =
        25 * height
        + 18 * width;

    std::atomic<int> completed_work(0);
    std::mutex progress_mutex;

    const auto work_completed =
        [&]() {
            const int completed =
                completed_work.fetch_add(
                    1,
                    std::memory_order_relaxed)
                + 1;

            if (progress
                && (completed == total_work
                    || (completed & 255) == 0)) {

                const std::lock_guard<std::mutex> lock(
                    progress_mutex);

                progress(
                    completed,
                    total_work);
            }
        };

    if (progress) {
        progress(0, total_work);
    }

    std::vector<float> highlight_weight(
        pixel_count,
        0.0f);

    if (!parallel_for_items(
            height,
            [&](int y) {
                const std::size_t row =
                    static_cast<std::size_t>(y)
                    * static_cast<std::size_t>(width);

                for (int x = 0;
                     x < width;
                     ++x) {

                    const std::size_t pixel =
                        row
                        + static_cast<std::size_t>(x);

                    const float luminance =
                        ap0_luminance(
                            &ap0_pixels[pixel * 3u]);

                    highlight_weight[pixel] =
                        smooth_highlight_weight(
                            luminance,
                            settings.threshold);
                }
            },
            cancel,
            work_completed)) {

        return false;
    }

    constexpr float near_mix = 0.72f;
    constexpr float far_mix = 1.0f - near_mix;
    constexpr float far_radius_scale = 2.2f;
    constexpr float source_suppression = 0.95f;

    for (int channel = 0;
         channel < 3;
         ++channel) {

        std::vector<float> source(
            pixel_count,
            0.0f);

        if (!parallel_for_items(
                height,
                [&](int y) {
                    const std::size_t row =
                        static_cast<std::size_t>(y)
                        * static_cast<std::size_t>(width);

                    for (int x = 0;
                         x < width;
                         ++x) {

                        const std::size_t pixel =
                            row
                            + static_cast<std::size_t>(x);

                        source[pixel] =
                            std::max(
                                0.0f,
                                channel_value(
                                    negative_exposure[pixel],
                                    channel))
                            * highlight_weight[pixel];
                    }
                },
                cancel,
                work_completed)) {

            return false;
        }

        const std::vector<float> near_blur =
            gaussian_blur_scalar(
                source,
                width,
                height,
                settings.radius_pixels,
                cancel,
                work_completed);

        const std::vector<float> far_blur =
            gaussian_blur_scalar(
                source,
                width,
                height,
                settings.radius_pixels
                    * far_radius_scale,
                cancel,
                work_completed);

        if (near_blur.size() != pixel_count
            || far_blur.size() != pixel_count) {
            return false;
        }

        const float channel_scatter =
            scatter_value(
                settings.record_scatter,
                channel);

        if (!parallel_for_items(
                height,
                [&](int y) {
                    const std::size_t row =
                        static_cast<std::size_t>(y)
                        * static_cast<std::size_t>(width);

                    for (int x = 0;
                         x < width;
                         ++x) {

                        const std::size_t pixel =
                            row
                            + static_cast<std::size_t>(x);

                        const float blurred =
                            near_mix * near_blur[pixel]
                            + far_mix * far_blur[pixel];

                        // Remove almost all of the original source contribution.
                        // This keeps the effect primarily outside bright objects
                        // instead of tinting the source itself like a conventional
                        // glow filter.
                        const float spread =
                            std::max(
                                0.0f,
                                blurred
                                - source_suppression
                                    * source[pixel]);

                        channel_value(
                            negative_exposure[pixel],
                            channel) +=
                            settings.strength
                            * channel_scatter
                            * spread;
                    }
                },
                cancel,
                work_completed)) {

            return false;
        }
    }

    if (progress) {
        progress(total_work, total_work);
    }

    return true;
}

float
HalationModel::ap0_luminance(
    const float* ap0)
{
    return
        0.34396645f * ap0[0]
        + 0.72816610f * ap0[1]
        - 0.07213255f * ap0[2];
}

float
HalationModel::smooth_highlight_weight(
    float luminance,
    float threshold)
{
    const float safe_luminance =
        std::max(
            0.0f,
            luminance);

    if (threshold <= 1e-8f) {
        return safe_luminance;
    }

    // Start the transition below the nominal threshold. This means adjacent
    // bright gray patches contribute progressively less instead of jumping
    // abruptly between "halation" and "no halation".
    const float onset =
        0.5f * threshold;

    if (safe_luminance <= onset) {
        return 0.0f;
    }

    const float t =
        std::clamp(
            (safe_luminance - onset)
            / std::max(
                threshold - onset,
                1e-8f),
            0.0f,
            1.0f);

    const float smooth =
        t * t * (3.0f - 2.0f * t);

    return
        safe_luminance
        * smooth;
}

std::vector<float>
HalationModel::gaussian_blur_scalar(
    const std::vector<float>& source,
    int width,
    int height,
    float radius_pixels,
    const Cancel& cancel,
    const std::function<void()>& work_completed)
{
    if (source.empty()
        || width <= 0
        || height <= 0
        || radius_pixels <= 0.0f) {
        return source;
    }

    // The previous implementation evaluated a full Gaussian kernel for every
    // pixel. At large radii that becomes O(radius * pixels) per axis. Three
    // box-filter passes closely approximate the same Gaussian while making the
    // cost O(pixels), independent of radius.
    const float sigma =
        std::max(
            0.5f,
            radius_pixels / 3.0f);

    const int box_radius =
        std::max(
            1,
            static_cast<int>(
                std::round(
                    0.5f
                    * (-1.0f
                        + std::sqrt(
                            1.0f
                            + 4.0f
                                * sigma
                                * sigma)))));

    const std::size_t count =
        static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);

    std::vector<float> current = source;
    std::vector<float> temporary(count, 0.0f);
    std::vector<float> result(count, 0.0f);

    for (int pass = 0;
         pass < 3;
         ++pass) {

        std::atomic<bool> success(true);

        box_blur_horizontal(
            current,
            temporary,
            width,
            height,
            box_radius,
            cancel,
            work_completed,
            success);

        if (!success.load(
                std::memory_order_relaxed)) {
            return std::vector<float>();
        }

        box_blur_vertical(
            temporary,
            result,
            width,
            height,
            box_radius,
            cancel,
            work_completed,
            success);

        if (!success.load(
                std::memory_order_relaxed)) {
            return std::vector<float>();
        }

        if (pass < 2) {
            current.swap(result);
        }
    }

    return result;
}
