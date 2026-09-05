// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "lut3d.h"
#include "threading.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <thread>

bool
Lut3D::generate(
    int size,
    const Evaluator& evaluator,
    const Progress& progress)
{
    if (size < 2 || !evaluator) {
        return false;
    }

    size_ = size;
    values_.assign(
        static_cast<std::size_t>(
            size * size * size),
        {{0.0f, 0.0f, 0.0f}});

    std::atomic<int> next_blue(0);
    std::atomic<int> completed(0);
    std::atomic<bool> failed(false);
    std::mutex progress_mutex;
    const int worker_count =
        FilmVizThreading::effective_thread_count(size);
    std::vector<std::thread> workers;
    workers.reserve(
        static_cast<std::size_t>(worker_count));

    for (int worker = 0;
         worker < worker_count;
         ++worker) {

        workers.emplace_back(
            [&]() {
                while (!failed.load(
                           std::memory_order_relaxed)) {

                    const int blue =
                        next_blue.fetch_add(
                            1,
                            std::memory_order_relaxed);

                    if (blue >= size) {
                        break;
                    }

                    for (int green = 0;
                         green < size
                         && !failed.load(
                             std::memory_order_relaxed);
                         ++green) {

                        for (int red = 0;
                             red < size;
                             ++red) {

                            const RGB input = {{
                                static_cast<float>(red)
                                    / static_cast<float>(size - 1),
                                static_cast<float>(green)
                                    / static_cast<float>(size - 1),
                                static_cast<float>(blue)
                                    / static_cast<float>(size - 1)
                            }};

                            RGB output;

                            if (!evaluator(input, output)) {
                                failed.store(
                                    true,
                                    std::memory_order_relaxed);
                                break;
                            }

                            at_mutable(
                                red,
                                green,
                                blue) = output;
                        }
                    }

                    if (failed.load(
                            std::memory_order_relaxed)) {
                        break;
                    }

                    const int finished =
                        completed.fetch_add(
                            1,
                            std::memory_order_relaxed)
                        + 1;

                    if (progress) {
                        const std::lock_guard<std::mutex> lock(
                            progress_mutex);
                        progress(finished, size);
                    }
                }
            });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    if (failed.load(
            std::memory_order_relaxed)) {
        values_.clear();
        size_ = 0;
        return false;
    }

    return valid();
}

bool
Lut3D::assign(
    int size,
    const std::vector<RGB>& values)
{
    if (size < 2
        || values.size()
            != static_cast<std::size_t>(
                size * size * size)) {

        return false;
    }

    size_ = size;
    values_ = values;
    return true;
}

bool
Lut3D::valid() const
{
    return
        size_ >= 2
        && values_.size()
            == static_cast<std::size_t>(
                size_ * size_ * size_);
}

int
Lut3D::size() const
{
    return size_;
}

const Lut3D::RGB&
Lut3D::at(
    int red,
    int green,
    int blue) const
{
    return
        values_[
            static_cast<std::size_t>(
                (blue * size_ + green)
                * size_
                + red)];
}

Lut3D::RGB&
Lut3D::at_mutable(
    int red,
    int green,
    int blue)
{
    return
        values_[
            static_cast<std::size_t>(
                (blue * size_ + green)
                * size_
                + red)];
}

Lut3D::RGB
Lut3D::sample_trilinear(
    const RGB& input) const
{
    if (!valid()) {
        return {{0.0f, 0.0f, 0.0f}};
    }

    const float scale =
        static_cast<float>(
            size_ - 1);

    const auto coordinate =
        [scale](float value) {

            return
                std::clamp(
                    value,
                    0.0f,
                    1.0f)
                * scale;
        };

    const float rf = coordinate(input[0]);
    const float gf = coordinate(input[1]);
    const float bf = coordinate(input[2]);

    const int r0 =
        std::min(
            size_ - 2,
            static_cast<int>(
                std::floor(rf)));

    const int g0 =
        std::min(
            size_ - 2,
            static_cast<int>(
                std::floor(gf)));

    const int b0 =
        std::min(
            size_ - 2,
            static_cast<int>(
                std::floor(bf)));

    const int r1 = r0 + 1;
    const int g1 = g0 + 1;
    const int b1 = b0 + 1;

    const float tr = rf - static_cast<float>(r0);
    const float tg = gf - static_cast<float>(g0);
    const float tb = bf - static_cast<float>(b0);

    const auto lerp =
        [](const RGB& a,
           const RGB& b,
           float t) {

            return RGB{{
                a[0] + (b[0] - a[0]) * t,
                a[1] + (b[1] - a[1]) * t,
                a[2] + (b[2] - a[2]) * t
            }};
        };

    const RGB c00 =
        lerp(
            at(r0, g0, b0),
            at(r1, g0, b0),
            tr);

    const RGB c10 =
        lerp(
            at(r0, g1, b0),
            at(r1, g1, b0),
            tr);

    const RGB c01 =
        lerp(
            at(r0, g0, b1),
            at(r1, g0, b1),
            tr);

    const RGB c11 =
        lerp(
            at(r0, g1, b1),
            at(r1, g1, b1),
            tr);

    const RGB c0 = lerp(c00, c10, tg);
    const RGB c1 = lerp(c01, c11, tg);

    return lerp(c0, c1, tb);
}

Lut3D::RGB
Lut3D::sample_tetrahedral(
    const RGB& input) const
{
    if (!valid()) {
        return {{0.0f, 0.0f, 0.0f}};
    }

    const float scale =
        static_cast<float>(
            size_ - 1);

    const auto coordinate =
        [scale](float value) {

            return
                std::clamp(
                    value,
                    0.0f,
                    1.0f)
                * scale;
        };

    const float rf = coordinate(input[0]);
    const float gf = coordinate(input[1]);
    const float bf = coordinate(input[2]);

    const int r0 =
        std::min(
            size_ - 2,
            static_cast<int>(
                std::floor(rf)));

    const int g0 =
        std::min(
            size_ - 2,
            static_cast<int>(
                std::floor(gf)));

    const int b0 =
        std::min(
            size_ - 2,
            static_cast<int>(
                std::floor(bf)));

    const int r1 = r0 + 1;
    const int g1 = g0 + 1;
    const int b1 = b0 + 1;

    const float tr = rf - static_cast<float>(r0);
    const float tg = gf - static_cast<float>(g0);
    const float tb = bf - static_cast<float>(b0);

    const RGB& c000 = at(r0, g0, b0);
    const RGB& c100 = at(r1, g0, b0);
    const RGB& c010 = at(r0, g1, b0);
    const RGB& c110 = at(r1, g1, b0);
    const RGB& c001 = at(r0, g0, b1);
    const RGB& c101 = at(r1, g0, b1);
    const RGB& c011 = at(r0, g1, b1);
    const RGB& c111 = at(r1, g1, b1);

    const auto combine =
        [](const RGB& base,
           const RGB& v1,
           float w1,
           const RGB& v2,
           float w2,
           const RGB& v3,
           float w3) {

            return RGB{{
                base[0] + v1[0] * w1 + v2[0] * w2 + v3[0] * w3,
                base[1] + v1[1] * w1 + v2[1] * w2 + v3[1] * w3,
                base[2] + v1[2] * w1 + v2[2] * w2 + v3[2] * w3
            }};
        };

    const auto difference =
        [](const RGB& a,
           const RGB& b) {

            return RGB{{
                a[0] - b[0],
                a[1] - b[1],
                a[2] - b[2]
            }};
        };

    if (tr >= tg) {
        if (tg >= tb) {
            return combine(
                c000,
                difference(c100, c000), tr,
                difference(c110, c100), tg,
                difference(c111, c110), tb);
        }

        if (tr >= tb) {
            return combine(
                c000,
                difference(c100, c000), tr,
                difference(c101, c100), tb,
                difference(c111, c101), tg);
        }

        return combine(
            c000,
            difference(c001, c000), tb,
            difference(c101, c001), tr,
            difference(c111, c101), tg);
    }

    if (tb >= tg) {
        return combine(
            c000,
            difference(c001, c000), tb,
            difference(c011, c001), tg,
            difference(c111, c011), tr);
    }

    if (tb >= tr) {
        return combine(
            c000,
            difference(c010, c000), tg,
            difference(c011, c010), tb,
            difference(c111, c011), tr);
    }

    return combine(
        c000,
        difference(c010, c000), tg,
        difference(c110, c010), tr,
        difference(c111, c110), tb);
}

bool
Lut3D::write_cube(
    const std::string& filename,
    const std::string& title,
    const std::vector<std::string>& comments) const
{
    if (!valid()) {
        return false;
    }

    std::ofstream file(
        filename.c_str());

    if (!file) {
        return false;
    }

    for (const std::string& comment : comments) {
        file
            << "# "
            << comment
            << "\n";
    }

    file
        << "TITLE \""
        << title
        << "\"\n"
        << "LUT_3D_SIZE "
        << size_
        << "\n"
        << "DOMAIN_MIN 0.0 0.0 0.0\n"
        << "DOMAIN_MAX 1.0 1.0 1.0\n"
        << std::setprecision(10);

    for (int blue = 0;
         blue < size_;
         ++blue) {

        for (int green = 0;
             green < size_;
             ++green) {

            for (int red = 0;
                 red < size_;
                 ++red) {

                const RGB& value =
                    at(
                        red,
                        green,
                        blue);

                file
                    << value[0]
                    << " "
                    << value[1]
                    << " "
                    << value[2]
                    << "\n";
            }
        }
    }

    return true;
}

Lut3D::Validation
Lut3D::validate(
    const Evaluator& evaluator,
    int grid_size) const
{
    Validation validation;

    if (!valid()
        || !evaluator
        || grid_size < 1) {

        return validation;
    }

    double sum = 0.0;
    int component_count = 0;

    for (int blue = 0;
         blue < grid_size;
         ++blue) {

        for (int green = 0;
             green < grid_size;
             ++green) {

            for (int red = 0;
                 red < grid_size;
                 ++red) {

                const RGB input = {{
                    (static_cast<float>(red) + 0.37f)
                        / static_cast<float>(grid_size),
                    (static_cast<float>(green) + 0.53f)
                        / static_cast<float>(grid_size),
                    (static_cast<float>(blue) + 0.71f)
                        / static_cast<float>(grid_size)
                }};

                RGB direct;

                if (!evaluator(input, direct)) {
                    return validation;
                }

                const RGB interpolated =
                    sample_tetrahedral(
                        input);

                for (int channel = 0;
                     channel < 3;
                     ++channel) {

                    const double error =
                        std::abs(
                            static_cast<double>(
                                interpolated[channel])
                            - static_cast<double>(
                                direct[channel]));

                    sum += error;

                    validation.max_abs_error =
                        std::max(
                            validation.max_abs_error,
                            error);

                    ++component_count;
                }

                ++validation.samples;
            }
        }
    }

    if (component_count > 0) {
        validation.mean_abs_error =
            sum
            / static_cast<double>(
                component_count);

        validation.valid = true;
    }

    return validation;
}
