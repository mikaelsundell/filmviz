// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "filmformat.h"
#include "granularitymodel.h"
#include "imageprocessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int patch_size = 128;
using RGB = std::array<float, 3>;

struct Moments {
    double sum = 0.0;
    double square = 0.0;
    std::size_t count = 0;

    void add(double value) { sum += value; square += value * value; ++count; }
    double mean() const { return count ? sum / count : 0.0; }
    double variance() const { return count ? std::max(0.0, square / count - mean() * mean()) : 0.0; }
    double sd() const { return std::sqrt(variance()); }
    double standard_error() const { return count > 1 ? std::sqrt(variance() / (count - 1)) : 0.0; }
};

double luma(const RGB& rgb)
{
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
}

struct Patch {
    std::array<Moments, 3> encoded, linear, preclip_linear;
    Moments luminance, blue_yellow;
    std::vector<double> field = std::vector<double>(patch_size * patch_size);
    std::size_t clipped = 0;

    bool add(int x, int y, const RGB& base_linear, const RGB& base_encoded, const RGB& delta)
    {
        // Match the production CPU composite, with chroma=1 and both MTFs off.
        const RGB noisy = ImageProcessor::composite_grain(base_linear, delta, 1.0f);
        RGB difference;
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(noisy[c])) return false;
            preclip_linear[c].add(noisy[c] - base_linear[c]);
            const float encoded_value = std::pow(std::max(0.0f, noisy[c]), 1.0f / 2.4f);
            if (encoded_value <= 0.0f || encoded_value >= 1.0f) ++clipped;
            const float stored = std::clamp(encoded_value, 0.0f, 1.0f);
            difference[c] = stored - base_encoded[c];
            encoded[c].add(difference[c]);
            linear[c].add(std::pow(stored, 2.4f) - base_linear[c]);
        }
        const double value = luma(difference);
        field[y * patch_size + x] = value;
        luminance.add(value);
        blue_yellow.add(difference[2] - 0.5 * (difference[0] + difference[1]));
        return true;
    }

    double lag_one() const
    {
        Moments left, right;
        double product = 0.0;
        for (int y = 0; y < patch_size; ++y) {
            for (int x = 0; x + 1 < patch_size; ++x) {
                const double a = field[y * patch_size + x];
                const double b = field[y * patch_size + x + 1];
                left.add(a); right.add(b); product += a * b;
            }
        }
        const double denominator = left.sd() * right.sd();
        return denominator > 1e-20 ? (product / left.count - left.mean() * right.mean()) / denominator : 0.0;
    }

    double coarse_fraction() const
    {
        Moments blocks;
        for (int y = 0; y < patch_size; y += 2) {
            for (int x = 0; x < patch_size; x += 2) {
                const int i = y * patch_size + x;
                blocks.add(0.25 * (field[i] + field[i+1] + field[i+patch_size] + field[i+patch_size+1]));
            }
        }
        return luminance.variance() > 1e-20 ? blocks.variance() / luminance.variance() : 0.0;
    }
};

void density_columns(const FilmDensity& value)
{
    std::cout << ',' << value.red << ',' << value.green << ',' << value.blue;
}

} // namespace

int main(int argc, const char* argv[])
{
    if (argc < 2 || argc > 8) {
        std::cerr << "Usage: " << argv[0]
                  << " RESOURCES [FORMAT=super-35] [WIDTH=4448] [SCALE=1]"
                     " [NEGATIVE=verita-200d] [PRINT=kodak-2383] [SEEDS=32]\n"
                     "Writes CSV to stdout; seeds 1..SEEDS, chroma 1, no MTF/halation.\n";
        return 1;
    }
    try {
        const auto* format = FilmFormatCatalog::find(argc > 2 ? argv[2] : "super-35");
        const int width = argc > 3 ? std::stoi(argv[3]) : 4448;
        const float scale = argc > 4 ? std::stof(argv[4]) : 1.0f;
        const int seed_count = argc > 7 ? std::stoi(argv[7]) : 32;
        if (seed_count < 2 || seed_count > 256)
            throw std::runtime_error("SEEDS must be 2..256");
        if (!format || format->identifier == "custom" || width < 64 || width > 65536
            || !std::isfinite(scale) || scale < 0.25f || scale > 10.0f)
            throw std::runtime_error("use a physical format preset, width 64..65536 and scale 0.25..10");
        FilmPipeline::Settings settings;
        settings.resources_directory = argv[1];
        if (argc > 5) settings.negative_profile = argv[5];
        if (argc > 6) settings.print_profile = argv[6];
        FilmPipeline pipeline;
        if (!pipeline.initialize(settings)) throw std::runtime_error(pipeline.error());
        const float size = GranularityModel::grain_size_pixels(scale, width, format->image_width_mm);
        const auto texture = GranularityModel::texture(size, static_cast<float>(width) / format->image_width_mm);
        std::cerr << "Neutral AP0 ramp; " << settings.negative_profile << " / " << settings.print_profile
                  << "; " << format->identifier << "; " << width << "px; scale " << scale
                  << "; seeds " << seed_count << "; empirical size " << size << "px. CPU response approximation.\n";
        std::cout << std::setprecision(9)
                  << "stops,stage,base_code255,negative_density_r,negative_density_g,negative_density_b"
                     ",print_density_r,print_density_g,print_density_b,negative_sigma_r,negative_sigma_g,negative_sigma_b"
                     ",print_sigma_r,print_sigma_g,print_sigma_b"
                     ",mean_r255,mean_g255,mean_b255,sd_r255,sd_g255,sd_b255"
                     ",linear_mean_r,linear_mean_g,linear_mean_b,luma_sd255,relative_percent"
                     ",lag1,block2_variance_fraction,blue_yellow_sd255,clipped_channel_fraction"
                     ",seed_count,preclip_linear_mean_r,preclip_linear_mean_g,preclip_linear_mean_b"
                     ",mean_r255_se,mean_g255_se,mean_b255_se"
                     ",linear_mean_r_se,linear_mean_g_se,linear_mean_b_se"
                     ",preclip_linear_mean_r_se,preclip_linear_mean_g_se,preclip_linear_mean_b_se,grain_visibility\n";
        for (int stops = -6; stops <= 6; ++stops) {
            const float level = 0.18f * std::exp2(static_cast<float>(stops));
            const auto base = pipeline.process({{level, level, level}});
            FilmPipeline::GrainResponse response;
            if (!base.valid || !pipeline.grain_response(base, true, response))
                throw std::runtime_error("density response failed at stop " + std::to_string(stops));
            RGB base_encoded, base_linear;
            for (int c = 0; c < 3; ++c) {
                base_encoded[c] = std::clamp(base.rec709_gamma24[c], 0.0f, 1.0f);
                base_linear[c] = std::pow(base_encoded[c], 2.4f);
            }
            // Each seed is one independent statistical unit. Pixels inside a
            // patch are correlated and must not be counted as independent trials.
            std::array<std::array<Moments, 18>, 3> summaries;
            for (int seed = 1; seed <= seed_count; ++seed) {
                std::array<Patch, 3> patches;
                for (int y = 0; y < patch_size; ++y) {
                    for (int x = 0; x < patch_size; ++x) {
                        // Use the same realization for every density and for the
                        // isolated/combined rows. Do not confuse different seeds
                        // with a change of density-dependent structure.
                        const auto residual = ImageProcessor::grain_residuals(response, base_linear, texture,
                                                                             static_cast<std::uint32_t>(seed), x, y, 1.0f, 1.0f);
                        for (int stage = 0; stage < 3; ++stage) {
                            RGB delta;
                            for (int c = 0; c < 3; ++c)
                                delta[c] = stage == 0 ? residual[c] : stage == 1 ? residual[c+3]
                                                                                     : residual[c] + residual[c+3];
                            if (!patches[stage].add(x, y, base_linear, base_encoded, delta))
                                throw std::runtime_error("non-finite grain composite");
                        }
                    }
                }
                for (int stage = 0; stage < 3; ++stage) {
                    const auto& patch = patches[stage];
                    auto& summary = summaries[stage];
                    for (int c = 0; c < 3; ++c) {
                        summary[c].add(patch.encoded[c].mean() * 255.0);
                        summary[3+c].add(patch.encoded[c].sd() * 255.0);
                        summary[6+c].add(patch.linear[c].mean());
                        summary[15+c].add(patch.preclip_linear[c].mean());
                    }
                    summary[9].add(patch.luminance.sd() * 255.0);
                    summary[10].add(100.0 * patch.luminance.sd() / std::max(1e-9, luma(base_encoded)));
                    summary[11].add(patch.lag_one());
                    summary[12].add(patch.coarse_fraction());
                    summary[13].add(patch.blue_yellow.sd() * 255.0);
                    summary[14].add(static_cast<double>(patch.clipped) / (patch_size * patch_size * 3));
                }
            }
            const char* names[] = {"negative", "print", "combined"};
            for (int stage = 0; stage < 3; ++stage) {
                const auto& summary = summaries[stage];
                std::cout << stops << ',' << names[stage] << ',' << luma(base_encoded) * 255.0;
                density_columns(base.negative_status_m_density);
                density_columns(base.print_density);
                density_columns(base.negative_granularity_sigma);
                density_columns(base.print_granularity_sigma);
                // Existing columns now contain averages of per-seed statistics;
                // SD/correlation are not computed by concatenating patch borders.
                for (int i = 0; i < 15; ++i) std::cout << ',' << summary[i].mean();
                std::cout << ',' << seed_count;
                for (int c = 0; c < 3; ++c) std::cout << ',' << summary[15+c].mean();
                for (int offset : {0, 6, 15})
                    for (int c = 0; c < 3; ++c)
                        std::cout << ',' << summary[offset+c].standard_error();
                std::cout << ',' << ImageProcessor::grain_visibility(base_linear) << '\n';
            }
            std::cerr << "Completed stop " << stops << " (" << seed_count << " seeds).\n";
        }
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Grain density diagnostic: " << error.what() << '\n';
        return 1;
    }
}
