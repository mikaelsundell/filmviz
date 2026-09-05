// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <cstddef>
#include <string>
#include <vector>


struct CurveValidationResult
{
    std::string name;

    std::size_t sample_count = 0;
    std::size_t worst_index = 0;

    float x = 0.0f;
    float expected = 0.0f;
    float sampled = 0.0f;
    float max_abs_error = 0.0f;

    bool passed = true;
};

class FilmStock
{
public:
    FilmStock() = default;
    explicit FilmStock(const std::string& name);

    bool load(
        const std::string& sensitivity_filename,
        const std::string& characteristic_filename);

    bool valid() const;

    const std::string& name() const;
    const FilmSpectralSensitivity& sensitivity() const;
    const FilmCharacteristicCurves& characteristic() const;

    // Validate that SampledCurve::sample() reproduces every original CSV knot.
    //
    // Because FilmViz currently uses piecewise-linear interpolation, every
    // source knot should be reproduced exactly apart from floating-point noise.
    std::vector<CurveValidationResult>
    validate_interpolation(
        float tolerance = 1e-6f) const;

private:
    bool load_sensitivity(
        const std::string& filename);

    bool load_characteristic(
        const std::string& filename);

    std::string name_;
    FilmSpectralSensitivity sensitivity_;
    FilmCharacteristicCurves characteristic_;
};
