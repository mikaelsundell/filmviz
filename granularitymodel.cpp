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

namespace {

std::vector<std::string>
split_csv_line(const std::string& line)
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
parse_float(const std::vector<std::string>& fields, std::size_t index, float& value)
{
    if (index >= fields.size() || fields[index].empty()) {
        return false;
    }

    try {
        std::size_t parsed = 0;
        value = std::stof(fields[index], &parsed);

        while (parsed < fields[index].size() && std::isspace(static_cast<unsigned char>(fields[index][parsed]))) {
            ++parsed;
        }

        return parsed == fields[index].size();
    } catch (...) {
        return false;
    }
}

void
append(SampledCurve& curve, float x, float y)
{
    curve.x.push_back(x);
    curve.y.push_back(y);
}

std::uint32_t
mix_bits(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float
uniform_open(std::uint32_t value)
{
    return (static_cast<float>(mix_bits(value) & 0x00ffffffu) + 0.5f) / 16777216.0f;
}

}  // namespace

bool
GranularityModel::load(const std::string& negative_filename, const std::string& print_filename)
{
    negative_ = Curves();
    print_ = Curves();
    valid_ = load_curves(negative_filename, true, negative_) && load_curves(print_filename, false, print_);

    return valid_;
}

bool
GranularityModel::valid() const
{
    return valid_;
}

FilmDensity
GranularityModel::negative_sigma(const FilmDensity& status_m_density) const
{
    return sample(negative_, status_m_density);
}

FilmDensity
GranularityModel::print_sigma(const FilmDensity& status_a_density) const
{
    return sample(print_, status_a_density);
}

float
GranularityModel::normal_sample(std::uint32_t seed, int x, int y, int stage, int channel)
{
    std::uint32_t key = seed;
    key ^= mix_bits(static_cast<std::uint32_t>(x) + 0x9e3779b9u);
    key ^= mix_bits(static_cast<std::uint32_t>(y) + 0x85ebca6bu);
    key ^= mix_bits(static_cast<std::uint32_t>(stage) * 0xc2b2ae35u + static_cast<std::uint32_t>(channel));

    const float u1 = std::max(uniform_open(key), 1e-7f);

    const float u2 = uniform_open(key ^ 0x68bc21ebu);

    constexpr float two_pi = 6.2831853071795864769f;

    return std::sqrt(-2.0f * std::log(u1)) * std::cos(two_pi * u2);
}

namespace {

float hat_integral(float x)
{
    if (x <= -1.0f) return 0.0f;
    if (x < 0.0f) return 0.5f * (x + 1.0f) * (x + 1.0f);
    if (x < 1.0f) return 1.0f - 0.5f * (1.0f - x) * (1.0f - x);
    return 1.0f;
}

float texture_band(std::uint32_t seed, int x, int y, int stage, int channel, float scale)
{
    const float footprint = 1.0f / scale;
    // Far below pixel resolution, use the white-noise area-average limit.
    // Avoid unbounded lattice work on thumbnails; never clamp the grain size.
    if (footprint > 8.0f) {
        return GranularityModel::normal_sample(seed, x, y, stage, channel)
            * GranularityModel::aperture_energy(1.0f, scale);
    }
    const float px = (static_cast<float>(x) + 0.5f) / scale + 0.37f;
    const float py = (static_cast<float>(y) + 0.5f) / scale + 0.61f;
    const float lx = px - 0.5f * footprint, hx = px + 0.5f * footprint;
    const float ly = py - 0.5f * footprint, hy = py + 0.5f * footprint;
    float value = 0.0f;
    for (int j = static_cast<int>(std::floor(ly)); j <= static_cast<int>(std::ceil(hy)); ++j) {
        const float wy = (hat_integral(hy-j) - hat_integral(ly-j)) / footprint;
        for (int i = static_cast<int>(std::floor(lx)); i <= static_cast<int>(std::ceil(hx)); ++i) {
            const float wx = (hat_integral(hx-i) - hat_integral(lx-i)) / footprint;
            value += wx * wy * GranularityModel::normal_sample(seed, i, j, stage, channel);
        }
    }
    return value;
}

float texture_field(std::uint32_t seed, int x, int y, int stage, int channel,
                    const GranularityModel::Texture& texture)
{
    // Empirical stage shapes, not measured stock-specific grain dimensions.
    // Print has finer structure and less broad clustering than the negative.
    const bool print = stage == 1;
    const float fine = texture_band(seed ^ 0xa511e9b3u, x, y, stage, channel,
                                   texture.size_pixels * (print ? 0.50f : 0.85f));
    const float coarse = texture_band(seed ^ 0x63d83595u, x, y, stage, channel,
                                     texture.size_pixels * (print ? 1.10f : 1.80f));
    // Fractions of variance at the reference aperture, not individual pixels.
    return (print ? 0.948683298f * texture.print_fine_normalization
                  : 0.866025404f * texture.fine_normalization) * fine
         + (print ? 0.316227766f * texture.print_coarse_normalization
                  : 0.5f * texture.coarse_normalization) * coarse;
}

float band_variance(int x, int y, float scale)
{
    const float footprint = 1.0f / scale;
    if (footprint > 8.0f) {
        const float rms = GranularityModel::aperture_energy(1.0f, scale);
        return rms * rms;
    }
    const auto axis_energy = [footprint, scale](int coordinate, float offset) {
        const float center = (static_cast<float>(coordinate) + 0.5f) / scale + offset;
        const float low = center - 0.5f * footprint, high = center + 0.5f * footprint;
        float energy = 0.0f;
        for (int i = static_cast<int>(std::floor(low)); i <= static_cast<int>(std::ceil(high)); ++i) {
            const float weight = (hat_integral(high-i) - hat_integral(low-i)) / footprint;
            energy += weight * weight;
        }
        return energy;
    };
    return axis_energy(x, 0.37f) * axis_energy(y, 0.61f);
}

} // namespace

float
GranularityModel::aperture_energy(float footprint_pixels, float lattice_pixels)
{
    const float width = footprint_pixels / lattice_pixels;
    // Phase-averaged squared weight sum of an integrated linear B-spline.
    // In 2D the RMS equals this 1D energy (separable equal-width footprint).
    // This polynomial is the exact integral of the triangular-basis covariance.
    if (width >= 2.0f) return 1.0f / width - 7.0f / (15.0f * width * width);
    if (width >= 1.0f) {
        const float u = 2.0f - width;
        return (width - 7.0f/15.0f + u*u*u*u*u/60.0f) / (width*width);
    }
    return 2.0f/3.0f - width*width/6.0f + width*width*width/20.0f;
}

GranularityModel::Texture
GranularityModel::texture(float size_pixels, float pixels_per_mm)
{
    Texture result;
    result.size_pixels = std::max(size_pixels, 1e-6f);
    // Equal area to a 48um circular aperture: side = diameter * sqrt(pi)/2.
    const float aperture_pixels = 0.048f * 0.886226925f * pixels_per_mm;
    result.aperture_pixels = aperture_pixels;
    result.print_fine_normalization = 1.0f / aperture_energy(aperture_pixels, result.size_pixels * 0.50f);
    result.print_coarse_normalization = 1.0f / aperture_energy(aperture_pixels, result.size_pixels * 1.10f);
    result.fine_normalization = 1.0f / aperture_energy(aperture_pixels, result.size_pixels * 0.85f);
    result.coarse_normalization = 1.0f / aperture_energy(aperture_pixels, result.size_pixels * 1.80f);
    return result;
}

float
GranularityModel::spatial_sample(std::uint32_t seed, int x, int y, int stage, int channel,
                               const Texture& texture)
{
    const float shared = texture_field(seed, x, y, stage, 3, texture);
    const float independent = texture_field(seed, x, y, stage, channel, texture);
    return 0.9f * shared + 0.435889894f * independent;
}

float
GranularityModel::spatial_variance(int x, int y, const Texture& texture, int stage)
{
    const bool print = stage == 1;
    const float fine = print ? texture.print_fine_normalization : texture.fine_normalization;
    const float coarse = print ? texture.print_coarse_normalization : texture.coarse_normalization;
    return (print ? 0.90f : 0.75f) * fine * fine
               * band_variance(x, y, texture.size_pixels * (print ? 0.50f : 0.85f))
         + (print ? 0.10f : 0.25f) * coarse * coarse
               * band_variance(x, y, texture.size_pixels * (print ? 1.10f : 1.80f));
}

bool
GranularityModel::load_curves(const std::string& filename, bool negative_order, Curves& curves)
{
    std::ifstream file(filename.c_str());

    std::string line;

    if (!file || !std::getline(file, line)) {
        return false;
    }

    while (std::getline(file, line)) {
        const auto fields = split_csv_line(line);

        float x = 0.0f;

        if (!parse_float(fields, 0, x)) {
            continue;
        }

        const std::size_t red_density = negative_order ? 3u : 1u;
        const std::size_t green_density = 2u;
        const std::size_t blue_density = negative_order ? 1u : 3u;
        const std::size_t red_sigma = negative_order ? 6u : 4u;
        const std::size_t green_sigma = 5u;
        const std::size_t blue_sigma = negative_order ? 4u : 6u;

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

    return curves.red_density.valid() && curves.green_density.valid() && curves.blue_density.valid()
           && curves.red_sigma.valid() && curves.green_sigma.valid() && curves.blue_sigma.valid();
}

float
GranularityModel::sigma_for_density(const SampledCurve& density, const SampledCurve& sigma, float target_density)
{
    if (!density.valid() || !sigma.valid()) {
        return 0.0f;
    }

    float best_x = density.x.front();
    float best_error = std::numeric_limits<float>::infinity();

    for (std::size_t i = 0; i + 1 < density.y.size(); ++i) {
        const float d0 = density.y[i];
        const float d1 = density.y[i + 1];
        const float low = std::min(d0, d1);
        const float high = std::max(d0, d1);
        const float nearest = std::clamp(target_density, low, high);
        const float error = std::abs(target_density - nearest);

        if (error < best_error) {
            best_error = error;

            const float denominator = d1 - d0;
            const float t = std::abs(denominator) > 1e-12f ? std::clamp((nearest - d0) / denominator, 0.0f, 1.0f)
                                                           : 0.0f;

            best_x = density.x[i] + t * (density.x[i + 1] - density.x[i]);
        }
    }

    return std::max(0.0f, sigma.sample(std::clamp(best_x, sigma.x.front(), sigma.x.back()), 0.0f));
}

FilmDensity
GranularityModel::sample(const Curves& curves, const FilmDensity& density)
{
    FilmDensity result;

    result.red = sigma_for_density(curves.red_density, curves.red_sigma, density.red);

    result.green = sigma_for_density(curves.green_density, curves.green_sigma, density.green);

    result.blue = sigma_for_density(curves.blue_density, curves.blue_sigma, density.blue);

    return result;
}

float
GranularityModel::grain_size_pixels(float scale, int image_width_pixels, float image_width_mm)
{
    return scale * (static_cast<float>(image_width_pixels) / 2048.0f)
        * (24.89f / image_width_mm);
}
