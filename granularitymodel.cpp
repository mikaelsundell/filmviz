// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "granularitymodel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace
{

std::vector<std::string>
split_csv_line(
    const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }

    return fields;
}

bool
parse_float(
    const std::vector<std::string>& fields,
    std::size_t index,
    float& value)
{
    if (index >= fields.size()
        || fields[index].empty()) {
        return false;
    }

    try {
        std::size_t parsed = 0;
        value = std::stof(fields[index], &parsed);

        while (parsed < fields[index].size()
               && std::isspace(
                   static_cast<unsigned char>(
                       fields[index][parsed]))) {
            ++parsed;
        }

        return parsed == fields[index].size();
    }
    catch (...) {
        return false;
    }
}

void
append(
    SampledCurve& curve,
    float x,
    float y)
{
    curve.x.push_back(x);
    curve.y.push_back(y);
}

std::uint32_t
mix_bits(
    std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float
uniform_open(
    std::uint32_t value)
{
    return
        (static_cast<float>(
            mix_bits(value) & 0x00ffffffu)
         + 0.5f)
        / 16777216.0f;
}

} // namespace

bool
GranularityModel::load(
    const std::string& negative_filename,
    const std::string& print_filename)
{
    negative_ = Curves();
    print_ = Curves();
    valid_ =
        load_curves(
            negative_filename,
            true,
            negative_)
        && load_curves(
            print_filename,
            false,
            print_);

    return valid_;
}

bool
GranularityModel::valid() const
{
    return valid_;
}

FilmDensity
GranularityModel::negative_sigma(
    const FilmDensity& status_m_density) const
{
    return sample(
        negative_,
        status_m_density);
}

FilmDensity
GranularityModel::print_sigma(
    const FilmDensity& status_a_density) const
{
    return sample(
        print_,
        status_a_density);
}

float
GranularityModel::normal_sample(
    std::uint32_t seed,
    int x,
    int y,
    int stage,
    int channel)
{
    std::uint32_t key = seed;
    key ^= mix_bits(
        static_cast<std::uint32_t>(x)
        + 0x9e3779b9u);
    key ^= mix_bits(
        static_cast<std::uint32_t>(y)
        + 0x85ebca6bu);
    key ^= mix_bits(
        static_cast<std::uint32_t>(stage)
        * 0xc2b2ae35u
        + static_cast<std::uint32_t>(channel));

    const float u1 =
        std::max(
            uniform_open(key),
            1e-7f);

    const float u2 =
        uniform_open(
            key ^ 0x68bc21ebu);

    constexpr float two_pi =
        6.2831853071795864769f;

    return
        std::sqrt(
            -2.0f * std::log(u1))
        * std::cos(
            two_pi * u2);
}

bool
GranularityModel::load_curves(
    const std::string& filename,
    bool negative_order,
    Curves& curves)
{
    std::ifstream file(
        filename.c_str());

    std::string line;

    if (!file
        || !std::getline(file, line)) {
        return false;
    }

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        float x = 0.0f;

        if (!parse_float(fields, 0, x)) {
            continue;
        }

        const std::size_t red_density =
            negative_order ? 3u : 1u;
        const std::size_t green_density = 2u;
        const std::size_t blue_density =
            negative_order ? 1u : 3u;
        const std::size_t red_sigma =
            negative_order ? 6u : 4u;
        const std::size_t green_sigma = 5u;
        const std::size_t blue_sigma =
            negative_order ? 4u : 6u;

        float value = 0.0f;

        if (parse_float(fields, red_density, value)) {
            append(curves.red_density, x, value);
        }
        if (parse_float(fields, green_density, value)) {
            append(curves.green_density, x, value);
        }
        if (parse_float(fields, blue_density, value)) {
            append(curves.blue_density, x, value);
        }
        if (parse_float(fields, red_sigma, value)) {
            append(curves.red_sigma, x, value);
        }
        if (parse_float(fields, green_sigma, value)) {
            append(curves.green_sigma, x, value);
        }
        if (parse_float(fields, blue_sigma, value)) {
            append(curves.blue_sigma, x, value);
        }
    }

    return
        curves.red_density.valid()
        && curves.green_density.valid()
        && curves.blue_density.valid()
        && curves.red_sigma.valid()
        && curves.green_sigma.valid()
        && curves.blue_sigma.valid();
}

float
GranularityModel::sigma_for_density(
    const SampledCurve& density,
    const SampledCurve& sigma,
    float target_density)
{
    if (!density.valid()
        || !sigma.valid()) {
        return 0.0f;
    }

    float best_x = density.x.front();
    float best_error =
        std::numeric_limits<float>::infinity();

    for (std::size_t i = 0;
         i + 1 < density.y.size();
         ++i) {

        const float d0 = density.y[i];
        const float d1 = density.y[i + 1];
        const float low = std::min(d0, d1);
        const float high = std::max(d0, d1);
        const float nearest =
            std::clamp(
                target_density,
                low,
                high);
        const float error =
            std::abs(target_density - nearest);

        if (error < best_error) {
            best_error = error;

            const float denominator = d1 - d0;
            const float t =
                std::abs(denominator) > 1e-12f
                    ? std::clamp(
                        (nearest - d0) / denominator,
                        0.0f,
                        1.0f)
                    : 0.0f;

            best_x =
                density.x[i]
                + t
                    * (density.x[i + 1]
                       - density.x[i]);
        }
    }

    return
        std::max(
            0.0f,
            sigma.sample(
                std::clamp(
                    best_x,
                    sigma.x.front(),
                    sigma.x.back()),
                0.0f));
}

FilmDensity
GranularityModel::sample(
    const Curves& curves,
    const FilmDensity& density)
{
    FilmDensity result;

    result.red =
        sigma_for_density(
            curves.red_density,
            curves.red_sigma,
            density.red);

    result.green =
        sigma_for_density(
            curves.green_density,
            curves.green_sigma,
            density.green);

    result.blue =
        sigma_for_density(
            curves.blue_density,
            curves.blue_sigma,
            density.blue);

    return result;
}
