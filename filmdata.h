// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

struct SampledCurve
{
    std::vector<float> x;
    std::vector<float> y;

    bool empty() const
    {
        return x.empty() || y.empty();
    }

    bool valid() const
    {
        return !empty() && x.size() == y.size();
    }

    float sample(
        float position,
        float outside_value = 0.0f) const
    {
        if (!valid()) {
            return outside_value;
        }

        if (position < x.front() || position > x.back()) {
            return outside_value;
        }

        const auto upper =
            std::lower_bound(
                x.begin(),
                x.end(),
                position);

        if (upper == x.begin()) {
            return y.front();
        }

        if (upper == x.end()) {
            return y.back();
        }

        const std::size_t i1 =
            static_cast<std::size_t>(
                upper - x.begin());

        const std::size_t i0 = i1 - 1;

        const float x0 = x[i0];
        const float x1 = x[i1];
        const float y0 = y[i0];
        const float y1 = y[i1];

        if (std::abs(x1 - x0) < 1e-12f) {
            return y0;
        }

        const float t =
            (position - x0)
            / (x1 - x0);

        return y0 + t * (y1 - y0);
    }
};

struct FilmSpectralSensitivity
{
    SampledCurve blue_sensitive_log;
    SampledCurve green_sensitive_log;
    SampledCurve red_sensitive_log;
};

struct FilmCharacteristicCurves
{
    SampledCurve blue_density;
    SampledCurve green_density;
    SampledCurve red_density;
};

struct FilmExposure
{
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
};

struct FilmLogExposure
{
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
};

struct FilmDensity
{
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
};

struct FilmExposureBalance
{
    float red_log_offset = 0.0f;
    float green_log_offset = 0.0f;
    float blue_log_offset = 0.0f;

    FilmExposure neutral_reference;
};
