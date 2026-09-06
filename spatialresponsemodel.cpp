// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "spatialresponsemodel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string
trim(
    const std::string& text)
{
    const auto first =
        std::find_if_not(
            text.begin(),
            text.end(),
            [](unsigned char value) { return std::isspace(value); });
    const auto last =
        std::find_if_not(
            text.rbegin(),
            text.rend(),
            [](unsigned char value) { return std::isspace(value); })
            .base();

    return first < last
        ? std::string(first, last)
        : std::string();
}

std::vector<std::string>
split_csv(
    const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(trim(field));
    }

    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }

    return fields;
}

int
column_index(
    const std::vector<std::string>& header,
    const std::string& first,
    const std::string& second = std::string())
{
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (header[index] == first
            || (!second.empty() && header[index] == second)) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

bool
parse_float(
    const std::string& text,
    float& value)
{
    if (text.empty()) {
        return false;
    }

    try {
        std::size_t consumed = 0;
        value = std::stof(text, &consumed);
        return consumed == text.size() && std::isfinite(value);
    }
    catch (...) {
        return false;
    }
}

float
to_linear(
    float value,
    bool gamma24)
{
    return gamma24
        ? std::pow(std::max(0.0f, value), 2.4f)
        : value;
}

float
from_linear(
    float value,
    bool gamma24)
{
    return gamma24
        ? std::pow(std::max(0.0f, value), 1.0f / 2.4f)
        : value;
}

} // namespace

bool
SpatialResponseModel::load(
    const std::string& negative_mtf_filename,
    const std::string& print_mtf_filename)
{
    negative_ = {};
    print_ = {};
    valid_ =
        load_curves(negative_mtf_filename, negative_)
        && load_curves(print_mtf_filename, print_);
    return valid_;
}

bool
SpatialResponseModel::valid() const
{
    return valid_;
}

bool
SpatialResponseModel::load_curves(
    const std::string& filename,
    std::array<SampledCurve, 3>& curves)
{
    std::ifstream file(filename);
    std::string line;

    if (!file || !std::getline(file, line)) {
        return false;
    }

    const std::vector<std::string> header = split_csv(line);
    const int frequency_column =
        column_index(
            header,
            "cycles_per_mm",
            "spatial_frequency_cycles_per_mm");
    const int columns[3] = {
        column_index(header, "red_mtf_percent", "red_response_percent"),
        column_index(header, "green_mtf_percent", "green_response_percent"),
        column_index(header, "blue_mtf_percent", "blue_response_percent")
    };

    if (frequency_column < 0
        || columns[0] < 0
        || columns[1] < 0
        || columns[2] < 0) {
        return false;
    }

    while (std::getline(file, line)) {
        const std::vector<std::string> fields = split_csv(line);
        float frequency = 0.0f;

        if (frequency_column >= static_cast<int>(fields.size())
            || !parse_float(fields[frequency_column], frequency)) {
            continue;
        }

        for (int channel = 0; channel < 3; ++channel) {
            float percent = 0.0f;

            if (columns[channel] < static_cast<int>(fields.size())
                && parse_float(fields[columns[channel]], percent)) {
                curves[channel].x.push_back(frequency);
                curves[channel].y.push_back(percent * 0.01f);
            }
        }
    }

    return
        curves[0].valid()
        && curves[1].valid()
        && curves[2].valid();
}

float
SpatialResponseModel::response(
    const SampledCurve& curve,
    float cycles_per_mm)
{
    if (!curve.valid()) {
        return 1.0f;
    }

    if (cycles_per_mm <= 0.0f) {
        return 1.0f;
    }

    if (cycles_per_mm < curve.x.front()) {
        const float fraction =
            cycles_per_mm / curve.x.front();
        return
            1.0f
            + fraction * (curve.y.front() - 1.0f);
    }

    return curve.sample(cycles_per_mm, curve.y.back());
}

std::vector<float>
SpatialResponseModel::kernel(
    int channel,
    int width,
    const Settings& settings) const
{
    const int radius =
        std::clamp(settings.kernel_radius, 1, 64);
    const int transform_size = 512;
    const int half = transform_size / 2;
    const int sampling_width =
        settings.sampling_width_pixels > 0
            ? settings.sampling_width_pixels
            : width;
    const float pixels_per_mm =
        static_cast<float>(sampling_width)
        / settings.image_width_mm;
    const float nyquist = 0.5f * pixels_per_mm;
    std::vector<float> result(
        static_cast<std::size_t>(2 * radius + 1),
        0.0f);

    for (int tap = -radius; tap <= radius; ++tap) {
        float value = 0.0f;

        for (int frequency_index = 0;
             frequency_index <= half;
             ++frequency_index) {
            const float frequency =
                nyquist
                * static_cast<float>(frequency_index)
                / static_cast<float>(half);
            const float negative_response =
                1.0f
                + settings.negative_amount
                    * (response(negative_[channel], frequency) - 1.0f);
            const float print_response =
                1.0f
                + settings.print_amount
                    * (response(print_[channel], frequency) - 1.0f);
            const float combined =
                std::max(0.0f, negative_response)
                * std::max(0.0f, print_response);
            const float endpoint_weight =
                frequency_index == 0 || frequency_index == half
                    ? 1.0f
                    : 2.0f;

            value +=
                endpoint_weight
                * combined
                * std::cos(
                    2.0f
                    * kPi
                    * static_cast<float>(frequency_index * tap)
                    / static_cast<float>(transform_size));
        }

        value /= static_cast<float>(transform_size);

        const float window =
            0.5f
            + 0.5f
                * std::cos(
                    kPi
                    * static_cast<float>(tap)
                    / static_cast<float>(radius + 1));

        result[static_cast<std::size_t>(tap + radius)] =
            value * window;
    }

    float sum = 0.0f;
    for (float value : result) {
        sum += value;
    }

    if (std::abs(sum) <= std::numeric_limits<float>::epsilon()) {
        result.assign(result.size(), 0.0f);
        result[static_cast<std::size_t>(radius)] = 1.0f;
    }
    else {
        for (float& value : result) {
            value /= sum;
        }
    }

    return result;
}

bool
SpatialResponseModel::apply(
    std::vector<float>& rgb,
    int width,
    int height,
    const Settings& settings,
    const Cancel& cancel) const
{
    if (!valid_
        || width <= 0
        || height <= 0
        || settings.image_width_mm <= 0.0f
        || settings.negative_amount < 0.0f
        || settings.print_amount < 0.0f
        || rgb.size()
            != static_cast<std::size_t>(width)
                * static_cast<std::size_t>(height)
                * 3u) {
        return false;
    }

    if (settings.negative_amount <= 0.0f
        && settings.print_amount <= 0.0f) {
        return true;
    }

    std::array<std::vector<float>, 3> kernels;
    for (int channel = 0; channel < 3; ++channel) {
        kernels[channel] = kernel(channel, width, settings);
    }

    const int radius =
        static_cast<int>(kernels[0].size() / 2u);
    std::vector<float> source(rgb.size());
    std::vector<float> horizontal(rgb.size(), 0.0f);

    for (std::size_t index = 0; index < rgb.size(); ++index) {
        source[index] =
            to_linear(rgb[index], settings.gamma24_encoded);
    }

    for (int y = 0; y < height; ++y) {
        if (cancel && cancel()) {
            return false;
        }

        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                float value = 0.0f;

                for (int tap = -radius; tap <= radius; ++tap) {
                    const int sample_x =
                        std::clamp(x + tap, 0, width - 1);
                    const std::size_t index =
                        (static_cast<std::size_t>(y)
                             * static_cast<std::size_t>(width)
                         + static_cast<std::size_t>(sample_x))
                            * 3u
                        + static_cast<std::size_t>(channel);
                    value +=
                        source[index]
                        * kernels[channel][static_cast<std::size_t>(tap + radius)];
                }

                horizontal[
                    (static_cast<std::size_t>(y)
                         * static_cast<std::size_t>(width)
                     + static_cast<std::size_t>(x))
                        * 3u
                    + static_cast<std::size_t>(channel)] = value;
            }
        }
    }

    for (int y = 0; y < height; ++y) {
        if (cancel && cancel()) {
            return false;
        }

        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                float value = 0.0f;

                for (int tap = -radius; tap <= radius; ++tap) {
                    const int sample_y =
                        std::clamp(y + tap, 0, height - 1);
                    const std::size_t index =
                        (static_cast<std::size_t>(sample_y)
                             * static_cast<std::size_t>(width)
                         + static_cast<std::size_t>(x))
                            * 3u
                        + static_cast<std::size_t>(channel);
                    value +=
                        horizontal[index]
                        * kernels[channel][static_cast<std::size_t>(tap + radius)];
                }

                const std::size_t index =
                    (static_cast<std::size_t>(y)
                         * static_cast<std::size_t>(width)
                     + static_cast<std::size_t>(x))
                        * 3u
                    + static_cast<std::size_t>(channel);
                rgb[index] =
                    std::clamp(
                        from_linear(
                            value,
                            settings.gamma24_encoded),
                        0.0f,
                        1.0f);
            }
        }
    }

    return true;
}
