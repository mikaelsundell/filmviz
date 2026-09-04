// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

// Simple RGB 3D LUT container used by FilmViz. The storage/order matches the
// .cube convention: red changes fastest, then green, then blue.
class Lut3D
{
public:
    using RGB = std::array<float, 3>;
    using Evaluator = std::function<bool(const RGB&, RGB&)>;
    using Progress = std::function<void(int completed_slices, int total_slices)>;

    struct Validation
    {
        double mean_abs_error = 0.0;
        double max_abs_error = 0.0;
        int samples = 0;
        bool valid = false;
    };

    bool generate(
        int size,
        const Evaluator& evaluator,
        const Progress& progress = Progress());

    bool valid() const;
    int size() const;

    const RGB& at(
        int red,
        int green,
        int blue) const;

    RGB sample_trilinear(
        const RGB& input) const;

    bool write_cube(
        const std::string& filename,
        const std::string& title,
        const std::vector<std::string>& comments = {}) const;

    Validation validate(
        const Evaluator& evaluator,
        int grid_size = 5) const;

private:
    RGB& at_mutable(
        int red,
        int green,
        int blue);

    int size_ = 0;
    std::vector<RGB> values_;
};
