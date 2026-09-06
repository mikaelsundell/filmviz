// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

// Empirical negative-density colour shaping. The model projects calibrated
// spectral-dye coordinates onto the stock's neutral axis, progressively
// compresses chromatic differences, protects the warm mid-density direction
// and adds chroma-weighted density depth. It is an explicit look control, not
// a claimed reconstruction of interimage chemistry. Its internal amount zero
// is a strict calibrated bypass; public interfaces expose the signed trim
// mapped by amount_from_trim().
class FilmColorResponse
{
public:
    static constexpr float minimum_trim = -4.0f;
    static constexpr float standard_trim = 0.0f;
    static constexpr float maximum_trim = 4.0f;
    static constexpr float standard_amount = 1.5f;
    static constexpr float minimum_warm_tone_separation = 0.0f;
    static constexpr float standard_warm_tone_separation = 1.0f;
    static constexpr float maximum_warm_tone_separation = 2.0f;

    struct Settings
    {
        float amount = 0.0f;
        float chroma_compression = 0.22f;
        float density_depth = 0.08f;
        float chroma_knee = 0.50f;
        float warm_tone_separation = standard_warm_tone_separation;
    };

    FilmColorResponse(
        const FilmDensity& minimum_coordinate,
        const FilmDensity& neutral_reference_coordinate);

    bool valid() const;

    static float amount_from_trim(float trim);

    FilmDensity apply(
        const FilmDensity& coordinate,
        const Settings& settings) const;

private:
    FilmDensity minimum_;
    FilmDensity increment_;
    bool valid_ = false;
};
