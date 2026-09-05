// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

// Profile-independent bleach-bypass process model.
//
// FilmViz deliberately keeps this outside FilmStock / PrintFilmStock. The
// stock profiles remain measured source descriptions; this class applies a
// process look to an already-synthesized spectral-density curve.
//
// The current implementation is a reference-look approximation rather than a
// measured retained-silver model:
//   - negative bypass preserves mean spectral density while introducing a
//     cool/cyan printer bias through a zero-mean wavelength tilt;
//   - print bypass preserves mean spectral density while contracting spectral
//     density differences, reducing colour saturation without adding a neutral
//     density veil or a large contrast change.
class BleachBypass
{
public:
    struct Result
    {
        SampledCurve spectral_density;
        float mean_density_before = 0.0f;
        float mean_density_after = 0.0f;
        float spectral_span_before = 0.0f;
        float spectral_span_after = 0.0f;
        bool valid = false;
    };

    // Zero is normal processing; one is the full modeled negative-bypass look.
    static Result apply_negative(
        const SampledCurve& spectral_density,
        float amount);

    // Zero is normal processing; one is the full modeled print-bypass look.
    static Result apply_print(
        const SampledCurve& spectral_density,
        float amount);

private:
    static float mean_density(
        const SampledCurve& spectral_density);

    static float spectral_span(
        const SampledCurve& spectral_density);
};
