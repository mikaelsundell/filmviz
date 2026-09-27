// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <cstdint>
#include <string>

// Interpolates Kodak diffuse-RMS-granularity measurements in their native
// density coordinates. Spatial synthesis is deliberately kept separate.
class GranularityModel {
public:
    bool load(const std::string& negative_filename, const std::string& print_filename);

    bool valid() const;

    FilmDensity negative_sigma(const FilmDensity& status_m_density) const;

    FilmDensity print_sigma(const FilmDensity& status_a_density) const;

    // Empirical size convention: scale 1 is one pixel at 2048px / 24.89mm.
    // Use full image width, not the render window. This is not crystal metrology.
    static float grain_size_pixels(float scale, int image_width_pixels, float image_width_mm);

    // Reproducible unit-normal sample for image-domain grain synthesis.
    static float normal_sample(std::uint32_t seed, int x, int y, int stage, int channel);

    struct Texture {
        float size_pixels = 1.0f;
        float fine_normalization = 1.0f;
        float coarse_normalization = 1.0f;
        float print_fine_normalization = 1.0f;
        float print_coarse_normalization = 1.0f;
        float aperture_pixels = 1.0f;
    };

    // Integrate the continuous lattice over each pixel; normalize to an
    // equal-area square approximation of a 48um circular measuring aperture.
    // The aperture and spatial spectrum are explicit rendering assumptions.
    static Texture texture(float size_pixels, float pixels_per_mm);
    static float spatial_sample(std::uint32_t seed, int x, int y, int stage, int channel,
                                const Texture& texture);
    // Ensemble variance at this pixel's lattice phase, including both bands.
    static float spatial_variance(int x, int y, const Texture& texture, int stage = 0);
    static float aperture_energy(float footprint_pixels, float lattice_pixels);


private:
    struct Curves {
        SampledCurve red_density;
        SampledCurve green_density;
        SampledCurve blue_density;
        SampledCurve red_sigma;
        SampledCurve green_sigma;
        SampledCurve blue_sigma;
    };

    static bool load_curves(const std::string& filename, bool negative_order, Curves& curves);

    static float sigma_for_density(const SampledCurve& density, const SampledCurve& sigma, float target_density);

    static FilmDensity sample(const Curves& curves, const FilmDensity& density);

    Curves negative_;
    Curves print_;
    bool valid_ = false;
};
