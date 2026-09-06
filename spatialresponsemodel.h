// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

// Applies the cascaded, measured negative and print modulation-transfer
// responses to an image. The source MTFs are expressed in cycles/mm; the
// active image width supplies the physical-to-pixel mapping.
class SpatialResponseModel
{
public:
    using Cancel = std::function<bool()>;

    struct Settings
    {
        float image_width_mm = 24.89f;
        float negative_amount = 0.0f;
        float print_amount = 0.0f;
        int kernel_radius = 24;
        int sampling_width_pixels = 0;
        bool gamma24_encoded = false;
    };

    bool load(
        const std::string& negative_mtf_filename,
        const std::string& print_mtf_filename);

    bool valid() const;

    bool apply(
        std::vector<float>& rgb,
        int width,
        int height,
        const Settings& settings,
        const Cancel& cancel = Cancel()) const;

private:
    static bool load_curves(
        const std::string& filename,
        std::array<SampledCurve, 3>& curves);

    static float response(
        const SampledCurve& curve,
        float cycles_per_mm);

    std::vector<float> kernel(
        int channel,
        int width,
        const Settings& settings) const;

    std::array<SampledCurve, 3> negative_;
    std::array<SampledCurve, 3> print_;
    bool valid_ = false;
};
