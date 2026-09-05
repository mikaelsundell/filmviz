// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "diagramwriter.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

using namespace OIIO;

namespace {

// Status-A mapping audit diagnostic-only ANSI Status-A weighting functions.
//
// Samples are 340..830 nm in 10 nm steps. These values are the ANSI Status-A
// arrays exposed by python-colormath's density_standards module. They are used
// here only to test the mapping between Kodak's Status-A sensitometric records
// and the published peak-normalized C/M/Y dye shapes; the active renderer is
// unchanged.
constexpr std::array<float, 50> kStatusARed = {{
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0.37f,43.45f,100.00f,74.30f,40.18f,19.32f,7.94f,3.56f,1.46f,0.60f,
    0.24f,0.09f,0.04f,0.01f,0.01f,0,0,0,0,0,0,0,0,0,0
}};

constexpr std::array<float, 50> kStatusAGreen = {{
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0.04f,6.64f,60.53f,100.00f,80.54f,44.06f,16.63f,4.06f,0.58f,0.04f,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
}};

constexpr std::array<float, 50> kStatusABlue = {{
    0,0,0,0,0,0,0,0,
    4.00f,65.92f,100.00f,81.66f,41.69f,10.96f,0.79f,0.04f,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
}};

using Matrix3 = std::array<std::array<float, 3>, 3>;
using Vector3 = std::array<float, 3>;

bool solve_3x3(Matrix3 matrix, Vector3 rhs, Vector3& solution);

float
monotone_cubic_sample(
    const SampledCurve& curve,
    float x)
{
    const std::size_t n = curve.x.size();
    if (n == 0 || curve.y.size() != n) {
        return 0.0f;
    }
    if (n == 1 || x <= curve.x.front()) {
        return curve.y.front();
    }
    if (x >= curve.x.back()) {
        return curve.y.back();
    }

    std::vector<float> h(n - 1, 0.0f);
    std::vector<float> delta(n - 1, 0.0f);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        h[i] = curve.x[i + 1] - curve.x[i];
        if (h[i] <= 0.0f) {
            return curve.sample(x, curve.y.front());
        }
        delta[i] = (curve.y[i + 1] - curve.y[i]) / h[i];
    }

    std::vector<float> tangent(n, 0.0f);
    if (n == 2) {
        tangent[0] = tangent[1] = delta[0];
    }
    else {
        tangent[0] = ((2.0f * h[0] + h[1]) * delta[0] - h[0] * delta[1])
            / (h[0] + h[1]);
        if (tangent[0] * delta[0] <= 0.0f) {
            tangent[0] = 0.0f;
        }
        else if (delta[0] * delta[1] < 0.0f
                 && std::abs(tangent[0]) > std::abs(3.0f * delta[0])) {
            tangent[0] = 3.0f * delta[0];
        }

        for (std::size_t i = 1; i + 1 < n; ++i) {
            if (delta[i - 1] * delta[i] <= 0.0f) {
                tangent[i] = 0.0f;
            }
            else {
                const float w1 = 2.0f * h[i] + h[i - 1];
                const float w2 = h[i] + 2.0f * h[i - 1];
                tangent[i] = (w1 + w2)
                    / (w1 / delta[i - 1] + w2 / delta[i]);
            }
        }

        const std::size_t last = n - 1;
        tangent[last] = ((2.0f * h[last - 1] + h[last - 2]) * delta[last - 1]
                         - h[last - 1] * delta[last - 2])
            / (h[last - 1] + h[last - 2]);
        if (tangent[last] * delta[last - 1] <= 0.0f) {
            tangent[last] = 0.0f;
        }
        else if (delta[last - 1] * delta[last - 2] < 0.0f
                 && std::abs(tangent[last]) > std::abs(3.0f * delta[last - 1])) {
            tangent[last] = 3.0f * delta[last - 1];
        }
    }

    std::size_t interval = 0;
    while (interval + 1 < n && x > curve.x[interval + 1]) {
        ++interval;
    }
    interval = std::min(interval, n - 2);

    const float interval_h = curve.x[interval + 1] - curve.x[interval];
    const float t = (x - curve.x[interval]) / interval_h;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;

    return h00 * curve.y[interval]
        + h10 * interval_h * tangent[interval]
        + h01 * curve.y[interval + 1]
        + h11 * interval_h * tangent[interval + 1];
}

float
matrix_frobenius_condition(
    const Matrix3& matrix)
{
    float norm_squared = 0.0f;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            norm_squared += matrix[row][column] * matrix[row][column];
        }
    }

    Matrix3 inverse = {{{{0.0f,0.0f,0.0f}},{{0.0f,0.0f,0.0f}},{{0.0f,0.0f,0.0f}}}};
    for (int column = 0; column < 3; ++column) {
        Vector3 rhs = {{0.0f, 0.0f, 0.0f}};
        rhs[column] = 1.0f;
        Vector3 solution = {{0.0f, 0.0f, 0.0f}};
        if (!solve_3x3(matrix, rhs, solution)) {
            return std::numeric_limits<float>::infinity();
        }
        for (int row = 0; row < 3; ++row) {
            inverse[row][column] = solution[row];
        }
    }

    float inverse_norm_squared = 0.0f;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            inverse_norm_squared += inverse[row][column] * inverse[row][column];
        }
    }

    return std::sqrt(norm_squared) * std::sqrt(inverse_norm_squared);
}

float
status_a_channel_density(
    const PrintFilmStock& stock,
    const Vector3& amplitudes,
    const std::array<float, 50>& weights)
{
    const auto& source = stock.dye_density();
    double numerator = 0.0;
    double denominator = 0.0;

    for (std::size_t i = 0; i < weights.size(); ++i) {
        const float weight = weights[i];
        if (weight <= 0.0f) {
            continue;
        }

        const float wavelength_nm = 340.0f + 10.0f * static_cast<float>(i);
        const float cyan = source.cyan_density.sample(
            wavelength_nm, std::numeric_limits<float>::quiet_NaN());
        const float magenta = source.magenta_density.sample(
            wavelength_nm, std::numeric_limits<float>::quiet_NaN());
        const float yellow = source.yellow_density.sample(
            wavelength_nm, std::numeric_limits<float>::quiet_NaN());

        if (!std::isfinite(cyan)
            || !std::isfinite(magenta)
            || !std::isfinite(yellow)) {
            continue;
        }

        const float spectral_density =
            amplitudes[0] * cyan
            + amplitudes[1] * magenta
            + amplitudes[2] * yellow;

        const double transmittance =
            std::pow(10.0, -static_cast<double>(spectral_density));

        numerator += static_cast<double>(weight) * transmittance;
        denominator += static_cast<double>(weight);
    }

    if (numerator <= 0.0 || denominator <= 0.0) {
        return 0.0f;
    }

    return static_cast<float>(
        -std::log10(numerator / denominator));
}

Vector3
status_a_density(
    const PrintFilmStock& stock,
    const Vector3& amplitudes)
{
    return {{
        status_a_channel_density(stock, amplitudes, kStatusARed),
        status_a_channel_density(stock, amplitudes, kStatusAGreen),
        status_a_channel_density(stock, amplitudes, kStatusABlue)
    }};
}

Matrix3
status_a_jacobian(
    const PrintFilmStock& stock,
    const Vector3& amplitudes)
{
    Matrix3 result = {{
        {{0.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 0.0f}}
    }};

    constexpr float h = 1e-3f;

    for (int column = 0; column < 3; ++column) {
        Vector3 plus = amplitudes;
        Vector3 minus = amplitudes;
        plus[column] += h;
        minus[column] = std::max(0.0f, minus[column] - h);

        const Vector3 dp = status_a_density(stock, plus);
        const Vector3 dm = status_a_density(stock, minus);
        const float denominator = plus[column] - minus[column];

        for (int row = 0; row < 3; ++row) {
            result[row][column] =
                denominator > 0.0f
                    ? (dp[row] - dm[row]) / denominator
                    : 0.0f;
        }
    }

    return result;
}

bool
solve_3x3(
    Matrix3 a,
    Vector3 b,
    Vector3& x)
{
    for (int pivot = 0; pivot < 3; ++pivot) {
        int best = pivot;
        float best_abs = std::abs(a[pivot][pivot]);
        for (int row = pivot + 1; row < 3; ++row) {
            const float v = std::abs(a[row][pivot]);
            if (v > best_abs) {
                best_abs = v;
                best = row;
            }
        }

        if (best_abs < 1e-8f) {
            return false;
        }

        if (best != pivot) {
            std::swap(a[best], a[pivot]);
            std::swap(b[best], b[pivot]);
        }

        const float inv_pivot = 1.0f / a[pivot][pivot];
        for (int c = pivot; c < 3; ++c) {
            a[pivot][c] *= inv_pivot;
        }
        b[pivot] *= inv_pivot;

        for (int row = 0; row < 3; ++row) {
            if (row == pivot) {
                continue;
            }
            const float f = a[row][pivot];
            for (int c = pivot; c < 3; ++c) {
                a[row][c] -= f * a[pivot][c];
            }
            b[row] -= f * b[pivot];
        }
    }

    x = b;
    return true;
}

Vector3
solve_status_a_neutral_amplitudes(
    const PrintFilmStock& stock,
    Vector3 amplitudes)
{
    const Vector3 target = {{1.0f, 1.0f, 1.0f}};

    for (int iteration = 0; iteration < 12; ++iteration) {
        const Vector3 current = status_a_density(stock, amplitudes);
        Vector3 error = {{
            target[0] - current[0],
            target[1] - current[1],
            target[2] - current[2]
        }};

        Vector3 delta = {{0.0f, 0.0f, 0.0f}};
        if (!solve_3x3(
                status_a_jacobian(stock, amplitudes),
                error,
                delta)) {
            break;
        }

        float max_delta = 0.0f;
        for (int i = 0; i < 3; ++i) {
            amplitudes[i] = std::max(0.0f, amplitudes[i] + delta[i]);
            max_delta = std::max(max_delta, std::abs(delta[i]));
        }

        if (max_delta < 1e-6f) {
            break;
        }
    }

    return amplitudes;
}

SampledCurve
source_transmittance_from_amplitudes(
    const PrintFilmStock& stock,
    const Vector3& amplitudes,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm)
{
    SampledCurve result;
    const auto& source = stock.dye_density();

    for (float wavelength_nm = wavelength_min_nm;
         wavelength_nm <= wavelength_max_nm + 0.001f;
         wavelength_nm += wavelength_step_nm) {

        const float cyan = source.cyan_density.sample(
            wavelength_nm, std::numeric_limits<float>::quiet_NaN());
        const float magenta = source.magenta_density.sample(
            wavelength_nm, std::numeric_limits<float>::quiet_NaN());
        const float yellow = source.yellow_density.sample(
            wavelength_nm, std::numeric_limits<float>::quiet_NaN());

        if (!std::isfinite(cyan)
            || !std::isfinite(magenta)
            || !std::isfinite(yellow)) {
            return SampledCurve();
        }

        const float density =
            amplitudes[0] * cyan
            + amplitudes[1] * magenta
            + amplitudes[2] * yellow;

        result.x.push_back(wavelength_nm);
        result.y.push_back(std::pow(10.0f, -density));
    }

    return result;
}

struct PlotMapper
{
    float xmin = 0.0f;
    float xmax = 1.0f;
    float ymin = 0.0f;
    float ymax = 1.0f;

    int width = 1200;
    int height = 800;

    int margin_left = 100;
    int margin_right = 40;
    // Extra room at the top for:
    //   title
    //   optional secondary x-axis title
    //   optional secondary x-axis values
    int margin_top = 120;
    int margin_bottom = 90;

    bool log_x = false;
    bool log_y = false;

    static float axis_coordinate(
        float value,
        bool logarithmic)
    {
        if (!logarithmic) {
            return value;
        }

        return std::log10(
            std::max(
                value,
                1e-20f));
    }

    void to_pixel(
        float x,
        float y,
        int& px,
        int& py) const
    {
        const float mapped_x =
            axis_coordinate(
                x,
                log_x);

        const float mapped_xmin =
            axis_coordinate(
                xmin,
                log_x);

        const float mapped_xmax =
            axis_coordinate(
                xmax,
                log_x);

        const float mapped_y =
            axis_coordinate(
                y,
                log_y);

        const float mapped_ymin =
            axis_coordinate(
                ymin,
                log_y);

        const float mapped_ymax =
            axis_coordinate(
                ymax,
                log_y);

        const float nx =
            (mapped_x - mapped_xmin)
            / std::max(
                1e-12f,
                mapped_xmax - mapped_xmin);

        const float ny =
            (mapped_y - mapped_ymin)
            / std::max(
                1e-12f,
                mapped_ymax - mapped_ymin);

        const int plot_width =
            width
            - margin_left
            - margin_right;

        const int plot_height =
            height
            - margin_top
            - margin_bottom;

        px =
            margin_left
            + static_cast<int>(
                std::lround(
                    nx * plot_width));

        py =
            margin_top
            + plot_height
            - static_cast<int>(
                std::lround(
                    ny * plot_height));
    }
};

void
draw_line(
    ImageBuf& image,
    int x0,
    int y0,
    int x1,
    int y1,
    const float* rgb)
{
    const int dx =
        std::abs(x1 - x0);

    const int dy =
        std::abs(y1 - y0);

    const int steps =
        std::max(dx, dy);

    if (steps <= 0) {
        image.setpixel(
            x0,
            y0,
            rgb);
        return;
    }

    for (int i = 0; i <= steps; ++i) {
        const float t =
            static_cast<float>(i)
            / static_cast<float>(steps);

        const int x =
            static_cast<int>(
                std::lround(
                    x0 + t * (x1 - x0)));

        const int y =
            static_cast<int>(
                std::lround(
                    y0 + t * (y1 - y0)));

        image.setpixel(
            x,
            y,
            rgb);
    }
}

void
draw_line_thick(
    ImageBuf& image,
    int x0,
    int y0,
    int x1,
    int y1,
    const float* rgb,
    int radius = 1)
{
    for (int oy = -radius;
         oy <= radius;
         ++oy) {

        for (int ox = -radius;
             ox <= radius;
             ++ox) {

            if (ox * ox + oy * oy
                > radius * radius) {
                continue;
            }

            draw_line(
                image,
                x0 + ox,
                y0 + oy,
                x1 + ox,
                y1 + oy,
                rgb);
        }
    }
}


void
draw_marker(
    ImageBuf& image,
    int cx,
    int cy,
    const float* rgb,
    int radius)
{
    radius =
        std::max(
            1,
            radius);

    for (int oy = -radius;
         oy <= radius;
         ++oy) {

        for (int ox = -radius;
             ox <= radius;
             ++ox) {

            if (ox * ox + oy * oy
                > radius * radius) {
                continue;
            }

            const int px = cx + ox;
            const int py = cy + oy;

            if (px >= 0
                && px < image.spec().width
                && py >= 0
                && py < image.spec().height) {

                image.setpixel(
                    px,
                    py,
                    rgb);
            }
        }
    }
}

const unsigned char*
glyph5x7(char c)
{
    static const unsigned char blank[7] =
        {0,0,0,0,0,0,0};

    static const unsigned char digits[10][7] = {
        {14,17,19,21,25,17,14},
        {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2},
        {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},
        {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };

    static const unsigned char letters[26][7] = {
        {14,17,17,31,17,17,17},
        {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31},
        {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},
        {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17},
        {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},
        {17,25,25,21,19,19,17},
        {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13},
        {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},
        {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10},
        {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},
        {31,1,2,4,8,16,31}
    };

    static const unsigned char dot[7] =
        {0,0,0,0,0,4,4};

    static const unsigned char dash[7] =
        {0,0,0,31,0,0,0};

    static const unsigned char slash[7] =
        {1,2,2,4,8,8,16};

    static const unsigned char colon[7] =
        {0,4,4,0,4,4,0};

    static const unsigned char plus[7] =
        {0,4,4,31,4,4,0};

    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(
            c - 'a' + 'A');
    }

    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }

    if (c >= 'A' && c <= 'Z') {
        return letters[c - 'A'];
    }

    switch (c) {
        case '.': return dot;
        case '-': return dash;
        case '/': return slash;
        case ':': return colon;
        case '+': return plus;
        default: return blank;
    }
}

void
draw_char(
    ImageBuf& image,
    int x,
    int y,
    char c,
    const float* rgb,
    int scale)
{
    const unsigned char* glyph =
        glyph5x7(c);

    for (int row = 0;
         row < 7;
         ++row) {

        for (int col = 0;
             col < 5;
             ++col) {

            if ((glyph[row]
                 & (1u << (4 - col)))
                == 0) {
                continue;
            }

            for (int sy = 0;
                 sy < scale;
                 ++sy) {

                for (int sx = 0;
                     sx < scale;
                     ++sx) {

                    const int px =
                        x + col * scale + sx;

                    const int py =
                        y + row * scale + sy;

                    if (px >= 0
                        && px < image.spec().width
                        && py >= 0
                        && py < image.spec().height) {

                        image.setpixel(
                            px,
                            py,
                            rgb);
                    }
                }
            }
        }
    }
}

int
text_width(
    const std::string& text,
    int scale)
{
    if (text.empty()) {
        return 0;
    }

    return
        static_cast<int>(text.size())
        * 6 * scale
        - scale;
}

void
draw_text(
    ImageBuf& image,
    int x,
    int y,
    const std::string& text,
    const float* rgb,
    int scale = 1)
{
    int cursor = x;

    for (char c : text) {
        draw_char(
            image,
            cursor,
            y,
            c,
            rgb,
            scale);

        cursor += 6 * scale;
    }
}

std::string
format_value(float value)
{
    char buffer[64];

    const float abs_value =
        std::abs(value);

    if (abs_value >= 1000.0f) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%.0f",
            value);
    }
    else if (abs_value >= 10.0f) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%.1f",
            value);
    }
    else {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%.2f",
            value);
    }

    return std::string(buffer);
}

std::string
output_stem(
    const std::string& filename)
{
    const std::size_t slash =
        filename.find_last_of("/\\");

    const std::size_t dot =
        filename.find_last_of('.');

    if (dot == std::string::npos
        || (slash != std::string::npos
            && dot < slash)) {

        return filename;
    }

    return filename.substr(
        0,
        dot);
}

void
expand_range(
    float& minimum,
    float& maximum,
    float padding_fraction)
{
    if (!std::isfinite(minimum)
        || !std::isfinite(maximum)) {

        minimum = 0.0f;
        maximum = 1.0f;
        return;
    }

    if (std::abs(maximum - minimum)
        < 1e-8f) {

        const float center =
            0.5f * (minimum + maximum);

        minimum = center - 0.5f;
        maximum = center + 0.5f;
        return;
    }

    const float margin =
        std::max(
            0.0f,
            padding_fraction)
        * (maximum - minimum);

    minimum -= margin;
    maximum += margin;
}

float
axis_display_value(
    float native_value,
    float scale,
    float offset)
{
    return native_value * scale + offset;
}

SampledCurve
sample_curve_for_simulation(
    const SampledCurve& source,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm)
{
    SampledCurve result;

    if (!source.valid()
        || wavelength_step_nm <= 0.0f
        || wavelength_max_nm < wavelength_min_nm) {

        return result;
    }

    for (float wavelength = wavelength_min_nm;
         wavelength <= wavelength_max_nm + 0.001f;
         wavelength += wavelength_step_nm) {

        result.x.push_back(
            wavelength);

        result.y.push_back(
            source.sample(
                wavelength,
                0.0f));
    }

    return result;
}

SampledCurve
crop_curve_to_range(
    const SampledCurve& source,
    float wavelength_min_nm,
    float wavelength_max_nm)
{
    SampledCurve result;

    if (!source.valid()) {
        return result;
    }

    for (std::size_t i = 0;
         i < source.x.size();
         ++i) {

        const float wavelength =
            source.x[i];

        if (wavelength < wavelength_min_nm
            || wavelength > wavelength_max_nm) {
            continue;
        }

        result.x.push_back(
            wavelength);

        result.y.push_back(
            source.y[i]);
    }

    return result;
}

std::string
format_wavelength_metadata(
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm,
    std::size_t sample_count)
{
    char buffer[256];

    std::snprintf(
        buffer,
        sizeof(buffer),
        "RANGE %.0F-%.0F NM  STEP %.0F NM  SAMPLES %zu  NORMALIZED PEAK 1.0",
        wavelength_min_nm,
        wavelength_max_nm,
        wavelength_step_nm,
        sample_count);

    return std::string(buffer);
}


struct ConstrainedBasisFit
{
    std::vector<float> coefficients;
    SampledCurve reconstructed;
    SampledCurve residual;
    float rms_error = 0.0f;
    float max_abs_error = 0.0f;
    float max_error_wavelength_nm = 0.0f;
    bool valid = false;
};

bool
solve_dense_system(
    std::vector<std::vector<double>> a,
    std::vector<double> b,
    std::vector<double>& x)
{
    const std::size_t n = b.size();

    if (n == 0 || a.size() != n) {
        return false;
    }

    for (std::size_t row = 0; row < n; ++row) {
        if (a[row].size() != n) {
            return false;
        }
    }

    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        double pivot_abs = std::abs(a[col][col]);

        for (std::size_t row = col + 1; row < n; ++row) {
            const double candidate = std::abs(a[row][col]);
            if (candidate > pivot_abs) {
                pivot_abs = candidate;
                pivot = row;
            }
        }

        if (pivot_abs < 1e-14) {
            return false;
        }

        if (pivot != col) {
            std::swap(a[pivot], a[col]);
            std::swap(b[pivot], b[col]);
        }

        const double diagonal = a[col][col];
        for (std::size_t j = col; j < n; ++j) {
            a[col][j] /= diagonal;
        }
        b[col] /= diagonal;

        for (std::size_t row = 0; row < n; ++row) {
            if (row == col) {
                continue;
            }

            const double factor = a[row][col];
            if (std::abs(factor) < 1e-18) {
                continue;
            }

            for (std::size_t j = col; j < n; ++j) {
                a[row][j] -= factor * a[col][j];
            }
            b[row] -= factor * b[col];
        }
    }

    x = b;
    return true;
}

ConstrainedBasisFit
fit_nonnegative_basis(
    const SampledCurve& target,
    const std::vector<const SampledCurve*>& bases,
    float wavelength_min_nm,
    float wavelength_max_nm)
{
    ConstrainedBasisFit best;

    const std::size_t basis_count = bases.size();
    if (!target.valid()
        || basis_count == 0
        || basis_count > 8) {
        return best;
    }

    for (const SampledCurve* basis : bases) {
        if (!basis || !basis->valid()) {
            return best;
        }
    }

    double best_sse = std::numeric_limits<double>::infinity();
    std::vector<double> best_coefficients(basis_count, 0.0);

    const unsigned int mask_count =
        1u << static_cast<unsigned int>(basis_count);

    for (unsigned int mask = 0; mask < mask_count; ++mask) {
        std::vector<std::size_t> active;

        for (std::size_t i = 0; i < basis_count; ++i) {
            if ((mask & (1u << static_cast<unsigned int>(i))) != 0u) {
                active.push_back(i);
            }
        }

        std::vector<double> coefficients(basis_count, 0.0);

        if (!active.empty()) {
            std::vector<std::vector<double>> normal(
                active.size(),
                std::vector<double>(active.size(), 0.0));

            std::vector<double> rhs(active.size(), 0.0);
            std::size_t sample_count = 0;

            for (std::size_t sample = 0; sample < target.x.size(); ++sample) {
                const float wavelength_nm = target.x[sample];

                if (wavelength_nm < wavelength_min_nm
                    || wavelength_nm > wavelength_max_nm) {
                    continue;
                }

                const double y = target.y[sample];

                std::vector<double> phi(active.size(), 0.0);
                for (std::size_t j = 0; j < active.size(); ++j) {
                    phi[j] = bases[active[j]]->sample(wavelength_nm, 0.0f);
                }

                for (std::size_t row = 0; row < active.size(); ++row) {
                    rhs[row] += phi[row] * y;
                    for (std::size_t col = 0; col < active.size(); ++col) {
                        normal[row][col] += phi[row] * phi[col];
                    }
                }

                ++sample_count;
            }

            if (sample_count == 0) {
                continue;
            }

            std::vector<double> solved;
            if (!solve_dense_system(normal, rhs, solved)) {
                continue;
            }

            bool nonnegative = true;
            for (std::size_t j = 0; j < active.size(); ++j) {
                if (solved[j] < -1e-10) {
                    nonnegative = false;
                    break;
                }
                coefficients[active[j]] = std::max(0.0, solved[j]);
            }

            if (!nonnegative) {
                continue;
            }
        }

        double sse = 0.0;
        std::size_t sample_count = 0;

        for (std::size_t sample = 0; sample < target.x.size(); ++sample) {
            const float wavelength_nm = target.x[sample];

            if (wavelength_nm < wavelength_min_nm
                || wavelength_nm > wavelength_max_nm) {
                continue;
            }

            double estimate = 0.0;
            for (std::size_t j = 0; j < basis_count; ++j) {
                estimate += coefficients[j]
                    * bases[j]->sample(wavelength_nm, 0.0f);
            }

            const double error = estimate - target.y[sample];
            sse += error * error;
            ++sample_count;
        }

        if (sample_count > 0 && sse < best_sse) {
            best_sse = sse;
            best_coefficients = coefficients;
        }
    }

    if (!std::isfinite(best_sse)) {
        return best;
    }

    best.coefficients.reserve(basis_count);
    for (double value : best_coefficients) {
        best.coefficients.push_back(static_cast<float>(value));
    }

    double squared_error_sum = 0.0;
    std::size_t sample_count = 0;

    for (std::size_t sample = 0; sample < target.x.size(); ++sample) {
        const float wavelength_nm = target.x[sample];

        if (wavelength_nm < wavelength_min_nm
            || wavelength_nm > wavelength_max_nm) {
            continue;
        }

        double estimate = 0.0;
        for (std::size_t j = 0; j < basis_count; ++j) {
            estimate += best_coefficients[j]
                * bases[j]->sample(wavelength_nm, 0.0f);
        }

        const float reconstructed = static_cast<float>(estimate);
        const float error = reconstructed - target.y[sample];

        best.reconstructed.x.push_back(wavelength_nm);
        best.reconstructed.y.push_back(reconstructed);
        best.residual.x.push_back(wavelength_nm);
        best.residual.y.push_back(error);

        squared_error_sum += static_cast<double>(error) * error;

        const float abs_error = std::abs(error);
        if (abs_error > best.max_abs_error) {
            best.max_abs_error = abs_error;
            best.max_error_wavelength_nm = wavelength_nm;
        }

        ++sample_count;
    }

    best.rms_error = sample_count > 0
        ? static_cast<float>(
            std::sqrt(
                squared_error_sum
                / static_cast<double>(sample_count)))
        : 0.0f;

    best.valid =
        sample_count > 0
        && best.reconstructed.valid()
        && best.residual.valid();

    return best;
}

SampledCurve
make_constant_basis(
    const SampledCurve& reference,
    float value)
{
    SampledCurve result;
    if (!reference.valid()) {
        return result;
    }

    result.x = reference.x;
    result.y.assign(reference.y.size(), value);
    return result;
}

SampledCurve
density_to_transmittance(
    const SampledCurve& density)
{
    SampledCurve result;
    if (!density.valid()) {
        return result;
    }

    result.x = density.x;
    result.y.reserve(density.y.size());

    for (float value : density.y) {
        result.y.push_back(
            std::pow(10.0f, -std::max(0.0f, value)));
    }

    return result;
}

SampledCurve
combine_density_bases(
    const SampledCurve& reference_grid,
    const SampledCurve& cyan,
    const SampledCurve& magenta,
    const SampledCurve& yellow,
    const std::array<float, 3>& coefficients)
{
    SampledCurve result;
    if (!reference_grid.valid()
        || !cyan.valid()
        || !magenta.valid()
        || !yellow.valid()) {
        return result;
    }

    result.x = reference_grid.x;
    result.y.reserve(reference_grid.x.size());

    for (float wavelength_nm : reference_grid.x) {
        const float density =
            coefficients[0] * cyan.sample(wavelength_nm, 0.0f)
            + coefficients[1] * magenta.sample(wavelength_nm, 0.0f)
            + coefficients[2] * yellow.sample(wavelength_nm, 0.0f);

        result.y.push_back(std::max(0.0f, density));
    }

    return result;
}

struct ColorimetricBasisCalibration
{
    std::array<float, 3> coefficients = {{1.0f, 1.0f, 1.0f}};
    SampledCurve reconstructed_density;
    PrintViewer::Result viewed;
    PrintViewer::Result target;
    float relative_xyz_rms = 0.0f;
    bool valid = false;
};

double
colorimetric_objective(
    const PrintViewer::Result& value,
    const PrintViewer::Result& target)
{
    const double eps = 1e-12;
    const double vx = std::max(eps, static_cast<double>(value.viewed_xyz.x));
    const double vy = std::max(eps, static_cast<double>(value.viewed_xyz.y));
    const double vz = std::max(eps, static_cast<double>(value.viewed_xyz.z));
    const double tx = std::max(eps, static_cast<double>(target.viewed_xyz.x));
    const double ty = std::max(eps, static_cast<double>(target.viewed_xyz.y));
    const double tz = std::max(eps, static_cast<double>(target.viewed_xyz.z));

    const double rx = std::log(vx / tx);
    const double ry = std::log(vy / ty);
    const double rz = std::log(vz / tz);
    return rx * rx + ry * ry + rz * rz;
}

ColorimetricBasisCalibration
calibrate_colorimetric_basis(
    const SampledCurve& visual,
    const SampledCurve& cyan,
    const SampledCurve& magenta,
    const SampledCurve& yellow,
    const PrintViewer& viewer,
    float wavelength_min_nm,
    float wavelength_max_nm,
    const std::array<float, 3>& initial_coefficients)
{
    ColorimetricBasisCalibration result;

    const SampledCurve target_density =
        crop_curve_to_range(visual, wavelength_min_nm, wavelength_max_nm);

    if (!target_density.valid()) {
        return result;
    }

    result.target =
        viewer.view(density_to_transmittance(target_density));

    std::array<double, 3> c = {{
        std::max(0.0, static_cast<double>(initial_coefficients[0])),
        std::max(0.0, static_cast<double>(initial_coefficients[1])),
        std::max(0.0, static_cast<double>(initial_coefficients[2]))
    }};

    auto evaluate =
        [&](const std::array<double, 3>& coeffs,
            SampledCurve* density_out,
            PrintViewer::Result* viewed_out) {

            const std::array<float, 3> cf = {{
                static_cast<float>(coeffs[0]),
                static_cast<float>(coeffs[1]),
                static_cast<float>(coeffs[2])
            }};

            SampledCurve density =
                combine_density_bases(
                    target_density,
                    cyan,
                    magenta,
                    yellow,
                    cf);

            const PrintViewer::Result viewed =
                viewer.view(density_to_transmittance(density));

            if (density_out) {
                *density_out = density;
            }
            if (viewed_out) {
                *viewed_out = viewed;
            }

            return colorimetric_objective(viewed, result.target);
        };

    double objective = evaluate(c, nullptr, nullptr);

    for (int iteration = 0; iteration < 24; ++iteration) {
        PrintViewer::Result current_viewed;
        evaluate(c, nullptr, &current_viewed);

        const double eps = 1e-12;
        const std::array<double, 3> residual = {{
            std::log(std::max(eps, static_cast<double>(current_viewed.viewed_xyz.x))
                     / std::max(eps, static_cast<double>(result.target.viewed_xyz.x))),
            std::log(std::max(eps, static_cast<double>(current_viewed.viewed_xyz.y))
                     / std::max(eps, static_cast<double>(result.target.viewed_xyz.y))),
            std::log(std::max(eps, static_cast<double>(current_viewed.viewed_xyz.z))
                     / std::max(eps, static_cast<double>(result.target.viewed_xyz.z)))
        }};

        std::vector<std::vector<double>> jacobian(
            3,
            std::vector<double>(3, 0.0));

        for (int col = 0; col < 3; ++col) {
            std::array<double, 3> perturbed = c;
            const double step = std::max(1e-4, 1e-3 * std::max(1.0, c[col]));
            perturbed[col] += step;

            PrintViewer::Result pv;
            evaluate(perturbed, nullptr, &pv);

            const std::array<double, 3> pr = {{
                std::log(std::max(eps, static_cast<double>(pv.viewed_xyz.x))
                         / std::max(eps, static_cast<double>(result.target.viewed_xyz.x))),
                std::log(std::max(eps, static_cast<double>(pv.viewed_xyz.y))
                         / std::max(eps, static_cast<double>(result.target.viewed_xyz.y))),
                std::log(std::max(eps, static_cast<double>(pv.viewed_xyz.z))
                         / std::max(eps, static_cast<double>(result.target.viewed_xyz.z)))
            }};

            for (int row = 0; row < 3; ++row) {
                jacobian[row][col] = (pr[row] - residual[row]) / step;
            }
        }

        std::vector<double> rhs = {{
            -residual[0],
            -residual[1],
            -residual[2]
        }};
        std::vector<double> delta;

        if (!solve_dense_system(jacobian, rhs, delta) || delta.size() != 3) {
            break;
        }

        bool accepted = false;
        double alpha = 1.0;

        for (int line_search = 0; line_search < 16; ++line_search) {
            std::array<double, 3> candidate = c;
            bool nonnegative = true;

            for (int i = 0; i < 3; ++i) {
                candidate[i] += alpha * delta[i];
                if (candidate[i] < 0.0) {
                    nonnegative = false;
                    break;
                }
            }

            if (nonnegative) {
                const double candidate_objective =
                    evaluate(candidate, nullptr, nullptr);

                if (candidate_objective < objective) {
                    c = candidate;
                    objective = candidate_objective;
                    accepted = true;
                    break;
                }
            }

            alpha *= 0.5;
        }

        if (!accepted || objective < 1e-16) {
            break;
        }
    }

    result.coefficients = {{
        static_cast<float>(c[0]),
        static_cast<float>(c[1]),
        static_cast<float>(c[2])
    }};

    evaluate(c, &result.reconstructed_density, &result.viewed);

    const double tx = std::max(1e-12, static_cast<double>(result.target.viewed_xyz.x));
    const double ty = std::max(1e-12, static_cast<double>(result.target.viewed_xyz.y));
    const double tz = std::max(1e-12, static_cast<double>(result.target.viewed_xyz.z));

    const double ex = (result.viewed.viewed_xyz.x - result.target.viewed_xyz.x) / tx;
    const double ey = (result.viewed.viewed_xyz.y - result.target.viewed_xyz.y) / ty;
    const double ez = (result.viewed.viewed_xyz.z - result.target.viewed_xyz.z) / tz;

    result.relative_xyz_rms =
        static_cast<float>(std::sqrt((ex * ex + ey * ey + ez * ez) / 3.0));

    result.valid = result.reconstructed_density.valid()
        && std::isfinite(result.relative_xyz_rms);

    return result;
}

} // namespace

bool
DiagramWriter::write_plot(
    const std::string& filename,
    const std::vector<Series>& series,
    const PlotOptions& options)
{
    float xmin =
        std::numeric_limits<float>::max();

    float xmax =
        -std::numeric_limits<float>::max();

    float ymin =
        std::numeric_limits<float>::max();

    float ymax =
        -std::numeric_limits<float>::max();

    bool have_data = false;

    for (const auto& s : series) {
        if (s.curve == nullptr
            || !s.curve->valid()) {
            continue;
        }

        for (float x : s.curve->x) {
            xmin = std::min(xmin, x);
            xmax = std::max(xmax, x);
        }

        for (float y : s.curve->y) {
            ymin = std::min(ymin, y);
            ymax = std::max(ymax, y);
        }

        have_data = true;
    }

    if (!have_data) {
        std::cerr
            << "warning: diagram has no data: "
            << filename
            << std::endl;
        return false;
    }

    if (options.has_x_range) {
        xmin = options.x_min;
        xmax = options.x_max;
    }

    if (options.has_y_range) {
        ymin = options.y_min;
        ymax = options.y_max;
    }

    if (options.log_x
        && (xmin <= 0.0f || xmax <= 0.0f)) {

        std::cerr
            << "warning: logarithmic x-axis requires positive range: "
            << filename
            << std::endl;
        return false;
    }

    if (options.log_y
        && (ymin <= 0.0f || ymax <= 0.0f)) {

        std::cerr
            << "warning: logarithmic y-axis requires positive range: "
            << filename
            << std::endl;
        return false;
    }

    if (options.include_zero_x
        && !options.log_x) {
        xmin = std::min(xmin, 0.0f);
        xmax = std::max(xmax, 0.0f);
    }

    if (options.include_zero_y
        && !options.log_y) {
        ymin = std::min(ymin, 0.0f);
        ymax = std::max(ymax, 0.0f);
    }

    if (!options.has_x_range) {
        expand_range(
            xmin,
            xmax,
            options.x_padding_fraction);
    }

    if (!options.has_y_range) {
        expand_range(
            ymin,
            ymax,
            options.y_padding_fraction);
    }

    ImageSpec spec(
        options.width,
        options.height,
        3,
        TypeDesc::FLOAT);

    ImageBuf image(spec);

    const float background[3] =
        {0.02f, 0.02f, 0.02f};

    ImageBufAlgo::fill(
        image,
        background);

    PlotMapper map;
    map.xmin = xmin;
    map.xmax = xmax;
    map.ymin = ymin;
    map.ymax = ymax;
    map.width = options.width;
    map.height = options.height;
    map.log_x = options.log_x;
    map.log_y = options.log_y;

    const float grid_color[3] =
        {0.12f, 0.12f, 0.12f};

    const float axis_color[3] =
        {0.65f, 0.65f, 0.65f};

    const float text_color[3] =
        {0.85f, 0.85f, 0.85f};

    const int plot_left =
        map.margin_left;

    const int plot_right =
        map.width
        - map.margin_right;

    const int plot_top =
        map.margin_top;

    const int plot_bottom =
        map.height
        - map.margin_bottom;

    // Bottom x-axis / vertical grid.
    const int x_divisions =
        std::max(
            1,
            options.x_divisions);

    for (int i = 0;
         i <= x_divisions;
         ++i) {

        const float tx =
            static_cast<float>(i)
            / static_cast<float>(
                x_divisions);

        const float x =
            options.log_x
                ? std::pow(
                    10.0f,
                    std::log10(xmin)
                    + tx
                        * (std::log10(xmax)
                           - std::log10(xmin)))
                : xmin
                    + tx * (xmax - xmin);

        int px0, py0;
        int px1, py1;

        map.to_pixel(
            x,
            ymin,
            px0,
            py0);

        map.to_pixel(
            x,
            ymax,
            px1,
            py1);

        draw_line(
            image,
            px0,
            py0,
            px1,
            py1,
            grid_color);

        const float bottom_value =
            axis_display_value(
                x,
                options.bottom_x_scale,
                options.bottom_x_offset);

        const std::string label =
            format_value(
                bottom_value);

        draw_text(
            image,
            px0
                - text_width(label, 1) / 2,
            plot_bottom + 14,
            label,
            text_color,
            1);
    }

    // Left y-axis / horizontal grid.
    const int y_divisions =
        std::max(
            1,
            options.y_divisions);

    for (int i = 0;
         i <= y_divisions;
         ++i) {

        const float ty =
            static_cast<float>(i)
            / static_cast<float>(
                y_divisions);

        const float y =
            options.log_y
                ? std::pow(
                    10.0f,
                    std::log10(ymin)
                    + ty
                        * (std::log10(ymax)
                           - std::log10(ymin)))
                : ymin
                    + ty * (ymax - ymin);

        int px0, py0;
        int px1, py1;

        map.to_pixel(
            xmin,
            y,
            px0,
            py0);

        map.to_pixel(
            xmax,
            y,
            px1,
            py1);

        draw_line(
            image,
            px0,
            py0,
            px1,
            py1,
            grid_color);

        const std::string label =
            format_value(y);

        draw_text(
            image,
            8,
            py0 - 3,
            label,
            text_color,
            1);
    }

    // Plot border.
    draw_line_thick(
        image,
        plot_left,
        plot_top,
        plot_right,
        plot_top,
        axis_color);

    draw_line_thick(
        image,
        plot_right,
        plot_top,
        plot_right,
        plot_bottom,
        axis_color);

    draw_line_thick(
        image,
        plot_right,
        plot_bottom,
        plot_left,
        plot_bottom,
        axis_color);

    draw_line_thick(
        image,
        plot_left,
        plot_bottom,
        plot_left,
        plot_top,
        axis_color);

    // Optional secondary top x-axis.
    //
    // Its tick positions are mapped from the same native x coordinate as the
    // curves, but the displayed values may use a different scale/offset.
    if (options.show_top_x_axis) {
        const int top_divisions =
            std::max(
                1,
                options.top_x_divisions);

        for (int i = 0;
             i <= top_divisions;
             ++i) {

            const float t =
                static_cast<float>(i)
                / static_cast<float>(
                    top_divisions);

            const float native_x =
                xmin
                + t * (xmax - xmin);

            int px, py;

            map.to_pixel(
                native_x,
                ymax,
                px,
                py);

            // Small tick extending upward from the top border.
            draw_line(
                image,
                px,
                plot_top,
                px,
                plot_top - 7,
                axis_color);

            const float top_value =
                axis_display_value(
                    native_x,
                    options.top_x_scale,
                    options.top_x_offset);

            const std::string label =
                format_value(
                    top_value);

            draw_text(
                image,
                px
                    - text_width(
                        label,
                        1)
                    / 2,
                plot_top - 22,
                label,
                text_color,
                1);
        }
    }

    // Curves.
    for (const auto& s : series) {
        if (s.curve == nullptr
            || !s.curve->valid()) {
            continue;
        }

        const float color[3] = {
            s.color[0],
            s.color[1],
            s.color[2]
        };

        if (s.show_line) {
            for (std::size_t i = 1;
                 i < s.curve->x.size();
                 ++i) {

                int x0, y0;
                int x1, y1;

                map.to_pixel(
                    s.curve->x[i - 1],
                    s.curve->y[i - 1],
                    x0,
                    y0);

                map.to_pixel(
                    s.curve->x[i],
                    s.curve->y[i],
                    x1,
                    y1);

                draw_line_thick(
                    image,
                    x0,
                    y0,
                    x1,
                    y1,
                    color,
                    1);
            }
        }

        // Overlay the original loaded CSV samples.
        if (s.show_markers) {
            for (std::size_t i = 0;
                 i < s.curve->x.size();
                 ++i) {

                int px, py;

                map.to_pixel(
                    s.curve->x[i],
                    s.curve->y[i],
                    px,
                    py);

                draw_marker(
                    image,
                    px,
                    py,
                    color,
                    s.marker_radius);
            }
        }
    }

    // Main title.
    const int title_scale = 2;

    draw_text(
        image,
        std::max(
            8,
            (options.width
             - text_width(
                 options.title,
                 title_scale))
             / 2),
        14,
        options.title,
        text_color,
        title_scale);

    if (!options.subtitle.empty()) {
        draw_text(
            image,
            std::max(
                8,
                (options.width
                 - text_width(
                     options.subtitle,
                     1))
                 / 2),
            42,
            options.subtitle,
            text_color,
            1);
    }

    // Optional title for the top x-axis.
    if (options.show_top_x_axis
        && !options.top_x_label.empty()) {

        draw_text(
            image,
            std::max(
                8,
                (options.width
                 - text_width(
                     options.top_x_label,
                     1))
                 / 2),
            60,
            options.top_x_label,
            text_color,
            1);
    }

    // Bottom x-axis title.
    draw_text(
        image,
        std::max(
            8,
            (options.width
             - text_width(
                 options.x_label,
                 1))
             / 2),
        options.height - 28,
        options.x_label,
        text_color,
        1);

    // Left y-axis title.
    //
    // The current bitmap font is horizontal, so this remains placed at the
    // upper-left rather than rotated. We can add rotated glyph rendering later
    // if we want to match Kodak datasheets exactly.
    draw_text(
        image,
        8,
        48,
        options.y_label,
        text_color,
        1);

    // Legend.
    int legend_y = 40;

    for (const auto& s : series) {
        if (s.curve == nullptr
            || !s.curve->valid()) {
            continue;
        }

        const float color[3] = {
            s.color[0],
            s.color[1],
            s.color[2]
        };

        const int x0 =
            options.width - 260;

        if (s.show_line) {
            draw_line_thick(
                image,
                x0,
                legend_y + 3,
                x0 + 28,
                legend_y + 3,
                color,
                1);
        }
        else {
            draw_marker(
                image,
                x0 + 14,
                legend_y + 3,
                color,
                std::max(
                    1,
                    s.marker_radius));
        }

        draw_text(
            image,
            x0 + 38,
            legend_y,
            s.name,
            text_color,
            1);

        legend_y += 16;
    }

    return image.write(
        filename);
}

bool
DiagramWriter::write_film_diagnostics(
    const std::string& output_image_filename,
    const FilmStock& stock,
    const SampledCurve& illuminant,
    const FilmProcessor::Settings& processor_settings,
    const FilmExposureBalance& balance)
{
    const std::string stem =
        output_stem(
            output_image_filename);

    bool success = true;

    // Spectral sensitivity.
    {
        PlotOptions options;
        options.title =
            stock.name()
            + " SPECTRAL SENSITIVITY";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "LOG SENSITIVITY";

        const auto& s =
            stock.sensitivity();

        const std::vector<Series> series = {
            {
                "RED SENSITIVE CYAN FORMING",
                &s.red_sensitive_log,
                {{1.0f, 0.25f, 0.25f}},
                true,
                true,
                2
            },
            {
                "GREEN SENSITIVE MAGENTA FORMING",
                &s.green_sensitive_log,
                {{0.25f, 1.0f, 0.25f}},
                true,
                true,
                2
            },
            {
                "BLUE SENSITIVE YELLOW FORMING",
                &s.blue_sensitive_log,
                {{0.25f, 0.55f, 1.0f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_spectral_sensitivity.png",
                series,
                options)
            && success;
    }

    // Characteristic curves.
    {
        PlotOptions options;

        options.title =
            stock.name()
            + " SENSITOMETRIC CURVES";

        // The CSV's native x coordinate is log10 exposure.
        //
        // Top axis:
        //     native log exposure in lux-seconds.
        //
        // Bottom axis:
        //     camera stops, using the calibration embodied by the source
        //     Verita diagram:
        //
        //       -2.915 logE -> -8 stops
        //       -0.515 logE ->  0 stops
        //        1.885 logE -> +8 stops
        //
        // which is exactly 0.300 log10 units per stop in the digitized source.
        options.top_x_label =
            "LOG EXPOSURE LUX-SECONDS";

        options.x_label =
            "CAMERA STOPS";

        options.y_label =
            "DENSITY";

        options.show_top_x_axis = true;

        options.top_x_scale = 1.0f;
        options.top_x_offset = 0.0f;

        options.bottom_x_scale =
            1.0f / 0.300f;

        options.bottom_x_offset =
            0.515f / 0.300f;

        // One bottom division per camera stop across -8 .. +8.
        options.x_divisions = 16;

        // Kodak-style top labels: left / middle / right.
        options.top_x_divisions = 2;

        // Preserve the physical source endpoints rather than padding them.
        options.x_padding_fraction = 0.0f;

        const auto& c =
            stock.characteristic();

        const std::vector<Series> series = {
            {
                "RED RECORD",
                &c.red_density,
                {{1.0f, 0.25f, 0.25f}},
                true,
                true,
                2
            },
            {
                "GREEN RECORD",
                &c.green_density,
                {{0.25f, 1.0f, 0.25f}},
                true,
                true,
                2
            },
            {
                "BLUE RECORD",
                &c.blue_density,
                {{0.25f, 0.55f, 1.0f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_characteristic.png",
                series,
                options)
            && success;
    }

    // Illuminant.
    //
    // The yellow line is the exact wavelength grid used by FilmProcessor.
    // The source CSV samples inside that active range are overlaid as markers.
    {
        const SampledCurve simulation_illuminant =
            sample_curve_for_simulation(
                illuminant,
                processor_settings.wavelength_min_nm,
                processor_settings.wavelength_max_nm,
                processor_settings.wavelength_step_nm);

        const SampledCurve source_illuminant =
            crop_curve_to_range(
                illuminant,
                processor_settings.wavelength_min_nm,
                processor_settings.wavelength_max_nm);

        PlotOptions options;

        options.title =
            "VIRTUAL SCENE ILLUMINANT";

        options.subtitle =
            format_wavelength_metadata(
                processor_settings.wavelength_min_nm,
                processor_settings.wavelength_max_nm,
                processor_settings.wavelength_step_nm,
                simulation_illuminant.x.size());

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "NORMALIZED POWER";

        options.include_zero_y = true;

        // The active wavelength endpoints are physically meaningful.
        options.x_padding_fraction = 0.0f;

        // 20 nm divisions across 380-700 gives 16 intervals.
        const float active_range_nm =
            processor_settings.wavelength_max_nm
            - processor_settings.wavelength_min_nm;

        options.x_divisions =
            std::max(
                1,
                static_cast<int>(
                    std::lround(
                        active_range_nm / 20.0f)));

        const std::vector<Series> series = {
            {
                "SIMULATION SPD",
                &simulation_illuminant,
                {{1.0f, 0.9f, 0.45f}},
                true,
                false,
                2
            },
            {
                "SOURCE SAMPLES",
                &source_illuminant,
                {{0.75f, 0.75f, 0.75f}},
                false,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_illuminant.png",
                series,
                options)
            && success;
    }

    // Film balance is a tiny 3-point curve. This is intentionally plotted
    // as a diagnostic rather than pretending the channels form a spectrum.
    {
        SampledCurve offsets;
        offsets.x = {0.0f, 1.0f, 2.0f};

        const float log10_2 =
            std::log10(2.0f);

        offsets.y = {
            balance.red_log_offset / log10_2,
            balance.green_log_offset / log10_2,
            balance.blue_log_offset / log10_2
        };

        PlotOptions options;
        options.title =
            stock.name()
            + " FILM BALANCE R G B";
        options.x_label =
            "CHANNEL INDEX 0 R 1 G 2 B";
        options.y_label =
            "OFFSET STOPS";
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {
                "FILM BALANCE",
                &offsets,
                {{0.9f, 0.9f, 0.9f}},
                true,
                false,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_film_balance.png",
                series,
                options)
            && success;
    }

    return success;
}

bool
DiagramWriter::write_dye_model_diagnostics(
    const std::string& output_image_filename,
    const FilmStock& stock,
    const FilmDyeModel& dye_model)
{
    if (!dye_model.valid()) {
        return false;
    }

    const std::string stem =
        output_stem(
            output_image_filename);

    bool success = true;

    // ------------------------------------------------------------------
    // 1. Measured reference density data.
    // ------------------------------------------------------------------
    {
        PlotOptions options;

        options.title =
            stock.name()
            + " SPECTRAL DYE DENSITY REFERENCES";

        options.subtitle =
            "MEASURED KODAK DATA  MINIMUM / MIDSCALE NEUTRAL / DIFFERENCE";

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "DIFFUSE SPECTRAL DENSITY";

        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {
                "MINIMUM DENSITY",
                &dye_model.minimum_density(),
                {{0.65f, 0.65f, 0.65f}},
                true,
                true,
                2
            },
            {
                "MIDSCALE NEUTRAL",
                &dye_model.midscale_neutral_density(),
                {{1.0f, 1.0f, 1.0f}},
                true,
                true,
                2
            },
            {
                "NEUTRAL MINUS MINIMUM",
                &dye_model.neutral_increment(),
                {{1.0f, 0.85f, 0.25f}},
                true,
                false,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_dye_density_reference.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 2. Experimental C/M/Y contribution estimate.
    //
    // These are NOT claimed to be isolated measured dye spectra. They are a
    // partition of the measured neutral density increment using only the
    // stock's own measured spectral-sensitivity curves as wavelength weights.
    // ------------------------------------------------------------------
    {
        PlotOptions options;

        options.title =
            stock.name()
            + " EXPERIMENTAL DYE CONTRIBUTION ESTIMATE";

        options.subtitle =
            "DERIVED ONLY FROM KODAK DENSITY + SENSITIVITY DATA";

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "DENSITY CONTRIBUTION";

        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {
                "CYAN ESTIMATE",
                &dye_model.cyan_contribution(),
                {{0.25f, 0.9f, 1.0f}},
                true,
                false,
                2
            },
            {
                "MAGENTA ESTIMATE",
                &dye_model.magenta_contribution(),
                {{1.0f, 0.35f, 0.85f}},
                true,
                false,
                2
            },
            {
                "YELLOW ESTIMATE",
                &dye_model.yellow_contribution(),
                {{1.0f, 0.9f, 0.25f}},
                true,
                false,
                2
            },
            {
                "MEASURED NEUTRAL INCREMENT",
                &dye_model.neutral_increment(),
                {{0.9f, 0.9f, 0.9f}},
                true,
                false,
                2
            },
            {
                "RECONSTRUCTED INCREMENT",
                &dye_model.reconstructed_increment(),
                {{0.45f, 1.0f, 0.45f}},
                true,
                false,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_dye_basis_estimate.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 3. Reconstruction residual.
    //
    // The residual should be numerically near zero wherever at least one
    // measured sensitivity record exists, because the contributions are
    // normalized to partition the measured increment. This validates the
    // implementation, not the uniqueness of the physical dye solution.
    // ------------------------------------------------------------------
    {
        PlotOptions options;

        options.title =
            stock.name()
            + " DYE MODEL RECONSTRUCTION RESIDUAL";

        options.subtitle =
            "RECONSTRUCTED MIDSCALE MINUS MEASURED MIDSCALE";

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "DENSITY ERROR";

        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {
                "RESIDUAL",
                &dye_model.residual(),
                {{1.0f, 0.5f, 0.25f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_dye_basis_residual.png",
                series,
                options)
            && success;
    }


    // ------------------------------------------------------------------
    // 4. Calibrated per-record spectral-density basis.
    //
    // These curves are the neutral-state C/M/Y contributions divided by the
    // developed record-density increments at the Kodak 0-stop calibration
    // point (logE = -0.515). They are therefore the bridge used to synthesize
    // an approximate wavelength-dependent negative density from arbitrary
    // developed R/G/B record densities.
    // ------------------------------------------------------------------
    {
        PlotOptions options;

        options.title =
            stock.name()
            + " CALIBRATED SPECTRAL DYE BASIS";

        options.subtitle =
            "DENSITY CONTRIBUTION PER UNIT DEVELOPED RECORD DENSITY";

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "SPECTRAL DENSITY PER RECORD DENSITY";

        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {
                "CYAN BASIS PER RED RECORD",
                &dye_model.cyan_basis_per_record_density(),
                {{0.25f, 0.9f, 1.0f}},
                true,
                false,
                2
            },
            {
                "MAGENTA BASIS PER GREEN RECORD",
                &dye_model.magenta_basis_per_record_density(),
                {{1.0f, 0.35f, 0.85f}},
                true,
                false,
                2
            },
            {
                "YELLOW BASIS PER BLUE RECORD",
                &dye_model.yellow_basis_per_record_density(),
                {{1.0f, 0.9f, 0.25f}},
                true,
                false,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_dye_basis_calibrated.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 5. Calibration residual.
    //
    // This validates that the per-record basis reproduces the captured Kodak
    // midscale-neutral spectrum at the selected sensitometric reference.
    // ------------------------------------------------------------------
    {
        PlotOptions options;

        options.title =
            stock.name()
            + " DYE BASIS CALIBRATION RESIDUAL";

        options.subtitle =
            "CALIBRATED SPECTRAL NEGATIVE MINUS MEASURED MIDSCALE NEUTRAL";

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "DENSITY ERROR";

        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {
                "CALIBRATION RESIDUAL",
                &dye_model.calibration_residual(),
                {{1.0f, 0.55f, 0.25f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_dye_basis_calibration_residual.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 6. Neutral spectral-density ladder.
    //
    // Use the source sensitometric mapping of 0.300 log10 exposure units per
    // camera stop and synthesize the negative spectrum at five neutral
    // exposures around the measured midscale-neutral calibration point.
    // ------------------------------------------------------------------
    {
        const float reference_log_exposure =
            dye_model.diagnostics()
                .calibration_log_exposure;

        const float log_exposure_per_stop =
            0.300f;

        const SampledCurve density_m4 =
            dye_model.synthesize_neutral_density(
                stock,
                reference_log_exposure
                    - 4.0f * log_exposure_per_stop);

        const SampledCurve density_m2 =
            dye_model.synthesize_neutral_density(
                stock,
                reference_log_exposure
                    - 2.0f * log_exposure_per_stop);

        const SampledCurve density_0 =
            dye_model.synthesize_neutral_density(
                stock,
                reference_log_exposure);

        const SampledCurve density_p2 =
            dye_model.synthesize_neutral_density(
                stock,
                reference_log_exposure
                    + 2.0f * log_exposure_per_stop);

        const SampledCurve density_p4 =
            dye_model.synthesize_neutral_density(
                stock,
                reference_log_exposure
                    + 4.0f * log_exposure_per_stop);

        PlotOptions options;

        options.title =
            stock.name()
            + " SYNTHESIZED NEUTRAL SPECTRAL DENSITY";

        options.subtitle =
            "EXPERIMENTAL NEGATIVE DENSITY LADDER  -4 -2 0 +2 +4 STOPS";

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "DIFFUSE SPECTRAL DENSITY";

        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"-4 STOPS", &density_m4, {{0.45f,0.45f,0.45f}}, true, false, 2},
            {"-2 STOPS", &density_m2, {{0.65f,0.65f,0.65f}}, true, false, 2},
            {"0 STOPS",  &density_0,  {{1.00f,1.00f,1.00f}}, true, false, 2},
            {"+2 STOPS", &density_p2, {{0.80f,0.85f,1.00f}}, true, false, 2},
            {"+4 STOPS", &density_p4, {{0.55f,0.65f,1.00f}}, true, false, 2}
        };

        success =
            write_plot(
                stem
                    + "_negative_spectral_density_ladder.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 7. Neutral transmittance ladder.
    //
    // This is the quantity the printer/scanner stage will actually consume:
    //
    //     T(lambda) = 10^(-D(lambda))
    // ------------------------------------------------------------------
    {
        const float reference_log_exposure =
            dye_model.diagnostics()
                .calibration_log_exposure;

        const float log_exposure_per_stop =
            0.300f;

        const SampledCurve transmission_m4 =
            dye_model.synthesize_neutral_transmittance(
                stock,
                reference_log_exposure
                    - 4.0f * log_exposure_per_stop);

        const SampledCurve transmission_0 =
            dye_model.synthesize_neutral_transmittance(
                stock,
                reference_log_exposure);

        const SampledCurve transmission_p4 =
            dye_model.synthesize_neutral_transmittance(
                stock,
                reference_log_exposure
                    + 4.0f * log_exposure_per_stop);

        PlotOptions options;

        options.title =
            stock.name()
            + " SYNTHESIZED NEGATIVE TRANSMITTANCE";

        options.subtitle =
            "T LAMBDA = 10^-D LAMBDA   NEUTRAL -4 / 0 / +4 STOPS";

        options.x_label =
            "WAVELENGTH NM";

        options.y_label =
            "TRANSMITTANCE";

        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"-4 STOPS", &transmission_m4, {{0.65f,0.65f,0.65f}}, true, false, 2},
            {"0 STOPS",  &transmission_0,  {{1.00f,1.00f,1.00f}}, true, false, 2},
            {"+4 STOPS", &transmission_p4, {{0.55f,0.70f,1.00f}}, true, false, 2}
        };

        success =
            write_plot(
                stem
                    + "_negative_transmittance_ladder.png",
                series,
                options)
            && success;
    }

    return success;
}


bool
DiagramWriter::write_print_stock_source_diagnostics(
    const std::string& output_image_filename,
    const PrintFilmStock& stock)
{
    if (!stock.valid()) {
        return false;
    }

    const std::string stem =
        output_stem(
            output_image_filename);

    bool success = true;

    // ------------------------------------------------------------------
    // 1. Kodak 2383 sensitometric curves.
    // Raw digitized source: log exposure -> Status A density.
    // ------------------------------------------------------------------
    {
        const auto& c =
            stock.characteristic();

        PlotOptions options;
        options.title =
            stock.name()
            + " SOURCE SENSITOMETRIC CURVES";
        options.subtitle =
            "DIGITIZED KODAK SOURCE DATA  STATUS A";
        options.x_label =
            "LOG EXPOSURE LUX-SECONDS";
        options.y_label =
            "DENSITY";

        options.has_x_range = true;
        options.x_min = -1.0f;
        options.x_max = 2.7f;

        options.has_y_range = true;
        options.y_min = 0.0f;
        options.y_max = 4.2f;

        options.x_divisions = 8;
        options.y_divisions = 8;

        const std::vector<Series> series = {
            {
                "RED RECORD",
                &c.red_density,
                {{1.0f, 0.25f, 0.25f}},
                true,
                true,
                2
            },
            {
                "GREEN RECORD",
                &c.green_density,
                {{0.25f, 1.0f, 0.25f}},
                true,
                true,
                2
            },
            {
                "BLUE RECORD",
                &c.blue_density,
                {{0.25f, 0.55f, 1.0f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_source_sensitometric.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 2. Kodak 2383 spectral sensitivity.
    //
    // The two lower-left traces are intentionally shown as unresolved
    // auxiliary source traces rather than silently assigning them to a layer.
    // ------------------------------------------------------------------
    {
        const auto& s =
            stock.sensitivity();

        PlotOptions options;
        options.title =
            stock.name()
            + " SOURCE SPECTRAL SENSITIVITY";
        options.subtitle =
            "DIGITIZED KODAK SOURCE DATA  AUXILIARY TRACES PRESERVED";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "LOG SENSITIVITY";

        options.has_x_range = true;
        options.x_min = 250.0f;
        options.x_max = 750.0f;

        options.has_y_range = true;
        options.y_min = -3.0f;
        options.y_max = 1.0f;

        options.x_divisions = 10;
        options.y_divisions = 8;

        const std::vector<Series> series = {
            {
                "YELLOW FORMING",
                &s.yellow_forming_log,
                {{1.0f, 0.9f, 0.25f}},
                true,
                true,
                2
            },
            {
                "MAGENTA FORMING",
                &s.magenta_forming_log,
                {{1.0f, 0.35f, 0.85f}},
                true,
                true,
                2
            },
            {
                "CYAN FORMING",
                &s.cyan_forming_log,
                {{0.25f, 0.9f, 1.0f}},
                true,
                true,
                2
            },
            {
                "AUXILIARY TRACE A",
                &s.auxiliary_trace_a_log,
                {{0.75f, 0.75f, 0.75f}},
                true,
                true,
                2
            },
            {
                "AUXILIARY TRACE B",
                &s.auxiliary_trace_b_log,
                {{0.55f, 0.55f, 0.55f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_source_spectral_sensitivity.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 3. Kodak 2383 spectral dye-density curves.
    // Unlike the negative source, these are independently digitized C/M/Y
    // print-dye curves plus Kodak's visual-neutral reference.
    // ------------------------------------------------------------------
    {
        const auto& d =
            stock.dye_density();

        PlotOptions options;
        options.title =
            stock.name()
            + " SOURCE SPECTRAL DYE DENSITY";
        options.subtitle =
            "DIGITIZED KODAK SOURCE DATA  C M Y + VISUAL NEUTRAL";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "DIFFUSE SPECTRAL DENSITY";

        options.has_x_range = true;
        options.x_min = 250.0f;
        options.x_max = 850.0f;

        options.has_y_range = true;
        options.y_min = 0.0f;
        options.y_max = 1.4f;

        options.x_divisions = 12;
        options.y_divisions = 7;

        const std::vector<Series> series = {
            {
                "VISUAL NEUTRAL",
                &d.visual_neutral_density,
                {{1.0f, 1.0f, 1.0f}},
                true,
                true,
                2
            },
            {
                "CYAN",
                &d.cyan_density,
                {{0.25f, 0.9f, 1.0f}},
                true,
                true,
                2
            },
            {
                "MAGENTA",
                &d.magenta_density,
                {{1.0f, 0.35f, 0.85f}},
                true,
                true,
                2
            },
            {
                "YELLOW",
                &d.yellow_density,
                {{1.0f, 0.9f, 0.25f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_source_spectral_dye_density.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 4. Kodak 2383 MTF.
    // The Kodak source graph uses logarithmic frequency and response axes.
    // ------------------------------------------------------------------
    {
        const auto& m =
            stock.mtf();

        PlotOptions options;
        options.title =
            stock.name()
            + " SOURCE MODULATION TRANSFER FUNCTION";
        options.subtitle =
            "DIGITIZED KODAK SOURCE DATA  STATUS A  35 PERCENT TARGET";
        options.x_label =
            "SPATIAL FREQUENCY CYCLES/MM";
        options.y_label =
            "RESPONSE PERCENT";

        options.has_x_range = true;
        options.x_min = 1.0f;
        options.x_max = 600.0f;

        options.has_y_range = true;
        options.y_min = 1.0f;
        options.y_max = 200.0f;

        options.log_x = true;
        options.log_y = true;

        options.x_divisions = 9;
        options.y_divisions = 8;

        const std::vector<Series> series = {
            {
                "RED",
                &m.red_response_percent,
                {{1.0f, 0.25f, 0.25f}},
                true,
                true,
                2
            },
            {
                "GREEN",
                &m.green_response_percent,
                {{0.25f, 1.0f, 0.25f}},
                true,
                true,
                2
            },
            {
                "BLUE",
                &m.blue_response_percent,
                {{0.25f, 0.55f, 1.0f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_source_mtf.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 5a. Density traces from the Kodak diffuse-RMS-granularity graph.
    // These use the graph's left linear Density axis.
    // ------------------------------------------------------------------
    {
        const auto& g =
            stock.granularity();

        PlotOptions options;
        options.title =
            stock.name()
            + " SOURCE GRANULARITY DENSITY TRACES";
        options.subtitle =
            "LEFT AXIS OF THE KODAK DIFFUSE RMS GRANULARITY GRAPH";
        options.x_label =
            "LOG EXPOSURE LUX-SECONDS";
        options.y_label =
            "DENSITY";

        options.has_x_range = true;
        options.x_min = 0.0f;
        options.x_max = 3.0f;

        options.has_y_range = true;
        options.y_min = 0.0f;
        options.y_max = 4.0f;

        options.x_divisions = 6;
        options.y_divisions = 8;

        const std::vector<Series> series = {
            {
                "RED DENSITY",
                &g.red_density,
                {{1.0f, 0.25f, 0.25f}},
                true,
                true,
                2
            },
            {
                "GREEN DENSITY",
                &g.green_density,
                {{0.25f, 1.0f, 0.25f}},
                true,
                true,
                2
            },
            {
                "BLUE DENSITY",
                &g.blue_density,
                {{0.25f, 0.55f, 1.0f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_source_granularity_density.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // 5b. RMS granularity traces from the same Kodak graph.
    // Kodak's right-hand RMS scale is logarithmic.
    // Keeping this separate avoids mixing two y-axis coordinate systems in
    // the generic plotter while still validating every captured source value.
    // ------------------------------------------------------------------
    {
        const auto& g =
            stock.granularity();

        PlotOptions options;
        options.title =
            stock.name()
            + " SOURCE DIFFUSE RMS GRANULARITY";
        options.subtitle =
            "RIGHT LOGARITHMIC AXIS OF THE KODAK GRANULARITY GRAPH";
        options.x_label =
            "LOG EXPOSURE LUX-SECONDS";
        options.y_label =
            "DIFFUSE RMS GRANULARITY";

        options.has_x_range = true;
        options.x_min = 0.0f;
        options.x_max = 3.0f;

        options.has_y_range = true;
        options.y_min = 0.001f;
        options.y_max = 0.100f;

        options.log_y = true;

        options.x_divisions = 6;
        options.y_divisions = 8;

        const std::vector<Series> series = {
            {
                "RED RMS",
                &g.red_rms,
                {{1.0f, 0.25f, 0.25f}},
                true,
                true,
                2
            },
            {
                "GREEN RMS",
                &g.green_rms,
                {{0.25f, 1.0f, 0.25f}},
                true,
                true,
                2
            },
            {
                "BLUE RMS",
                &g.blue_rms,
                {{0.25f, 0.55f, 1.0f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_source_granularity_rms.png",
                series,
                options)
            && success;
    }

    return success;
}


bool
DiagramWriter::write_print_pipeline_diagnostics(
    const std::string& output_image_filename,
    const FilmStock& negative_stock,
    const FilmDyeModel& negative_dye_model,
    const PrintFilmStock& print_stock,
    const PrintFilmProcessor& print_processor,
    const PrintDyeModel& print_dye_model)
{
    if (!negative_stock.valid()
        || !negative_dye_model.valid()
        || !print_stock.valid()
        || !print_processor.valid()
        || !print_dye_model.valid()) {
        return false;
    }

    const std::string stem =
        output_stem(
            output_image_filename);

    bool success = true;

    // ------------------------------------------------------------------
    // Printer illuminant actually used by PrintFilmProcessor.
    // ------------------------------------------------------------------
    {
        const SampledCurve& printer =
            print_processor.printer_illuminant();

        PlotOptions options;
        options.title =
            "KODAK 2383 VIRTUAL PRINTER ILLUMINANT";
        options.subtitle =
            "ACTIVE SPD USED FOR NEGATIVE TO PRINT EXPOSURE";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "NORMALIZED POWER";

        options.has_x_range = true;
        options.x_min =
            print_processor.settings().wavelength_min_nm;
        options.x_max =
            print_processor.settings().wavelength_max_nm;

        options.include_zero_y = true;
        options.x_padding_fraction = 0.0f;
        options.x_divisions = 16;

        const std::vector<Series> series = {
            {
                "PRINTER SPD",
                &printer,
                {{1.0f, 0.75f, 0.35f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_printer_illuminant.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // Kodak 2383 dye-model reference reconstruction.
    // At Status-A R/G/B density 1.0 this must reproduce Kodak's published
    // visual-neutral spectral-density curve.
    // ------------------------------------------------------------------
    {
        const auto& visual =
            print_dye_model.visual_neutral_density();

        const auto& reconstructed =
            print_dye_model.reconstructed_visual_neutral();

        const auto& residual =
            print_dye_model.neutral_residual_density();

        PlotOptions options;
        options.title =
            "KODAK 2383 PRINT DYE MODEL REFERENCE";
        options.subtitle =
            "STATUS-A R G B DENSITY 1.0  MEASURED NEUTRAL VS RECONSTRUCTION";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "DIFFUSE SPECTRAL DENSITY";

        options.has_x_range = true;
        options.x_min =
            print_processor.settings().wavelength_min_nm;
        options.x_max =
            print_processor.settings().wavelength_max_nm;

        options.include_zero_y = true;
        options.x_padding_fraction = 0.0f;

        const std::vector<Series> series = {
            {
                "KODAK VISUAL NEUTRAL",
                &visual,
                {{1.0f, 1.0f, 1.0f}},
                true,
                true,
                2
            },
            {
                "RECONSTRUCTED",
                &reconstructed,
                {{0.25f, 1.0f, 0.35f}},
                true,
                false,
                2
            },
            {
                "NEUTRAL RESIDUAL",
                &residual,
                {{0.65f, 0.65f, 0.65f}},
                true,
                false,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_print_dye_reference.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // Numerical reconstruction residual.
    // ------------------------------------------------------------------
    {
        const auto& residual =
            print_dye_model.reconstruction_residual();

        PlotOptions options;
        options.title =
            "KODAK 2383 PRINT DYE REFERENCE RESIDUAL";
        options.subtitle =
            "RECONSTRUCTED VISUAL NEUTRAL MINUS DIGITIZED KODAK CURVE";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "DENSITY ERROR";
        options.include_zero_y = true;

        options.has_x_range = true;
        options.x_min =
            print_processor.settings().wavelength_min_nm;
        options.x_max =
            print_processor.settings().wavelength_max_nm;

        options.x_padding_fraction = 0.0f;

        const std::vector<Series> series = {
            {
                "RESIDUAL",
                &residual,
                {{1.0f, 0.5f, 0.15f}},
                true,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_print_dye_reference_residual.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // Full neutral-negative -> print spectral-density ladder.
    //
    // Negative exposure is stepped around the measured Verita calibration
    // state. One stop corresponds to log10(2) in the negative H-D domain.
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    const std::array<float, 5> stop_values = {{
        -4.0f,
        -2.0f,
         0.0f,
         2.0f,
         4.0f
    }};

    std::array<SampledCurve, 5> print_density_ladder;
    std::array<FilmDensity, 5> print_record_density_ladder;

    const float log10_2 =
        std::log10(
            2.0f);

    for (std::size_t i = 0;
         i < stop_values.size();
         ++i) {

        const float negative_log_exposure =
            negative_dye_model.diagnostics()
                .calibration_log_exposure
            + stop_values[i]
                * log10_2;

        const SampledCurve negative_transmittance =
            negative_dye_model.synthesize_neutral_transmittance(
                negative_stock,
                negative_log_exposure);

        print_record_density_ladder[i] =
            print_processor.process(
                negative_transmittance);

        print_density_ladder[i] =
            print_dye_model.synthesize_density(
                print_record_density_ladder[i]);
    }

    {
        PlotOptions options;
        options.title =
            "KODAK 2383 SYNTHESIZED PRINT SPECTRAL DENSITY";
        options.subtitle =
            "VERITA NEUTRAL NEGATIVE LADDER  -4 -2 0 +2 +4 STOPS";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "DIFFUSE SPECTRAL DENSITY";

        options.has_x_range = true;
        options.x_min =
            print_processor.settings().wavelength_min_nm;
        options.x_max =
            print_processor.settings().wavelength_max_nm;

        options.include_zero_y = true;
        options.x_padding_fraction = 0.0f;

        const std::vector<Series> series = {
            {
                "-4 STOPS",
                &print_density_ladder[0],
                {{0.35f, 0.35f, 0.35f}},
                true,
                false,
                2
            },
            {
                "-2 STOPS",
                &print_density_ladder[1],
                {{0.55f, 0.55f, 0.55f}},
                true,
                false,
                2
            },
            {
                "0 STOPS",
                &print_density_ladder[2],
                {{1.0f, 1.0f, 1.0f}},
                true,
                false,
                2
            },
            {
                "+2 STOPS",
                &print_density_ladder[3],
                {{0.65f, 0.75f, 1.0f}},
                true,
                false,
                2
            },
            {
                "+4 STOPS",
                &print_density_ladder[4],
                {{0.35f, 0.50f, 1.0f}},
                true,
                false,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_print_spectral_density_ladder.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // Print transmittance for the same neutral ladder.
    // ------------------------------------------------------------------
    std::array<SampledCurve, 3> print_transmittance_ladder;

    print_transmittance_ladder[0] =
        print_dye_model.synthesize_transmittance(
            print_record_density_ladder[0]);

    print_transmittance_ladder[1] =
        print_dye_model.synthesize_transmittance(
            print_record_density_ladder[2]);

    print_transmittance_ladder[2] =
        print_dye_model.synthesize_transmittance(
            print_record_density_ladder[4]);

    {
        PlotOptions options;
        options.title =
            "KODAK 2383 SYNTHESIZED PRINT TRANSMITTANCE";
        options.subtitle =
            "T LAMBDA = 10^-D LAMBDA  VERITA NEUTRAL -4 / 0 / +4 STOPS";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "TRANSMITTANCE";

        options.has_x_range = true;
        options.x_min =
            print_processor.settings().wavelength_min_nm;
        options.x_max =
            print_processor.settings().wavelength_max_nm;

        options.include_zero_y = true;
        options.x_padding_fraction = 0.0f;

        const std::vector<Series> series = {
            {
                "-4 STOPS",
                &print_transmittance_ladder[0],
                {{0.55f, 0.55f, 0.55f}},
                true,
                false,
                2
            },
            {
                "0 STOPS",
                &print_transmittance_ladder[1],
                {{1.0f, 1.0f, 1.0f}},
                true,
                false,
                2
            },
            {
                "+4 STOPS",
                &print_transmittance_ladder[2],
                {{0.35f, 0.50f, 1.0f}},
                true,
                false,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_print_transmittance_ladder.png",
                series,
                options)
            && success;
    }

    return success;
}



bool
DiagramWriter::write_print_viewer_diagnostics(
    const std::string& output_image_filename,
    const FilmStock& negative_stock,
    const FilmDyeModel& negative_dye_model,
    const PrintFilmStock& print_stock,
    const PrintFilmProcessor& print_processor,
    const PrintDyeModel& print_dye_model,
    const PrintViewer& print_viewer)
{
    if (!negative_stock.valid()
        || !negative_dye_model.valid()
        || !print_stock.valid()
        || !print_processor.valid()
        || !print_dye_model.valid()
        || !print_viewer.valid()) {
        return false;
    }

    const std::string stem =
        output_stem(
            output_image_filename);

    bool success = true;

    // ------------------------------------------------------------------
    // Actual output-side viewing illuminant.
    // ------------------------------------------------------------------
    {
        const SampledCurve sampled_viewing_illuminant =
            sample_curve_for_simulation(
                print_viewer.viewing_illuminant(),
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm);

        const SampledCurve source_viewing_illuminant =
            crop_curve_to_range(
                print_viewer.viewing_illuminant(),
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm);

        PlotOptions options;
        options.title =
            "KODAK 2383 VIEWING ILLUMINANT";
        options.subtitle =
            "OUTPUT-SIDE SPD USED FOR PRINT -> CIE XYZ";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "RELATIVE POWER";
        options.has_x_range = true;
        options.x_min =
            print_viewer.settings().wavelength_min_nm;
        options.x_max =
            print_viewer.settings().wavelength_max_nm;
        options.include_zero_y = true;
        options.x_padding_fraction = 0.0f;
        options.x_divisions = 16;

        const std::vector<Series> series = {
            {
                "INTEGRATION SPD",
                &sampled_viewing_illuminant,
                {{1.0f, 0.9f, 0.45f}},
                true,
                false,
                2
            },
            {
                "SOURCE SAMPLES",
                &source_viewing_illuminant,
                {{0.75f, 0.75f, 0.75f}},
                false,
                true,
                2
            }
        };

        success =
            write_plot(
                stem
                    + "_2383_viewing_illuminant.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // source-curve semantics audit: validate Kodak's source-curve semantics before changing
    // the active renderer. Kodak documents the C/M/Y curves as
    // peak-normalized dye shapes, so their raw ordinates are not absolute
    // additive density contributions. The separate Visual Neutral curve is
    // the neutral reference. These plots quantify that distinction only.
    // ------------------------------------------------------------------
    {
        const SampledCurve& visual =
            print_dye_model.visual_neutral_density();
        const SampledCurve& cyan =
            print_dye_model.cyan_reference_density();
        const SampledCurve& magenta =
            print_dye_model.magenta_reference_density();
        const SampledCurve& yellow =
            print_dye_model.yellow_reference_density();

        if (visual.valid()
            && cyan.valid()
            && magenta.valid()
            && yellow.valid()) {

            SampledCurve raw_sum;
            SampledCurve legacy_residual;

            for (std::size_t i = 0;
                 i < visual.x.size();
                 ++i) {

                const float wavelength_nm =
                    visual.x[i];

                const float sum =
                    cyan.sample(wavelength_nm, 0.0f)
                    + magenta.sample(wavelength_nm, 0.0f)
                    + yellow.sample(wavelength_nm, 0.0f);

                raw_sum.x.push_back(wavelength_nm);
                raw_sum.y.push_back(sum);

                legacy_residual.x.push_back(wavelength_nm);
                legacy_residual.y.push_back(
                    visual.y[i] - sum);
            }

            {
                PlotOptions options;
                options.title =
                    "KODAK 2383 SOURCE DYE SEMANTICS";
                options.subtitle =
                    "C/M/Y ARE PEAK-NORMALIZED SHAPES - RAW SUM IS NOT AN ABSOLUTE DENSITY MODEL";
                options.x_label =
                    "WAVELENGTH NM";
                options.y_label =
                    "PUBLISHED GRAPH ORDINATE";
                options.has_x_range = true;
                options.x_min =
                    print_viewer.settings().wavelength_min_nm;
                options.x_max =
                    print_viewer.settings().wavelength_max_nm;
                options.x_padding_fraction = 0.0f;
                options.include_zero_y = true;

                const std::vector<Series> series = {
                    {"VISUAL NEUTRAL", &visual, {{1.0f,1.0f,1.0f}}, true, true, 2},
                    {"CYAN PEAK-NORMALIZED", &cyan, {{0.25f,0.9f,1.0f}}, true, false, 2},
                    {"MAGENTA PEAK-NORMALIZED", &magenta, {{1.0f,0.35f,0.85f}}, true, false, 2},
                    {"YELLOW PEAK-NORMALIZED", &yellow, {{1.0f,0.9f,0.25f}}, true, false, 2},
                    {"RAW C+M+Y", &raw_sum, {{1.0f,0.45f,0.25f}}, true, false, 2}
                };

                success =
                    write_plot(
                        stem
                            + "_2383_source_dye_semantics.png",
                        series,
                        options)
                    && success;
            }

            {
                PlotOptions options;
                options.title =
                    "KODAK 2383 LEGACY RAW-SUM RESIDUAL";
                options.subtitle =
                    "VISUAL NEUTRAL - RAW PEAK-NORMALIZED C/M/Y  NOT A PHYSICAL BASE/STAIN MEASUREMENT";
                options.x_label =
                    "WAVELENGTH NM";
                options.y_label =
                    "ORDINATE DIFFERENCE";
                options.has_x_range = true;
                options.x_min =
                    print_viewer.settings().wavelength_min_nm;
                options.x_max =
                    print_viewer.settings().wavelength_max_nm;
                options.x_padding_fraction = 0.0f;
                options.include_zero_y = true;

                const std::vector<Series> series = {
                    {"LEGACY DIFFERENCE", &legacy_residual, {{1.0f,0.55f,0.25f}}, true, true, 2}
                };

                success =
                    write_plot(
                        stem
                            + "_2383_legacy_raw_sum_residual.png",
                        series,
                        options)
                    && success;
            }
        }
    }

    // ------------------------------------------------------------------
    // constrained basis-fit diagnostic: constrained spectral-basis fitting.
    //
    // Kodak's C/M/Y source curves are peak-normalized shapes. Fit their
    // amplitudes to the measured Visual Neutral spectrum instead of assuming
    // unit amplitudes. Two deliberately simple models are compared:
    //
    //   A: aC*C + aM*M + aY*Y
    //   B: b + aC*C + aM*M + aY*Y
    //
    // All fitted amplitudes, including the optional constant baseline, are
    // constrained nonnegative. This is diagnostic-only; the active
    // PrintDyeModel remains unchanged in constrained basis-fit diagnostic.
    // ------------------------------------------------------------------
    {
        const SampledCurve& visual =
            print_dye_model.visual_neutral_density();
        const SampledCurve& cyan =
            print_dye_model.cyan_reference_density();
        const SampledCurve& magenta =
            print_dye_model.magenta_reference_density();
        const SampledCurve& yellow =
            print_dye_model.yellow_reference_density();

        const SampledCurve constant =
            make_constant_basis(visual, 1.0f);

        const ConstrainedBasisFit model_a =
            fit_nonnegative_basis(
                visual,
                {&cyan, &magenta, &yellow},
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm);

        const ConstrainedBasisFit model_b =
            fit_nonnegative_basis(
                visual,
                {&constant, &cyan, &magenta, &yellow},
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm);

        if (model_a.valid && model_b.valid) {
            const SampledCurve visual_cropped =
                crop_curve_to_range(
                    visual,
                    print_viewer.settings().wavelength_min_nm,
                    print_viewer.settings().wavelength_max_nm);

            const PrintViewer::Result visual_viewed =
                print_viewer.view(
                    density_to_transmittance(
                        visual_cropped));

            const PrintViewer::Result model_a_viewed =
                print_viewer.view(
                    density_to_transmittance(
                        model_a.reconstructed));

            const PrintViewer::Result model_b_viewed =
                print_viewer.view(
                    density_to_transmittance(
                        model_b.reconstructed));

            auto xy_distance =
                [](const PrintViewer::xy& a,
                   const PrintViewer::xy& b) {

                const float dx = a.x - b.x;
                const float dy = a.y - b.y;
                return std::sqrt(dx * dx + dy * dy);
            };

            std::cout << "info: constrained basis-fit diagnostic Kodak 2383 constrained basis-fit diagnostics" << std::endl;
            std::cout
                << "info:   model A nonnegative amplitudes C/M/Y: "
                << model_a.coefficients[0] << ", "
                << model_a.coefficients[1] << ", "
                << model_a.coefficients[2]
                << std::endl;
            std::cout
                << "info:   model A RMS/max spectral-density error: "
                << model_a.rms_error << " / "
                << model_a.max_abs_error
                << " at "
                << model_a.max_error_wavelength_nm
                << " nm"
                << std::endl;
            std::cout
                << "info:   model A viewed xy: "
                << model_a_viewed.viewed_xy.x << ", "
                << model_a_viewed.viewed_xy.y
                << "  delta from measured Visual Neutral xy: "
                << xy_distance(
                    model_a_viewed.viewed_xy,
                    visual_viewed.viewed_xy)
                << std::endl;

            std::cout
                << "info:   model B nonnegative baseline/C/M/Y: "
                << model_b.coefficients[0] << ", "
                << model_b.coefficients[1] << ", "
                << model_b.coefficients[2] << ", "
                << model_b.coefficients[3]
                << std::endl;
            std::cout
                << "info:   model B RMS/max spectral-density error: "
                << model_b.rms_error << " / "
                << model_b.max_abs_error
                << " at "
                << model_b.max_error_wavelength_nm
                << " nm"
                << std::endl;
            std::cout
                << "info:   model B viewed xy: "
                << model_b_viewed.viewed_xy.x << ", "
                << model_b_viewed.viewed_xy.y
                << "  delta from measured Visual Neutral xy: "
                << xy_distance(
                    model_b_viewed.viewed_xy,
                    visual_viewed.viewed_xy)
                << std::endl;
            std::cout
                << "info:   measured Visual Neutral viewed xy under current D55 viewer: "
                << visual_viewed.viewed_xy.x << ", "
                << visual_viewed.viewed_xy.y
                << std::endl;
            std::cout
                << "info:   NOTE: constrained basis-fit diagnostic fits source-curve amplitudes only; active print rendering is linear C/M/Y reference model residual-free C/M/Y"
                << std::endl;

            {
                PlotOptions options;
                options.title =
                    "KODAK 2383 CONSTRAINED DYE-BASIS FIT";
                options.subtitle =
                    "MEASURED VISUAL NEUTRAL VS NONNEGATIVE C/M/Y FITS  DIAGNOSTIC ONLY";
                options.x_label =
                    "WAVELENGTH NM";
                options.y_label =
                    "SPECTRAL DENSITY";
                options.has_x_range = true;
                options.x_min =
                    print_viewer.settings().wavelength_min_nm;
                options.x_max =
                    print_viewer.settings().wavelength_max_nm;
                options.x_padding_fraction = 0.0f;
                options.include_zero_y = true;

                const std::vector<Series> series = {
                    {"MEASURED VISUAL NEUTRAL", &visual_cropped, {{1.0f,1.0f,1.0f}}, true, true, 2},
                    {"MODEL A  C/M/Y", &model_a.reconstructed, {{1.0f,0.55f,0.25f}}, true, false, 2},
                    {"MODEL B  BASE+C/M/Y", &model_b.reconstructed, {{0.35f,1.0f,0.45f}}, true, false, 2}
                };

                success =
                    write_plot(
                        stem
                            + "_2383_constrained_basis_fit.png",
                        series,
                        options)
                    && success;
            }

            {
                PlotOptions options;
                options.title =
                    "KODAK 2383 CONSTRAINED DYE-BASIS FIT RESIDUALS";
                options.subtitle =
                    "RECONSTRUCTED - MEASURED VISUAL NEUTRAL  LOWER IS BETTER";
                options.x_label =
                    "WAVELENGTH NM";
                options.y_label =
                    "DENSITY ERROR";
                options.has_x_range = true;
                options.x_min =
                    print_viewer.settings().wavelength_min_nm;
                options.x_max =
                    print_viewer.settings().wavelength_max_nm;
                options.x_padding_fraction = 0.0f;
                options.include_zero_y = true;

                const std::vector<Series> series = {
                    {"MODEL A  C/M/Y", &model_a.residual, {{1.0f,0.55f,0.25f}}, true, true, 2},
                    {"MODEL B  BASE+C/M/Y", &model_b.residual, {{0.35f,1.0f,0.45f}}, true, true, 2}
                };

                success =
                    write_plot(
                        stem
                            + "_2383_constrained_basis_fit_residuals.png",
                        series,
                        options)
                    && success;
            }
        }
    }

    // ------------------------------------------------------------------
    // D55 colorimetric reference calibration: colorimetric reference calibration of the three Kodak
    // peak-normalized dye shapes.
    //
    // Instead of minimizing unweighted spectral-density error, solve for
    // nonnegative C/M/Y amplitudes whose D55-viewed XYZ matches the measured
    // Kodak Visual Neutral reference. Matching XYZ constrains both chromaticity
    // and luminance under the validated viewing integration diagnostic viewer. The active
    // PrintDyeModel remains unchanged; this is diagnostic-only.
    // ------------------------------------------------------------------
    {
        const SampledCurve& visual =
            print_dye_model.visual_neutral_density();
        const SampledCurve& cyan =
            print_dye_model.cyan_reference_density();
        const SampledCurve& magenta =
            print_dye_model.magenta_reference_density();
        const SampledCurve& yellow =
            print_dye_model.yellow_reference_density();

        const float wavelength_min_nm =
            print_viewer.settings().wavelength_min_nm;
        const float wavelength_max_nm =
            print_viewer.settings().wavelength_max_nm;

        const ConstrainedBasisFit spectral_start =
            fit_nonnegative_basis(
                visual,
                {&cyan, &magenta, &yellow},
                wavelength_min_nm,
                wavelength_max_nm);

        std::array<float, 3> initial = {{1.0f, 1.0f, 1.0f}};
        if (spectral_start.valid
            && spectral_start.coefficients.size() == 3) {
            initial = {{
                spectral_start.coefficients[0],
                spectral_start.coefficients[1],
                spectral_start.coefficients[2]
            }};
        }

        const ColorimetricBasisCalibration calibration =
            calibrate_colorimetric_basis(
                visual,
                cyan,
                magenta,
                yellow,
                print_viewer,
                wavelength_min_nm,
                wavelength_max_nm,
                initial);

        if (calibration.valid) {
            const SampledCurve visual_cropped =
                crop_curve_to_range(
                    visual,
                    wavelength_min_nm,
                    wavelength_max_nm);

            SampledCurve residual;
            double squared_error_sum = 0.0;
            float max_abs_error = 0.0f;
            float max_error_wavelength_nm = 0.0f;

            for (std::size_t i = 0;
                 i < visual_cropped.x.size();
                 ++i) {

                const float wavelength_nm = visual_cropped.x[i];
                const float error =
                    calibration.reconstructed_density.sample(
                        wavelength_nm,
                        0.0f)
                    - visual_cropped.y[i];

                residual.x.push_back(wavelength_nm);
                residual.y.push_back(error);
                squared_error_sum += static_cast<double>(error) * error;

                if (std::abs(error) > max_abs_error) {
                    max_abs_error = std::abs(error);
                    max_error_wavelength_nm = wavelength_nm;
                }
            }

            const float spectral_rms =
                !residual.y.empty()
                    ? static_cast<float>(
                        std::sqrt(
                            squared_error_sum
                            / static_cast<double>(residual.y.size())))
                    : 0.0f;

            std::array<SampledCurve, 3> local_xy;
            const std::array<float, 3> multipliers = {{0.90f, 1.00f, 1.10f}};

            for (int dye = 0; dye < 3; ++dye) {
                for (float multiplier : multipliers) {
                    std::array<float, 3> c = calibration.coefficients;
                    c[dye] *= multiplier;

                    const SampledCurve perturbed_density =
                        combine_density_bases(
                            visual_cropped,
                            cyan,
                            magenta,
                            yellow,
                            c);

                    const PrintViewer::Result viewed =
                        print_viewer.view(
                            density_to_transmittance(
                                perturbed_density));

                    local_xy[dye].x.push_back(viewed.viewed_xy.x);
                    local_xy[dye].y.push_back(viewed.viewed_xy.y);
                }
            }

            auto xy_distance =
                [](const PrintViewer::xy& a,
                   const PrintViewer::xy& b) {
                    const float dx = a.x - b.x;
                    const float dy = a.y - b.y;
                    return std::sqrt(dx * dx + dy * dy);
                };

            std::cout << "info: D55 colorimetric reference calibration Kodak 2383 colorimetric reference calibration" << std::endl;
            std::cout
                << "info:   calibrated nonnegative amplitudes C/M/Y: "
                << calibration.coefficients[0] << ", "
                << calibration.coefficients[1] << ", "
                << calibration.coefficients[2]
                << std::endl;
            std::cout
                << "info:   target measured Visual Neutral XYZ under D55: "
                << calibration.target.viewed_xyz.x << ", "
                << calibration.target.viewed_xyz.y << ", "
                << calibration.target.viewed_xyz.z
                << std::endl;
            std::cout
                << "info:   calibrated basis XYZ under D55: "
                << calibration.viewed.viewed_xyz.x << ", "
                << calibration.viewed.viewed_xyz.y << ", "
                << calibration.viewed.viewed_xyz.z
                << std::endl;
            std::cout
                << "info:   target/calibrated xy: "
                << calibration.target.viewed_xy.x << ", "
                << calibration.target.viewed_xy.y << " / "
                << calibration.viewed.viewed_xy.x << ", "
                << calibration.viewed.viewed_xy.y
                << "  delta="
                << xy_distance(
                    calibration.target.viewed_xy,
                    calibration.viewed.viewed_xy)
                << std::endl;
            std::cout
                << "info:   relative XYZ RMS mismatch: "
                << calibration.relative_xyz_rms
                << std::endl;
            std::cout
                << "info:   spectral-density RMS/max error after colorimetric calibration: "
                << spectral_rms << " / "
                << max_abs_error << " at "
                << max_error_wavelength_nm << " nm"
                << std::endl;
            std::cout
                << "info:   local perturbation: each dye amplitude tested at -10% / reference / +10%"
                << std::endl;
            std::cout
                << "info:   NOTE: D55 colorimetric reference calibration calibrates only the reference neutral under the current D55 viewer; active print rendering is linear C/M/Y reference model residual-free C/M/Y"
                << std::endl;

            {
                PlotOptions options;
                options.title =
                    "KODAK 2383 COLORIMETRIC REFERENCE CALIBRATION";
                options.subtitle =
                    "C/M/Y AMPLITUDES MATCH MEASURED VISUAL-NEUTRAL XYZ UNDER D55  DIAGNOSTIC ONLY";
                options.x_label =
                    "WAVELENGTH NM";
                options.y_label =
                    "SPECTRAL DENSITY";
                options.has_x_range = true;
                options.x_min = wavelength_min_nm;
                options.x_max = wavelength_max_nm;
                options.x_padding_fraction = 0.0f;
                options.include_zero_y = true;

                const std::vector<Series> series = {
                    {"MEASURED VISUAL NEUTRAL", &visual_cropped, {{1.0f,1.0f,1.0f}}, true, true, 2},
                    {"COLORIMETRIC C/M/Y CALIBRATION", &calibration.reconstructed_density, {{0.35f,1.0f,0.45f}}, true, false, 2}
                };

                success =
                    write_plot(
                        stem
                            + "_2383_colorimetric_reference_calibration.png",
                        series,
                        options)
                    && success;
            }

            {
                PlotOptions options;
                options.title =
                    "KODAK 2383 LOCAL DYE-BASIS CHROMATICITY RESPONSE";
                options.subtitle =
                    "CIE XY TRAJECTORIES FOR +/-10% C/M/Y AMPLITUDE AROUND COLORIMETRIC NEUTRAL";
                options.x_label =
                    "CIE x";
                options.y_label =
                    "CIE y";
                options.x_divisions = 8;
                options.y_divisions = 8;

                const std::vector<Series> series = {
                    {"CYAN AMPLITUDE", &local_xy[0], {{0.25f,0.90f,1.0f}}, true, true, 4},
                    {"MAGENTA AMPLITUDE", &local_xy[1], {{1.0f,0.25f,0.90f}}, true, true, 4},
                    {"YELLOW AMPLITUDE", &local_xy[2], {{1.0f,0.90f,0.20f}}, true, true, 4}
                };

                success =
                    write_plot(
                        stem
                            + "_2383_local_dye_basis_xy.png",
                        series,
                        options)
                    && success;
            }

            {
                PlotOptions options;
                options.title =
                    "KODAK 2383 COLORIMETRIC CALIBRATION SPECTRAL RESIDUAL";
                options.subtitle =
                    "CALIBRATED C/M/Y DENSITY - MEASURED VISUAL NEUTRAL  NOT USED BY RENDERER";
                options.x_label =
                    "WAVELENGTH NM";
                options.y_label =
                    "DENSITY ERROR";
                options.has_x_range = true;
                options.x_min = wavelength_min_nm;
                options.x_max = wavelength_max_nm;
                options.x_padding_fraction = 0.0f;
                options.include_zero_y = true;

                const std::vector<Series> series = {
                    {"COLORIMETRIC FIT RESIDUAL", &residual, {{1.0f,0.55f,0.25f}}, true, true, 2}
                };

                success =
                    write_plot(
                        stem
                            + "_2383_colorimetric_reference_residual.png",
                        series,
                        options)
                    && success;
            }
        }
    }

    // Status-A mapping audit: ANSI Status-A mapping audit.
    //
    // The Kodak sensitometric curves are Status-A density measurements, while
    // linear C/M/Y reference model directly maps those three record values to C/M/Y amplitudes.
    // Here we use explicit ANSI Status-A spectral weighting to measure the
    // published dye shapes themselves, derive the local 3x3 Jacobian, and
    // compare two distinct reference constraints:
    //
    //   1. D55 colorimetric reference calibration D55/CIE colorimetric neutral amplitudes.
    //   2. C/M/Y amplitudes that synthesize Status-A R=G=B=1 exactly.
    //
    // If those constraints disagree, a simple record->dye matrix cannot be
    // promoted to production without choosing which measurement definition is
    // authoritative. Status-A mapping audit therefore remains diagnostic only.
    // ------------------------------------------------------------------
    const auto& status_a_audit_dye_diag = print_dye_model.diagnostics();

    const Vector3 status_a_audit_colorimetric_reference = {{
        status_a_audit_dye_diag.calibrated_reference_amplitude.red,
        status_a_audit_dye_diag.calibrated_reference_amplitude.green,
        status_a_audit_dye_diag.calibrated_reference_amplitude.blue
    }};

    const Vector3 status_a_audit_colorimetric_status =
        status_a_density(print_stock, status_a_audit_colorimetric_reference);

    const Vector3 status_a_audit_status_reference =
        solve_status_a_neutral_amplitudes(
            print_stock,
            status_a_audit_colorimetric_reference);

    const Vector3 status_a_audit_status_reference_check =
        status_a_density(print_stock, status_a_audit_status_reference);

    const Matrix3 status_a_audit_status_jacobian =
        status_a_jacobian(print_stock, status_a_audit_status_reference);

    const SampledCurve status_a_audit_status_reference_transmittance =
        source_transmittance_from_amplitudes(
            print_stock,
            status_a_audit_status_reference,
            print_viewer.settings().wavelength_min_nm,
            print_viewer.settings().wavelength_max_nm,
            print_viewer.settings().wavelength_step_nm);

    const PrintViewer::Result status_a_audit_status_reference_viewed =
        print_viewer.view(status_a_audit_status_reference_transmittance);

    SampledCurve status_a_audit_measured_visual_neutral_transmittance;
    const auto& status_a_audit_source_dye = print_stock.dye_density();
    for (float wavelength_nm = print_viewer.settings().wavelength_min_nm;
         wavelength_nm <= print_viewer.settings().wavelength_max_nm + 0.001f;
         wavelength_nm += print_viewer.settings().wavelength_step_nm) {
        const float density = status_a_audit_source_dye.visual_neutral_density.sample(
            wavelength_nm,
            std::numeric_limits<float>::quiet_NaN());
        if (std::isfinite(density)) {
            status_a_audit_measured_visual_neutral_transmittance.x.push_back(wavelength_nm);
            status_a_audit_measured_visual_neutral_transmittance.y.push_back(
                std::pow(10.0f, -density));
        }
    }

    const PrintViewer::Result status_a_audit_measured_visual_neutral_viewed =
        print_viewer.view(status_a_audit_measured_visual_neutral_transmittance);

    const float status_a_audit_reference_dx =
        status_a_audit_status_reference_viewed.viewed_xy.x
        - status_a_audit_measured_visual_neutral_viewed.viewed_xy.x;
    const float status_a_audit_reference_dy =
        status_a_audit_status_reference_viewed.viewed_xy.y
        - status_a_audit_measured_visual_neutral_viewed.viewed_xy.y;
    const float status_a_audit_reference_delta_xy =
        std::sqrt(
            status_a_audit_reference_dx * status_a_audit_reference_dx
            + status_a_audit_reference_dy * status_a_audit_reference_dy);

    std::cout << "info: Status-A mapping audit ANSI Status-A record/dye mapping audit" << std::endl;
    std::cout << "info:   Status-A weighting grid: 340 .. 830 nm / 10 nm (python-colormath ANSI Status-A tables)" << std::endl;
    std::cout
        << "info:   D55 colorimetric reference calibration colorimetric C/M/Y amplitudes: "
        << status_a_audit_colorimetric_reference[0] << ", "
        << status_a_audit_colorimetric_reference[1] << ", "
        << status_a_audit_colorimetric_reference[2] << std::endl;
    std::cout
        << "info:   synthetic ANSI Status-A R/G/B at colorimetric reference: "
        << status_a_audit_colorimetric_status[0] << ", "
        << status_a_audit_colorimetric_status[1] << ", "
        << status_a_audit_colorimetric_status[2] << std::endl;
    std::cout
        << "info:   amplitudes solving synthetic Status-A 1/1/1: "
        << status_a_audit_status_reference[0] << ", "
        << status_a_audit_status_reference[1] << ", "
        << status_a_audit_status_reference[2] << std::endl;
    std::cout
        << "info:   solved Status-A check R/G/B: "
        << status_a_audit_status_reference_check[0] << ", "
        << status_a_audit_status_reference_check[1] << ", "
        << status_a_audit_status_reference_check[2] << std::endl;
    std::cout
        << "info:   Status-A-neutral viewed xy / measured Visual Neutral xy: "
        << status_a_audit_status_reference_viewed.viewed_xy.x << ", "
        << status_a_audit_status_reference_viewed.viewed_xy.y << " / "
        << status_a_audit_measured_visual_neutral_viewed.viewed_xy.x << ", "
        << status_a_audit_measured_visual_neutral_viewed.viewed_xy.y
        << "  delta=" << status_a_audit_reference_delta_xy << std::endl;
    std::cout << "info:   local Status-A Jacobian d(R,G,B)/d(C,M,Y):" << std::endl;
    for (int row = 0; row < 3; ++row) {
        std::cout
            << "info:     [ "
            << status_a_audit_status_jacobian[row][0] << ", "
            << status_a_audit_status_jacobian[row][1] << ", "
            << status_a_audit_status_jacobian[row][2] << " ]"
            << std::endl;
    }
    std::cout
        << "info:   NOTE: ANSI Status-A spectral weights are diagnostic-only; "
        << "active rendering remains linear C/M/Y reference model residual-free C/M/Y"
        << std::endl;

    std::cout << "info: neutral-preserving inverse neutral-preserving dye-growth inference" << std::endl;
    std::cout
        << "info:   target chromaticity: Kodak measured Visual Neutral under current D55 viewer xy="
        << status_a_audit_measured_visual_neutral_viewed.viewed_xy.x << ", "
        << status_a_audit_measured_visual_neutral_viewed.viewed_xy.y << std::endl;
    std::cout << "info:   target tone per ladder point: preserve linear C/M/Y reference model viewed Y" << std::endl;
    std::cout << "info:   solve: nonlinear 3x3 XYZ inverse from Kodak C/M/Y amplitudes, initialized from linear C/M/Y reference model linear amplitudes" << std::endl;
    std::cout << "info:   NOTE: diagnostic only; active rendering remains linear C/M/Y reference model residual-free C/M/Y" << std::endl;

    {
        SampledCurve row_r;
        SampledCurve row_g;
        SampledCurve row_b;
        for (int column = 0; column < 3; ++column) {
            row_r.x.push_back(static_cast<float>(column));
            row_r.y.push_back(status_a_audit_status_jacobian[0][column]);
            row_g.x.push_back(static_cast<float>(column));
            row_g.y.push_back(status_a_audit_status_jacobian[1][column]);
            row_b.x.push_back(static_cast<float>(column));
            row_b.y.push_back(status_a_audit_status_jacobian[2][column]);
        }

        PlotOptions options;
        options.title = "KODAK 2383 STATUS-A AUDIT STATUS-A JACOBIAN";
        options.subtitle = "ROWS STATUS-A R/G/B  COLUMNS 0=C 1=M 2=Y  DIAGNOSTIC ONLY";
        options.x_label = "DYE AMPLITUDE INDEX  0 C   1 M   2 Y";
        options.y_label = "D STATUS-A / D DYE AMPLITUDE";
        options.has_x_range = true;
        options.x_min = 0.0f;
        options.x_max = 2.0f;
        options.x_divisions = 2;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"STATUS-A R", &row_r, {{1.0f,0.25f,0.25f}}, true, true, 4},
            {"STATUS-A G", &row_g, {{0.25f,1.0f,0.25f}}, true, true, 4},
            {"STATUS-A B", &row_b, {{0.25f,0.55f,1.0f}}, true, true, 4}
        };

        success =
            write_plot(
                stem + "_2383_status_a_status_a_jacobian.png",
                series,
                options)
            && success;
    }

    const std::array<float, 5> stop_values = {{
        -4.0f,
        -2.0f,
         0.0f,
         2.0f,
         4.0f
    }};

    SampledCurve viewed_x;
    SampledCurve viewed_y;
    SampledCurve ap0_r;
    SampledCurve ap0_g;
    SampledCurve ap0_b;

    // linear C/M/Y reference model explicit A/B regression against the retired residual model.
    SampledCurve legacy_delta_xy;
    SampledCurve new_delta_xy;
    SampledCurve legacy_ap0_spread;
    SampledCurve new_ap0_spread;
    SampledCurve matrix_delta_xy;
    SampledCurve matrix_ap0_spread;

    // neutral-preserving inverse diagnostic-only neutral-preserving inverse.
    SampledCurve neutral_inverse_delta_xy;
    SampledCurve neutral_inverse_ap0_spread;
    SampledCurve neutral_inverse_linear_c;
    SampledCurve neutral_inverse_linear_m;
    SampledCurve neutral_inverse_linear_y;
    SampledCurve neutral_inverse_inferred_c;
    SampledCurve neutral_inverse_inferred_m;
    SampledCurve neutral_inverse_inferred_y;

    // neutral-drift diagnostic neutral-drift decomposition retained for regression context.
    //
    // These controls deliberately do not alter the active pixel pipeline.
    // They ask which already-modelled stage is responsible for the observed
    // density-dependent neutral-axis movement.
    SampledCurve real_delta_xy;
    SampledCurve equal_status_a_delta_xy;
    SampledCurve matched_increment_delta_xy;
    SampledCurve scaled_visual_neutral_delta_xy;

    SampledCurve normalized_increment_r;
    SampledCurve normalized_increment_g;
    SampledCurve normalized_increment_b;
    SampledCurve normalized_increment_mean;

    std::array<SampledCurve, 3> viewed_spectra;

    const float log10_2 =
        std::log10(2.0f);

    for (std::size_t i = 0;
         i < stop_values.size();
         ++i) {

        const float negative_log_exposure =
            negative_dye_model.diagnostics()
                .calibration_log_exposure
            + stop_values[i]
                * log10_2;

        const SampledCurve negative_transmittance =
            negative_dye_model.synthesize_neutral_transmittance(
                negative_stock,
                negative_log_exposure);

        const FilmDensity print_density =
            print_processor.process(
                negative_transmittance);

        const SampledCurve print_transmittance =
            print_dye_model.synthesize_transmittance(
                print_density);

        const PrintViewer::Result viewed =
            print_viewer.view(
                print_transmittance);

        const PrintViewer::Result legacy_viewed =
            print_viewer.view(
                print_dye_model.synthesize_transmittance_legacy(
                    print_density));

        Vector3 status_a_audit_status_delta = {{
            print_density.red - 1.0f,
            print_density.green - 1.0f,
            print_density.blue - 1.0f
        }};
        Vector3 status_a_audit_amplitude_delta = {{0.0f, 0.0f, 0.0f}};
        solve_3x3(
            status_a_audit_status_jacobian,
            status_a_audit_status_delta,
            status_a_audit_amplitude_delta);

        Vector3 status_a_audit_matrix_amplitudes = status_a_audit_status_reference;
        for (int component = 0; component < 3; ++component) {
            status_a_audit_matrix_amplitudes[component] =
                std::max(
                    0.0f,
                    status_a_audit_matrix_amplitudes[component]
                        + status_a_audit_amplitude_delta[component]);
        }

        const SampledCurve status_a_audit_matrix_transmittance =
            source_transmittance_from_amplitudes(
                print_stock,
                status_a_audit_matrix_amplitudes,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm);

        const PrintViewer::Result status_a_audit_matrix_viewed =
            print_viewer.view(status_a_audit_matrix_transmittance);

        const auto& dye_diagnostics =
            print_dye_model.diagnostics();

        const FilmDensity& dmin =
            dye_diagnostics.minimum_record_density;

        const FilmDensity& reference_increment =
            dye_diagnostics.reference_record_increment;

        const float normalized_r =
            reference_increment.red > 1e-12f
                ? std::max(
                    0.0f,
                    (print_density.red - dmin.red)
                        / reference_increment.red)
                : 0.0f;

        const float normalized_g =
            reference_increment.green > 1e-12f
                ? std::max(
                    0.0f,
                    (print_density.green - dmin.green)
                        / reference_increment.green)
                : 0.0f;

        const float normalized_b =
            reference_increment.blue > 1e-12f
                ? std::max(
                    0.0f,
                    (print_density.blue - dmin.blue)
                        / reference_increment.blue)
                : 0.0f;

        const float normalized_mean =
            (normalized_r
             + normalized_g
             + normalized_b)
            / 3.0f;

        const FilmDensity& neutral_inverse_reference_amplitude_density =
            dye_diagnostics.calibrated_reference_amplitude;

        Vector3 neutral_inverse_linear_amplitudes = {{
            neutral_inverse_reference_amplitude_density.red * normalized_r,
            neutral_inverse_reference_amplitude_density.green * normalized_g,
            neutral_inverse_reference_amplitude_density.blue * normalized_b
        }};

        const float neutral_inverse_target_x = status_a_audit_measured_visual_neutral_viewed.viewed_xy.x;
        const float neutral_inverse_target_y_chromaticity = status_a_audit_measured_visual_neutral_viewed.viewed_xy.y;
        const float neutral_inverse_target_Y = viewed.viewed_xyz.y;
        const float neutral_inverse_target_z_chromaticity =
            std::max(0.0f, 1.0f - neutral_inverse_target_x - neutral_inverse_target_y_chromaticity);

        PrintViewer::XYZ neutral_inverse_target_xyz;
        if (neutral_inverse_target_y_chromaticity > 1e-12f) {
            neutral_inverse_target_xyz.x = neutral_inverse_target_Y * neutral_inverse_target_x / neutral_inverse_target_y_chromaticity;
            neutral_inverse_target_xyz.y = neutral_inverse_target_Y;
            neutral_inverse_target_xyz.z = neutral_inverse_target_Y * neutral_inverse_target_z_chromaticity / neutral_inverse_target_y_chromaticity;
        }

        Vector3 neutral_inverse_inferred_amplitudes = neutral_inverse_linear_amplitudes;
        bool neutral_inverse_solve_ok = true;

        for (int iteration = 0; iteration < 16; ++iteration) {
            const PrintViewer::Result current_viewed = print_viewer.view(
                source_transmittance_from_amplitudes(
                    print_stock, neutral_inverse_inferred_amplitudes,
                    print_viewer.settings().wavelength_min_nm,
                    print_viewer.settings().wavelength_max_nm,
                    print_viewer.settings().wavelength_step_nm));

            Vector3 xyz_error = {{
                neutral_inverse_target_xyz.x - current_viewed.viewed_xyz.x,
                neutral_inverse_target_xyz.y - current_viewed.viewed_xyz.y,
                neutral_inverse_target_xyz.z - current_viewed.viewed_xyz.z
            }};

            const float max_error = std::max(
                std::abs(xyz_error[0]),
                std::max(std::abs(xyz_error[1]), std::abs(xyz_error[2])));
            if (max_error < 1e-7f) {
                break;
            }

            Matrix3 xyz_jacobian = {{
                {{0.0f, 0.0f, 0.0f}},
                {{0.0f, 0.0f, 0.0f}},
                {{0.0f, 0.0f, 0.0f}}
            }};

            for (int column = 0; column < 3; ++column) {
                const float h = std::max(
                    1e-4f,
                    1e-3f * std::max(1.0f, neutral_inverse_inferred_amplitudes[column]));
                Vector3 plus = neutral_inverse_inferred_amplitudes;
                Vector3 minus = neutral_inverse_inferred_amplitudes;
                plus[column] += h;
                minus[column] = std::max(0.0f, minus[column] - h);
                const float denominator = plus[column] - minus[column];

                const PrintViewer::Result plus_viewed = print_viewer.view(
                    source_transmittance_from_amplitudes(
                        print_stock, plus,
                        print_viewer.settings().wavelength_min_nm,
                        print_viewer.settings().wavelength_max_nm,
                        print_viewer.settings().wavelength_step_nm));
                const PrintViewer::Result minus_viewed = print_viewer.view(
                    source_transmittance_from_amplitudes(
                        print_stock, minus,
                        print_viewer.settings().wavelength_min_nm,
                        print_viewer.settings().wavelength_max_nm,
                        print_viewer.settings().wavelength_step_nm));

                if (denominator <= 0.0f) {
                    neutral_inverse_solve_ok = false;
                    break;
                }
                xyz_jacobian[0][column] = (plus_viewed.viewed_xyz.x - minus_viewed.viewed_xyz.x) / denominator;
                xyz_jacobian[1][column] = (plus_viewed.viewed_xyz.y - minus_viewed.viewed_xyz.y) / denominator;
                xyz_jacobian[2][column] = (plus_viewed.viewed_xyz.z - minus_viewed.viewed_xyz.z) / denominator;
            }

            if (!neutral_inverse_solve_ok) {
                break;
            }

            Vector3 amplitude_delta = {{0.0f, 0.0f, 0.0f}};
            if (!solve_3x3(xyz_jacobian, xyz_error, amplitude_delta)) {
                neutral_inverse_solve_ok = false;
                break;
            }

            float step_scale = 1.0f;
            for (int component = 0; component < 3; ++component) {
                if (amplitude_delta[component] < 0.0f
                    && neutral_inverse_inferred_amplitudes[component] + amplitude_delta[component] < 0.0f) {
                    const float candidate = neutral_inverse_inferred_amplitudes[component]
                        / std::max(1e-12f, -amplitude_delta[component]);
                    step_scale = std::min(step_scale, 0.8f * candidate);
                }
            }
            step_scale = std::max(0.05f, std::min(1.0f, step_scale));

            for (int component = 0; component < 3; ++component) {
                neutral_inverse_inferred_amplitudes[component] = std::max(
                    0.0f,
                    neutral_inverse_inferred_amplitudes[component] + step_scale * amplitude_delta[component]);
            }
        }

        const PrintViewer::Result neutral_inverse_inferred_viewed = print_viewer.view(
            source_transmittance_from_amplitudes(
                print_stock, neutral_inverse_inferred_amplitudes,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm));

        neutral_inverse_linear_c.x.push_back(stop_values[i]); neutral_inverse_linear_c.y.push_back(neutral_inverse_linear_amplitudes[0]);
        neutral_inverse_linear_m.x.push_back(stop_values[i]); neutral_inverse_linear_m.y.push_back(neutral_inverse_linear_amplitudes[1]);
        neutral_inverse_linear_y.x.push_back(stop_values[i]); neutral_inverse_linear_y.y.push_back(neutral_inverse_linear_amplitudes[2]);
        neutral_inverse_inferred_c.x.push_back(stop_values[i]); neutral_inverse_inferred_c.y.push_back(neutral_inverse_inferred_amplitudes[0]);
        neutral_inverse_inferred_m.x.push_back(stop_values[i]); neutral_inverse_inferred_m.y.push_back(neutral_inverse_inferred_amplitudes[1]);
        neutral_inverse_inferred_y.x.push_back(stop_values[i]); neutral_inverse_inferred_y.y.push_back(neutral_inverse_inferred_amplitudes[2]);

        const float neutral_inverse_xy_error = std::sqrt(
            std::pow(neutral_inverse_inferred_viewed.viewed_xy.x - neutral_inverse_target_x, 2.0f)
            + std::pow(neutral_inverse_inferred_viewed.viewed_xy.y - neutral_inverse_target_y_chromaticity, 2.0f));
        const float neutral_inverse_y_relative_error = neutral_inverse_target_Y > 1e-12f
            ? std::abs(neutral_inverse_inferred_viewed.viewed_xyz.y - neutral_inverse_target_Y) / neutral_inverse_target_Y
            : 0.0f;

        std::cout
            << "info:   NEUTRAL INVERSE stop " << stop_values[i]
            << "  LINEAR REFERENCE C/M/Y=" << neutral_inverse_linear_amplitudes[0] << ", " << neutral_inverse_linear_amplitudes[1] << ", " << neutral_inverse_linear_amplitudes[2]
            << "  inferred=" << neutral_inverse_inferred_amplitudes[0] << ", " << neutral_inverse_inferred_amplitudes[1] << ", " << neutral_inverse_inferred_amplitudes[2]
            << "  xy_err=" << neutral_inverse_xy_error
            << "  rel_Y_err=" << neutral_inverse_y_relative_error
            << "  " << (neutral_inverse_solve_ok ? "PASS" : "SOLVE_FAIL")
            << std::endl;

        normalized_increment_r.x.push_back(stop_values[i]);
        normalized_increment_r.y.push_back(normalized_r);

        normalized_increment_g.x.push_back(stop_values[i]);
        normalized_increment_g.y.push_back(normalized_g);

        normalized_increment_b.x.push_back(stop_values[i]);
        normalized_increment_b.y.push_back(normalized_b);

        normalized_increment_mean.x.push_back(stop_values[i]);
        normalized_increment_mean.y.push_back(normalized_mean);

        // Control 1: force equal developed Status-A record densities while
        // preserving the real pipeline's average density. This removes the
        // R/G/B sensitometric separation at this point but retains the current
        // PrintDyeModel interpretation of equal record density.
        const float mean_status_a_density =
            (print_density.red
             + print_density.green
             + print_density.blue)
            / 3.0f;

        FilmDensity equal_status_a_density;
        equal_status_a_density.red = mean_status_a_density;
        equal_status_a_density.green = mean_status_a_density;
        equal_status_a_density.blue = mean_status_a_density;

        const PrintViewer::Result equal_status_a_viewed =
            print_viewer.view(
                print_dye_model.synthesize_transmittance(
                    equal_status_a_density));

        // Control 2: force a common normalized dye increment above each
        // record's measured D-min. At normalized_mean == 1 this is exactly
        // the Kodak density-1 reference used by PrintDyeModel.
        FilmDensity matched_increment_density;
        matched_increment_density.red =
            dmin.red
            + normalized_mean
                * reference_increment.red;
        matched_increment_density.green =
            dmin.green
            + normalized_mean
                * reference_increment.green;
        matched_increment_density.blue =
            dmin.blue
            + normalized_mean
                * reference_increment.blue;

        const PrintViewer::Result matched_increment_viewed =
            print_viewer.view(
                print_dye_model.synthesize_transmittance(
                    matched_increment_density));

        // Control 3: idealized shape-preserving scaling of Kodak's measured
        // visual-neutral spectral density. This also scales the residual/support
        // term, unlike the active PrintDyeModel, and therefore isolates the
        // chromaticity drift inherent in exponentiating the published neutral
        // density spectrum itself. This is diagnostic only.
        SampledCurve scaled_visual_neutral_transmittance;

        const SampledCurve& visual_neutral =
            print_dye_model.visual_neutral_density();

        scaled_visual_neutral_transmittance.x =
            visual_neutral.x;

        scaled_visual_neutral_transmittance.y.reserve(
            visual_neutral.y.size());

        for (float density_value :
             visual_neutral.y) {

            scaled_visual_neutral_transmittance.y.push_back(
                std::pow(
                    10.0f,
                    -normalized_mean
                        * density_value));
        }

        const PrintViewer::Result scaled_visual_neutral_viewed =
            print_viewer.view(
                scaled_visual_neutral_transmittance);

        const PrintViewer::xy viewing_white_xy =
            print_viewer.viewing_white_xy();

        auto chromaticity_distance =
            [&](const PrintViewer::xy& chromaticity) {

            const float dx =
                chromaticity.x
                - viewing_white_xy.x;

            const float dy =
                chromaticity.y
                - viewing_white_xy.y;

            return std::sqrt(
                dx * dx
                + dy * dy);
        };

        const float new_distance =
            chromaticity_distance(viewed.viewed_xy);
        const float legacy_distance =
            chromaticity_distance(legacy_viewed.viewed_xy);

        new_delta_xy.x.push_back(stop_values[i]);
        new_delta_xy.y.push_back(new_distance);
        legacy_delta_xy.x.push_back(stop_values[i]);
        legacy_delta_xy.y.push_back(legacy_distance);

        auto ap0_spread = [](const std::array<float, 3>& rgb) {
            const float minimum = std::min(rgb[0], std::min(rgb[1], rgb[2]));
            const float maximum = std::max(rgb[0], std::max(rgb[1], rgb[2]));
            const float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
            return mean > 1e-12f ? (maximum - minimum) / mean : 0.0f;
        };

        new_ap0_spread.x.push_back(stop_values[i]);
        new_ap0_spread.y.push_back(ap0_spread(viewed.aces2065_1));
        legacy_ap0_spread.x.push_back(stop_values[i]);
        legacy_ap0_spread.y.push_back(ap0_spread(legacy_viewed.aces2065_1));
        matrix_delta_xy.x.push_back(stop_values[i]);
        matrix_delta_xy.y.push_back(
            chromaticity_distance(status_a_audit_matrix_viewed.viewed_xy));
        matrix_ap0_spread.x.push_back(stop_values[i]);
        matrix_ap0_spread.y.push_back(
            ap0_spread(status_a_audit_matrix_viewed.aces2065_1));
        neutral_inverse_delta_xy.x.push_back(stop_values[i]);
        neutral_inverse_delta_xy.y.push_back(chromaticity_distance(neutral_inverse_inferred_viewed.viewed_xy));
        neutral_inverse_ap0_spread.x.push_back(stop_values[i]);
        neutral_inverse_ap0_spread.y.push_back(ap0_spread(neutral_inverse_inferred_viewed.aces2065_1));

        real_delta_xy.x.push_back(stop_values[i]);
        real_delta_xy.y.push_back(
            chromaticity_distance(
                viewed.viewed_xy));

        equal_status_a_delta_xy.x.push_back(stop_values[i]);
        equal_status_a_delta_xy.y.push_back(
            chromaticity_distance(
                equal_status_a_viewed.viewed_xy));

        matched_increment_delta_xy.x.push_back(stop_values[i]);
        matched_increment_delta_xy.y.push_back(
            chromaticity_distance(
                matched_increment_viewed.viewed_xy));

        scaled_visual_neutral_delta_xy.x.push_back(stop_values[i]);
        scaled_visual_neutral_delta_xy.y.push_back(
            chromaticity_distance(
                scaled_visual_neutral_viewed.viewed_xy));

        viewed_x.x.push_back(stop_values[i]);
        viewed_x.y.push_back(viewed.viewed_xy.x);

        viewed_y.x.push_back(stop_values[i]);
        viewed_y.y.push_back(viewed.viewed_xy.y);

        ap0_r.x.push_back(stop_values[i]);
        ap0_r.y.push_back(viewed.aces2065_1[0]);

        ap0_g.x.push_back(stop_values[i]);
        ap0_g.y.push_back(viewed.aces2065_1[1]);

        ap0_b.x.push_back(stop_values[i]);
        ap0_b.y.push_back(viewed.aces2065_1[2]);

        if (i == 0) {
            viewed_spectra[0] =
                print_viewer.viewed_spectrum(
                    print_transmittance);
        }
        else if (i == 2) {
            viewed_spectra[1] =
                print_viewer.viewed_spectrum(
                    print_transmittance);
        }
        else if (i == 4) {
            viewed_spectra[2] =
                print_viewer.viewed_spectrum(
                    print_transmittance);
        }
    }

    // ------------------------------------------------------------------
    // interpolation robustness audit: robustness/interpolation audit of the NEUTRAL INVERSE neutral-
    // preserving inverse.  Fit a monotone cubic mapping from the LINEAR REFERENCE
    // normalized record-growth variable to the NEUTRAL INVERSE normalized dye amplitude,
    // then evaluate it densely through the neutral ladder.  This is still
    // diagnostic only; active rendering remains linear C/M/Y reference model.
    // ------------------------------------------------------------------
    const FilmDensity& interpolation_audit_reference_amplitude =
        print_dye_model.diagnostics().calibrated_reference_amplitude;

    auto make_growth_mapping = [](const SampledCurve& linear,
                                  const SampledCurve& inferred,
                                  float reference) {
        SampledCurve mapping;
        if (reference <= 1e-12f) {
            return mapping;
        }
        for (std::size_t i = 0; i < linear.y.size() && i < inferred.y.size(); ++i) {
            mapping.x.push_back(linear.y[i] / reference);
            mapping.y.push_back(inferred.y[i] / reference);
        }
        std::vector<std::pair<float, float>> pairs;
        for (std::size_t i = 0; i < mapping.x.size(); ++i) {
            pairs.push_back(std::make_pair(mapping.x[i], mapping.y[i]));
        }
        std::sort(pairs.begin(), pairs.end(), [](const std::pair<float,float>& a,
                                                  const std::pair<float,float>& b) {
            return a.first < b.first;
        });
        mapping.x.clear();
        mapping.y.clear();
        for (const auto& pair : pairs) {
            mapping.x.push_back(pair.first);
            mapping.y.push_back(pair.second);
        }
        return mapping;
    };

    const SampledCurve interpolation_audit_growth_c = make_growth_mapping(
        neutral_inverse_linear_c, neutral_inverse_inferred_c, interpolation_audit_reference_amplitude.red);
    const SampledCurve interpolation_audit_growth_m = make_growth_mapping(
        neutral_inverse_linear_m, neutral_inverse_inferred_m, interpolation_audit_reference_amplitude.green);
    const SampledCurve interpolation_audit_growth_y = make_growth_mapping(
        neutral_inverse_linear_y, neutral_inverse_inferred_y, interpolation_audit_reference_amplitude.blue);

    SampledCurve interpolation_audit_dense_xy_error;
    SampledCurve interpolation_audit_dense_y_error;
    SampledCurve interpolation_audit_dense_delta_from_white;
    SampledCurve interpolation_audit_dense_ap0_spread;
    SampledCurve interpolation_audit_dense_c;
    SampledCurve interpolation_audit_dense_m;
    SampledCurve interpolation_audit_dense_y;
    SampledCurve interpolation_audit_crosstalk_c;
    SampledCurve interpolation_audit_crosstalk_m;
    SampledCurve interpolation_audit_crosstalk_y;
    SampledCurve interpolation_audit_xyz_condition;
    SampledCurve interpolation_audit_identity;

    for (int i = 0; i <= 80; ++i) {
        const float normalized = static_cast<float>(i) / 80.0f;
        const float stop = -4.0f + 8.0f * normalized;

        const float negative_log_exposure =
            negative_dye_model.diagnostics().calibration_log_exposure
            + stop * log10_2;
        const SampledCurve negative_transmittance =
            negative_dye_model.synthesize_neutral_transmittance(
                negative_stock, negative_log_exposure);
        const FilmDensity print_density = print_processor.process(negative_transmittance);
        const PrintViewer::Result linear_reference_viewed = print_viewer.view(
            print_dye_model.synthesize_transmittance(print_density));

        const auto& interpolation_audit_diag = print_dye_model.diagnostics();
        const FilmDensity& interpolation_audit_dmin = interpolation_audit_diag.minimum_record_density;
        const FilmDensity& interpolation_audit_ref_increment = interpolation_audit_diag.reference_record_increment;

        const float n_r = interpolation_audit_ref_increment.red > 1e-12f
            ? std::max(0.0f, (print_density.red - interpolation_audit_dmin.red) / interpolation_audit_ref_increment.red)
            : 0.0f;
        const float n_g = interpolation_audit_ref_increment.green > 1e-12f
            ? std::max(0.0f, (print_density.green - interpolation_audit_dmin.green) / interpolation_audit_ref_increment.green)
            : 0.0f;
        const float n_b = interpolation_audit_ref_increment.blue > 1e-12f
            ? std::max(0.0f, (print_density.blue - interpolation_audit_dmin.blue) / interpolation_audit_ref_increment.blue)
            : 0.0f;

        Vector3 amplitudes = {{
            interpolation_audit_reference_amplitude.red * monotone_cubic_sample(interpolation_audit_growth_c, n_r),
            interpolation_audit_reference_amplitude.green * monotone_cubic_sample(interpolation_audit_growth_m, n_g),
            interpolation_audit_reference_amplitude.blue * monotone_cubic_sample(interpolation_audit_growth_y, n_b)
        }};
        for (float& value : amplitudes) {
            value = std::max(0.0f, value);
        }

        const PrintViewer::Result interpolation_audit_viewed = print_viewer.view(
            source_transmittance_from_amplitudes(
                print_stock, amplitudes,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm));

        const float target_x = status_a_audit_measured_visual_neutral_viewed.viewed_xy.x;
        const float target_y = status_a_audit_measured_visual_neutral_viewed.viewed_xy.y;
        const float dx = interpolation_audit_viewed.viewed_xy.x - target_x;
        const float dy = interpolation_audit_viewed.viewed_xy.y - target_y;
        const float xy_error = std::sqrt(dx * dx + dy * dy);
        const float relative_y_error = linear_reference_viewed.viewed_xyz.y > 1e-12f
            ? std::abs(interpolation_audit_viewed.viewed_xyz.y - linear_reference_viewed.viewed_xyz.y)
                / linear_reference_viewed.viewed_xyz.y
            : 0.0f;

        const PrintViewer::xy viewing_white_xy = print_viewer.viewing_white_xy();
        const float white_dx = interpolation_audit_viewed.viewed_xy.x - viewing_white_xy.x;
        const float white_dy = interpolation_audit_viewed.viewed_xy.y - viewing_white_xy.y;
        const float delta_from_white = std::sqrt(white_dx * white_dx + white_dy * white_dy);

        const float ap0_min = std::min(interpolation_audit_viewed.aces2065_1[0],
            std::min(interpolation_audit_viewed.aces2065_1[1], interpolation_audit_viewed.aces2065_1[2]));
        const float ap0_max = std::max(interpolation_audit_viewed.aces2065_1[0],
            std::max(interpolation_audit_viewed.aces2065_1[1], interpolation_audit_viewed.aces2065_1[2]));
        const float ap0_mean = (interpolation_audit_viewed.aces2065_1[0]
            + interpolation_audit_viewed.aces2065_1[1]
            + interpolation_audit_viewed.aces2065_1[2]) / 3.0f;
        const float spread = ap0_mean > 1e-12f ? (ap0_max - ap0_min) / ap0_mean : 0.0f;

        interpolation_audit_dense_xy_error.x.push_back(stop); interpolation_audit_dense_xy_error.y.push_back(xy_error);
        interpolation_audit_dense_y_error.x.push_back(stop); interpolation_audit_dense_y_error.y.push_back(relative_y_error);
        interpolation_audit_dense_delta_from_white.x.push_back(stop); interpolation_audit_dense_delta_from_white.y.push_back(delta_from_white);
        interpolation_audit_dense_ap0_spread.x.push_back(stop); interpolation_audit_dense_ap0_spread.y.push_back(spread);
        interpolation_audit_dense_c.x.push_back(stop); interpolation_audit_dense_c.y.push_back(amplitudes[0]);
        interpolation_audit_dense_m.x.push_back(stop); interpolation_audit_dense_m.y.push_back(amplitudes[1]);
        interpolation_audit_dense_y.x.push_back(stop); interpolation_audit_dense_y.y.push_back(amplitudes[2]);

        Matrix3 xyz_jacobian = {{{{0.0f,0.0f,0.0f}},{{0.0f,0.0f,0.0f}},{{0.0f,0.0f,0.0f}}}};
        float crosstalk_xy[3] = {0.0f, 0.0f, 0.0f};
        for (int channel = 0; channel < 3; ++channel) {
            const float h = std::max(1e-4f, 0.05f * std::max(0.05f, amplitudes[channel]));
            Vector3 plus = amplitudes;
            Vector3 minus = amplitudes;
            plus[channel] += h;
            minus[channel] = std::max(0.0f, minus[channel] - h);
            const float denominator = plus[channel] - minus[channel];
            const PrintViewer::Result plus_viewed = print_viewer.view(
                source_transmittance_from_amplitudes(
                    print_stock, plus,
                    print_viewer.settings().wavelength_min_nm,
                    print_viewer.settings().wavelength_max_nm,
                    print_viewer.settings().wavelength_step_nm));
            const PrintViewer::Result minus_viewed = print_viewer.view(
                source_transmittance_from_amplitudes(
                    print_stock, minus,
                    print_viewer.settings().wavelength_min_nm,
                    print_viewer.settings().wavelength_max_nm,
                    print_viewer.settings().wavelength_step_nm));

            if (denominator > 0.0f) {
                xyz_jacobian[0][channel] =
                    (plus_viewed.viewed_xyz.x - minus_viewed.viewed_xyz.x) / denominator;
                xyz_jacobian[1][channel] =
                    (plus_viewed.viewed_xyz.y - minus_viewed.viewed_xyz.y) / denominator;
                xyz_jacobian[2][channel] =
                    (plus_viewed.viewed_xyz.z - minus_viewed.viewed_xyz.z) / denominator;
            }

            const float perturb_dx = plus_viewed.viewed_xy.x - minus_viewed.viewed_xy.x;
            const float perturb_dy = plus_viewed.viewed_xy.y - minus_viewed.viewed_xy.y;
            crosstalk_xy[channel] = std::sqrt(
                perturb_dx * perturb_dx + perturb_dy * perturb_dy);
        }

        interpolation_audit_crosstalk_c.x.push_back(stop); interpolation_audit_crosstalk_c.y.push_back(crosstalk_xy[0]);
        interpolation_audit_crosstalk_m.x.push_back(stop); interpolation_audit_crosstalk_m.y.push_back(crosstalk_xy[1]);
        interpolation_audit_crosstalk_y.x.push_back(stop); interpolation_audit_crosstalk_y.y.push_back(crosstalk_xy[2]);
        interpolation_audit_xyz_condition.x.push_back(stop); interpolation_audit_xyz_condition.y.push_back(matrix_frobenius_condition(xyz_jacobian));
    }

    const float identity_min = std::min(
        interpolation_audit_growth_c.x.front(), std::min(interpolation_audit_growth_m.x.front(), interpolation_audit_growth_y.x.front()));
    const float identity_max = std::max(
        interpolation_audit_growth_c.x.back(), std::max(interpolation_audit_growth_m.x.back(), interpolation_audit_growth_y.x.back()));
    for (int i = 0; i <= 80; ++i) {
        const float x = identity_min + (identity_max - identity_min) * static_cast<float>(i) / 80.0f;
        interpolation_audit_identity.x.push_back(x);
        interpolation_audit_identity.y.push_back(x);
    }

    auto max_curve_value = [](const SampledCurve& curve) {
        float result = 0.0f;
        for (float value : curve.y) {
            if (std::isfinite(value)) {
                result = std::max(result, std::abs(value));
            }
        }
        return result;
    };
    auto monotonic_nonincreasing = [](const SampledCurve& curve) {
        for (std::size_t i = 1; i < curve.y.size(); ++i) {
            if (curve.y[i] > curve.y[i - 1] + 1e-5f) {
                return false;
            }
        }
        return true;
    };

    std::cout << "info: interpolation robustness audit neutral-growth robustness/interpolation audit" << std::endl;
    std::cout << "info:   mapping: monotone cubic Hermite, normalized LINEAR REFERENCE record-growth -> normalized NEUTRAL INVERSE dye amplitude" << std::endl;
    std::cout << "info:   reference anchor: normalized 1 -> 1 at D55 colorimetric reference calibration colorimetric neutral" << std::endl;
    std::cout << "info:   dense audit: 81 neutral points from -4..+4 stops" << std::endl;
    std::cout << "info:   max dense xy error from measured Visual Neutral: " << max_curve_value(interpolation_audit_dense_xy_error) << std::endl;
    std::cout << "info:   max dense relative Y error vs linear C/M/Y reference model tone: " << max_curve_value(interpolation_audit_dense_y_error) << std::endl;
    std::cout << "info:   fitted amplitude monotonic C/M/Y: "
              << (monotonic_nonincreasing(interpolation_audit_dense_c) ? "yes" : "NO") << " / "
              << (monotonic_nonincreasing(interpolation_audit_dense_m) ? "yes" : "NO") << " / "
              << (monotonic_nonincreasing(interpolation_audit_dense_y) ? "yes" : "NO") << std::endl;
    std::cout << "info:   max local XYZ-basis Frobenius condition number: " << max_curve_value(interpolation_audit_xyz_condition) << std::endl;
    std::cout << "info:   off-neutral audit: +/-5% independent C/M/Y perturbations retained through full spectral viewer" << std::endl;
    std::cout << "info:   NOTE: INTERPOLATION AUDIT is a calibration sufficiency audit, not measured multi-density Kodak dye physics; active rendering remains linear C/M/Y reference model" << std::endl;

    {
        PlotOptions options;
        options.title = "KODAK 2383 INTERPOLATION AUDIT NORMALIZED DYE-GROWTH MAPPING";
        options.subtitle = "LINEAR REFERENCE NORMALIZED RECORD GROWTH -> NEUTRAL INVERSE NEUTRAL-PRESERVING DYE AMPLITUDE";
        options.x_label = "NORMALIZED LINEAR REFERENCE RECORD-GROWTH INPUT";
        options.y_label = "NORMALIZED DYE AMPLITUDE";
        options.include_zero_y = true;
        const std::vector<Series> series = {
            {"IDENTITY LINEAR REFERENCE", &interpolation_audit_identity, {{0.65f,0.65f,0.65f}}, true, false, 1},
            {"CYAN MAPPING", &interpolation_audit_growth_c, {{0.25f,1.0f,1.0f}}, true, true, 3},
            {"MAGENTA MAPPING", &interpolation_audit_growth_m, {{1.0f,0.25f,0.85f}}, true, true, 3},
            {"YELLOW MAPPING", &interpolation_audit_growth_y, {{1.0f,0.90f,0.20f}}, true, true, 3}
        };
        success = write_plot(stem + "_2383_interpolation_audit_normalized_dye_growth_mapping.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 INTERPOLATION AUDIT DENSE INTERPOLATION ERROR";
        options.subtitle = "81-POINT AUDIT BETWEEN NEUTRAL INVERSE ANCHORS  LOWER IS BETTER";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "ERROR";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"DELTA XY TO TARGET", &interpolation_audit_dense_xy_error, {{1.0f,0.45f,0.25f}}, true, false, 2},
            {"RELATIVE Y ERROR", &interpolation_audit_dense_y_error, {{0.35f,1.0f,0.45f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_interpolation_audit_dense_interpolation_error.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 INTERPOLATION AUDIT OFF-NEUTRAL DYE RESPONSE";
        options.subtitle = "CIE XY DISPLACEMENT FOR +/-5% INDEPENDENT DYE PERTURBATION";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "FULL-SPAN DELTA XY";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"CYAN", &interpolation_audit_crosstalk_c, {{0.25f,1.0f,1.0f}}, true, false, 2},
            {"MAGENTA", &interpolation_audit_crosstalk_m, {{1.0f,0.25f,0.85f}}, true, false, 2},
            {"YELLOW", &interpolation_audit_crosstalk_y, {{1.0f,0.90f,0.20f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_interpolation_audit_offneutral_dye_response.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 INTERPOLATION AUDIT LOCAL XYZ BASIS CONDITION";
        options.subtitle = "FROBENIUS CONDITION NUMBER OF D XYZ / D C,M,Y  LOWER IS BETTER";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "CONDITION NUMBER";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"XYZ BASIS CONDITION", &interpolation_audit_xyz_condition, {{0.35f,0.60f,1.0f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_interpolation_audit_local_xyz_basis_condition.png", series, options) && success;
    }

    // ------------------------------------------------------------------
    // dense calibration mapping: dense neutral-preserving calibration table.
    //
    // INTERPOLATION AUDIT showed that five anchors are insufficient.  Here the same NEUTRAL INVERSE
    // nonlinear XYZ inverse is solved densely, converted to channel-local
    // normalized record-growth -> dye-amplitude mappings, and then audited
    // against independent direct nonlinear solves on a finer grid.
    // Diagnostic only: active rendering remains linear C/M/Y reference model.
    // ------------------------------------------------------------------
    SampledCurve dense_mapping_map_c;
    SampledCurve dense_mapping_map_m;
    SampledCurve dense_mapping_map_y;
    SampledCurve dense_mapping_lut_xy_error;
    SampledCurve dense_mapping_lut_y_error;
    SampledCurve dense_mapping_lut_amplitude_error;
    SampledCurve dense_mapping_direct_xy_error;
    SampledCurve dense_mapping_direct_y_error;
    SampledCurve dense_mapping_cal_c;
    SampledCurve dense_mapping_cal_m;
    SampledCurve dense_mapping_cal_y;
    int dense_mapping_calibration_failures = 0;
    int dense_mapping_validation_failures = 0;

    auto dense_mapping_state_at_stop = [&](float stop,
                                 FilmDensity& density,
                                 Vector3& linear_amplitudes,
                                 PrintViewer::Result& linear_reference_result) {
        const float negative_log_exposure =
            negative_dye_model.diagnostics().calibration_log_exposure
            + stop * log10_2;
        const SampledCurve negative_transmittance =
            negative_dye_model.synthesize_neutral_transmittance(
                negative_stock, negative_log_exposure);
        density = print_processor.process(negative_transmittance);
        linear_reference_result = print_viewer.view(
            print_dye_model.synthesize_transmittance_linear_reference(density));

        const auto& diag = print_dye_model.diagnostics();
        const FilmDensity& dmin = diag.minimum_record_density;
        const FilmDensity& ref_inc = diag.reference_record_increment;
        const FilmDensity& ref_amp = diag.calibrated_reference_amplitude;
        const float nr = ref_inc.red > 1e-12f
            ? std::max(0.0f, (density.red - dmin.red) / ref_inc.red) : 0.0f;
        const float ng = ref_inc.green > 1e-12f
            ? std::max(0.0f, (density.green - dmin.green) / ref_inc.green) : 0.0f;
        const float nb = ref_inc.blue > 1e-12f
            ? std::max(0.0f, (density.blue - dmin.blue) / ref_inc.blue) : 0.0f;
        linear_amplitudes = {{ref_amp.red * nr, ref_amp.green * ng, ref_amp.blue * nb}};
    };

    auto dense_mapping_solve_neutral = [&](const PrintViewer::Result& linear_reference_result,
                                 const Vector3& initial,
                                 Vector3& amplitudes,
                                 float& xy_error,
                                 float& relative_y_error) {
        const float target_x = status_a_audit_measured_visual_neutral_viewed.viewed_xy.x;
        const float target_yc = status_a_audit_measured_visual_neutral_viewed.viewed_xy.y;
        const float target_Y = linear_reference_result.viewed_xyz.y;
        const float target_zc = std::max(0.0f, 1.0f - target_x - target_yc);
        PrintViewer::XYZ target_xyz;
        if (target_yc <= 1e-12f) {
            return false;
        }
        target_xyz.x = target_Y * target_x / target_yc;
        target_xyz.y = target_Y;
        target_xyz.z = target_Y * target_zc / target_yc;

        amplitudes = initial;
        bool ok = true;
        for (int iteration = 0; iteration < 20; ++iteration) {
            const PrintViewer::Result current = print_viewer.view(
                source_transmittance_from_amplitudes(
                    print_stock, amplitudes,
                    print_viewer.settings().wavelength_min_nm,
                    print_viewer.settings().wavelength_max_nm,
                    print_viewer.settings().wavelength_step_nm));
            Vector3 error = {{
                target_xyz.x - current.viewed_xyz.x,
                target_xyz.y - current.viewed_xyz.y,
                target_xyz.z - current.viewed_xyz.z
            }};
            const float max_error = std::max(std::abs(error[0]),
                std::max(std::abs(error[1]), std::abs(error[2])));
            if (max_error < 5e-8f) {
                break;
            }

            Matrix3 jacobian = {{{{0.0f,0.0f,0.0f}},{{0.0f,0.0f,0.0f}},{{0.0f,0.0f,0.0f}}}};
            for (int column = 0; column < 3; ++column) {
                const float h = std::max(1e-4f, 1e-3f * std::max(1.0f, amplitudes[column]));
                Vector3 plus = amplitudes;
                Vector3 minus = amplitudes;
                plus[column] += h;
                minus[column] = std::max(0.0f, minus[column] - h);
                const float denom = plus[column] - minus[column];
                if (denom <= 0.0f) { ok = false; break; }
                const PrintViewer::Result pv = print_viewer.view(
                    source_transmittance_from_amplitudes(print_stock, plus,
                        print_viewer.settings().wavelength_min_nm,
                        print_viewer.settings().wavelength_max_nm,
                        print_viewer.settings().wavelength_step_nm));
                const PrintViewer::Result mv = print_viewer.view(
                    source_transmittance_from_amplitudes(print_stock, minus,
                        print_viewer.settings().wavelength_min_nm,
                        print_viewer.settings().wavelength_max_nm,
                        print_viewer.settings().wavelength_step_nm));
                jacobian[0][column] = (pv.viewed_xyz.x - mv.viewed_xyz.x) / denom;
                jacobian[1][column] = (pv.viewed_xyz.y - mv.viewed_xyz.y) / denom;
                jacobian[2][column] = (pv.viewed_xyz.z - mv.viewed_xyz.z) / denom;
            }
            if (!ok) break;
            Vector3 delta = {{0.0f,0.0f,0.0f}};
            if (!solve_3x3(jacobian, error, delta)) { ok = false; break; }
            float scale = 1.0f;
            for (int c = 0; c < 3; ++c) {
                if (delta[c] < 0.0f && amplitudes[c] + delta[c] < 0.0f) {
                    scale = std::min(scale, 0.8f * amplitudes[c] / std::max(1e-12f, -delta[c]));
                }
            }
            scale = std::max(0.05f, std::min(1.0f, scale));
            for (int c = 0; c < 3; ++c) {
                amplitudes[c] = std::max(0.0f, amplitudes[c] + scale * delta[c]);
            }
        }

        const PrintViewer::Result solved = print_viewer.view(
            source_transmittance_from_amplitudes(print_stock, amplitudes,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm));
        const float dx = solved.viewed_xy.x - target_x;
        const float dy = solved.viewed_xy.y - target_yc;
        xy_error = std::sqrt(dx * dx + dy * dy);
        relative_y_error = target_Y > 1e-12f
            ? std::abs(solved.viewed_xyz.y - target_Y) / target_Y : 0.0f;
        return ok && xy_error < 1e-5f && relative_y_error < 1e-5f;
    };

    const FilmDensity& dense_mapping_ref_amp = print_dye_model.diagnostics().calibrated_reference_amplitude;

    // 81 direct calibration solves, one every 0.1 stop.
    for (int i = 0; i <= 80; ++i) {
        const float stop = -4.0f + 8.0f * static_cast<float>(i) / 80.0f;
        FilmDensity density;
        Vector3 linear = {{0.0f,0.0f,0.0f}};
        PrintViewer::Result linear_reference_result;
        dense_mapping_state_at_stop(stop, density, linear, linear_reference_result);
        Vector3 solved = linear;
        float xy_err = 0.0f, y_err = 0.0f;
        if (!dense_mapping_solve_neutral(linear_reference_result, linear, solved, xy_err, y_err)) {
            ++dense_mapping_calibration_failures;
        }

        const float nx[3] = {
            dense_mapping_ref_amp.red > 1e-12f ? linear[0] / dense_mapping_ref_amp.red : 0.0f,
            dense_mapping_ref_amp.green > 1e-12f ? linear[1] / dense_mapping_ref_amp.green : 0.0f,
            dense_mapping_ref_amp.blue > 1e-12f ? linear[2] / dense_mapping_ref_amp.blue : 0.0f
        };
        const float ny[3] = {
            dense_mapping_ref_amp.red > 1e-12f ? solved[0] / dense_mapping_ref_amp.red : 0.0f,
            dense_mapping_ref_amp.green > 1e-12f ? solved[1] / dense_mapping_ref_amp.green : 0.0f,
            dense_mapping_ref_amp.blue > 1e-12f ? solved[2] / dense_mapping_ref_amp.blue : 0.0f
        };
        dense_mapping_map_c.x.push_back(nx[0]); dense_mapping_map_c.y.push_back(ny[0]);
        dense_mapping_map_m.x.push_back(nx[1]); dense_mapping_map_m.y.push_back(ny[1]);
        dense_mapping_map_y.x.push_back(nx[2]); dense_mapping_map_y.y.push_back(ny[2]);
        dense_mapping_cal_c.x.push_back(stop); dense_mapping_cal_c.y.push_back(solved[0]);
        dense_mapping_cal_m.x.push_back(stop); dense_mapping_cal_m.y.push_back(solved[1]);
        dense_mapping_cal_y.x.push_back(stop); dense_mapping_cal_y.y.push_back(solved[2]);
    }

    auto sort_mapping = [](SampledCurve& curve) {
        std::vector<std::pair<float,float>> pairs;
        for (std::size_t i = 0; i < curve.x.size(); ++i) pairs.push_back({curve.x[i], curve.y[i]});
        std::sort(pairs.begin(), pairs.end(), [](const std::pair<float,float>& a, const std::pair<float,float>& b) {
            return a.first < b.first;
        });
        curve.x.clear(); curve.y.clear();
        for (const auto& p : pairs) { curve.x.push_back(p.first); curve.y.push_back(p.second); }
    };
    sort_mapping(dense_mapping_map_c); sort_mapping(dense_mapping_map_m); sort_mapping(dense_mapping_map_y);

    // Independent 321-point validation.  At each point compare the dense
    // mapping to a fresh direct nonlinear NEUTRAL INVERSE-style solve.
    for (int i = 0; i <= 320; ++i) {
        const float stop = -4.0f + 8.0f * static_cast<float>(i) / 320.0f;
        FilmDensity density;
        Vector3 linear = {{0.0f,0.0f,0.0f}};
        PrintViewer::Result linear_reference_result;
        dense_mapping_state_at_stop(stop, density, linear, linear_reference_result);

        Vector3 direct = linear;
        float direct_xy_err = 0.0f, direct_y_err = 0.0f;
        if (!dense_mapping_solve_neutral(linear_reference_result, linear, direct, direct_xy_err, direct_y_err)) {
            ++dense_mapping_validation_failures;
        }

        const float nr = dense_mapping_ref_amp.red > 1e-12f ? linear[0] / dense_mapping_ref_amp.red : 0.0f;
        const float ng = dense_mapping_ref_amp.green > 1e-12f ? linear[1] / dense_mapping_ref_amp.green : 0.0f;
        const float nb = dense_mapping_ref_amp.blue > 1e-12f ? linear[2] / dense_mapping_ref_amp.blue : 0.0f;
        Vector3 lut = {{
            dense_mapping_ref_amp.red * monotone_cubic_sample(dense_mapping_map_c, nr),
            dense_mapping_ref_amp.green * monotone_cubic_sample(dense_mapping_map_m, ng),
            dense_mapping_ref_amp.blue * monotone_cubic_sample(dense_mapping_map_y, nb)
        }};
        for (float& v : lut) v = std::max(0.0f, v);

        const PrintViewer::Result lut_viewed = print_viewer.view(
            source_transmittance_from_amplitudes(print_stock, lut,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm));
        const PrintViewer::Result direct_viewed = print_viewer.view(
            source_transmittance_from_amplitudes(print_stock, direct,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm));

        const float dx = lut_viewed.viewed_xy.x - direct_viewed.viewed_xy.x;
        const float dy = lut_viewed.viewed_xy.y - direct_viewed.viewed_xy.y;
        const float lut_xy_err = std::sqrt(dx * dx + dy * dy);
        const float lut_y_err = direct_viewed.viewed_xyz.y > 1e-12f
            ? std::abs(lut_viewed.viewed_xyz.y - direct_viewed.viewed_xyz.y) / direct_viewed.viewed_xyz.y
            : 0.0f;
        float amp_err = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float denom = std::max(1e-6f, std::abs(direct[c]));
            amp_err = std::max(amp_err, std::abs(lut[c] - direct[c]) / denom);
        }

        dense_mapping_lut_xy_error.x.push_back(stop); dense_mapping_lut_xy_error.y.push_back(lut_xy_err);
        dense_mapping_lut_y_error.x.push_back(stop); dense_mapping_lut_y_error.y.push_back(lut_y_err);
        dense_mapping_lut_amplitude_error.x.push_back(stop); dense_mapping_lut_amplitude_error.y.push_back(amp_err);
        dense_mapping_direct_xy_error.x.push_back(stop); dense_mapping_direct_xy_error.y.push_back(direct_xy_err);
        dense_mapping_direct_y_error.x.push_back(stop); dense_mapping_direct_y_error.y.push_back(direct_y_err);
    }

    auto dense_mapping_max = [](const SampledCurve& curve) {
        float result = 0.0f;
        for (float v : curve.y) if (std::isfinite(v)) result = std::max(result, std::abs(v));
        return result;
    };
    auto dense_mapping_mapping_monotone = [](const SampledCurve& curve) {
        for (std::size_t i = 1; i < curve.y.size(); ++i) {
            if (curve.y[i] + 1e-5f < curve.y[i - 1]) return false;
        }
        return true;
    };

    std::cout << "info: dense calibration mapping dense neutral-preserving calibration/LUT audit" << std::endl;
    std::cout << "info:   direct calibration solves: 81 points (-4..+4 stops, 0.1-stop spacing)" << std::endl;
    std::cout << "info:   independent validation solves: 321 points (-4..+4 stops, 0.025-stop spacing)" << std::endl;
    std::cout << "info:   calibration solve failures: " << dense_mapping_calibration_failures << std::endl;
    std::cout << "info:   validation solve failures: " << dense_mapping_validation_failures << std::endl;
    std::cout << "info:   max LUT-vs-direct delta xy: " << dense_mapping_max(dense_mapping_lut_xy_error) << std::endl;
    std::cout << "info:   max LUT-vs-direct relative Y error: " << dense_mapping_max(dense_mapping_lut_y_error) << std::endl;
    std::cout << "info:   max LUT-vs-direct relative dye-amplitude error: " << dense_mapping_max(dense_mapping_lut_amplitude_error) << std::endl;
    std::cout << "info:   max direct-solver target delta xy: " << dense_mapping_max(dense_mapping_direct_xy_error) << std::endl;
    std::cout << "info:   max direct-solver relative Y error: " << dense_mapping_max(dense_mapping_direct_y_error) << std::endl;
    std::cout << "info:   dense mapping monotonic C/M/Y: "
              << (dense_mapping_mapping_monotone(dense_mapping_map_c) ? "yes" : "NO") << " / "
              << (dense_mapping_mapping_monotone(dense_mapping_map_m) ? "yes" : "NO") << " / "
              << (dense_mapping_mapping_monotone(dense_mapping_map_y) ? "yes" : "NO") << std::endl;
    std::cout << "info:   NOTE: DENSE MAPPING remains calibration-derived, not measured multi-density Kodak dye physics; active rendering remains linear C/M/Y reference model" << std::endl;

    {
        PlotOptions options;
        options.title = "KODAK 2383 DENSE MAPPING DENSE CALIBRATION MAPPINGS";
        options.subtitle = "81 DIRECT NEUTRAL-PRESERVING SOLVES  NORMALIZED RECORD GROWTH -> DYE AMPLITUDE";
        options.x_label = "NORMALIZED LINEAR REFERENCE RECORD-GROWTH INPUT";
        options.y_label = "NORMALIZED DYE AMPLITUDE";
        options.include_zero_y = true;
        const std::vector<Series> series = {
            {"CYAN", &dense_mapping_map_c, {{0.25f,1.0f,1.0f}}, true, false, 2},
            {"MAGENTA", &dense_mapping_map_m, {{1.0f,0.25f,0.85f}}, true, false, 2},
            {"YELLOW", &dense_mapping_map_y, {{1.0f,0.90f,0.20f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_dense_mapping_dense_growth_mapping.png", series, options) && success;
    }
    {
        PlotOptions options;
        options.title = "KODAK 2383 DENSE MAPPING LUT VS DIRECT SOLVER ERROR";
        options.subtitle = "321 INDEPENDENT DIRECT SOLVES  LOWER IS BETTER";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "ERROR";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"DELTA XY", &dense_mapping_lut_xy_error, {{1.0f,0.45f,0.25f}}, true, false, 2},
            {"RELATIVE Y", &dense_mapping_lut_y_error, {{0.35f,1.0f,0.45f}}, true, false, 2},
            {"RELATIVE AMPLITUDE", &dense_mapping_lut_amplitude_error, {{0.35f,0.60f,1.0f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_dense_mapping_lut_vs_direct_error.png", series, options) && success;
    }
    {
        PlotOptions options;
        options.title = "KODAK 2383 DENSE MAPPING DIRECT SOLVER RESIDUAL";
        options.subtitle = "TARGET VISUAL-NEUTRAL XY + LINEAR REFERENCE Y  NUMERICAL SOLVE QUALITY";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "RESIDUAL";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"DELTA XY TO TARGET", &dense_mapping_direct_xy_error, {{1.0f,0.45f,0.25f}}, true, false, 2},
            {"RELATIVE Y ERROR", &dense_mapping_direct_y_error, {{0.35f,1.0f,0.45f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_dense_mapping_direct_solver_residual.png", series, options) && success;
    }

    // ------------------------------------------------------------------
    // production-mapping qualification: production-mapping qualification.
    //
    // DENSE MAPPING established that the direct neutral-preserving inverse is stable,
    // but its 81-point monotone-cubic representation narrowly missed the
    // desired 1e-4 delta-xy target.  Compare two cheap interpolation methods
    // at two calibration densities against a much denser direct-solver grid.
    // Diagnostic only: active rendering remains linear C/M/Y reference model.
    // ------------------------------------------------------------------
    SampledCurve mapping_qualification_map161_c;
    SampledCurve mapping_qualification_map161_m;
    SampledCurve mapping_qualification_map161_y;
    int mapping_qualification_calibration161_failures = 0;
    int mapping_qualification_validation_failures = 0;

    for (int i = 0; i <= 160; ++i) {
        const float stop = -4.0f + 8.0f * static_cast<float>(i) / 160.0f;
        FilmDensity density;
        Vector3 linear = {{0.0f,0.0f,0.0f}};
        PrintViewer::Result linear_reference_result;
        dense_mapping_state_at_stop(stop, density, linear, linear_reference_result);
        Vector3 solved = linear;
        float xy_err = 0.0f, y_err = 0.0f;
        if (!dense_mapping_solve_neutral(linear_reference_result, linear, solved, xy_err, y_err)) {
            ++mapping_qualification_calibration161_failures;
        }
        const float nx[3] = {
            dense_mapping_ref_amp.red > 1e-12f ? linear[0] / dense_mapping_ref_amp.red : 0.0f,
            dense_mapping_ref_amp.green > 1e-12f ? linear[1] / dense_mapping_ref_amp.green : 0.0f,
            dense_mapping_ref_amp.blue > 1e-12f ? linear[2] / dense_mapping_ref_amp.blue : 0.0f
        };
        const float ny[3] = {
            dense_mapping_ref_amp.red > 1e-12f ? solved[0] / dense_mapping_ref_amp.red : 0.0f,
            dense_mapping_ref_amp.green > 1e-12f ? solved[1] / dense_mapping_ref_amp.green : 0.0f,
            dense_mapping_ref_amp.blue > 1e-12f ? solved[2] / dense_mapping_ref_amp.blue : 0.0f
        };
        mapping_qualification_map161_c.x.push_back(nx[0]); mapping_qualification_map161_c.y.push_back(ny[0]);
        mapping_qualification_map161_m.x.push_back(nx[1]); mapping_qualification_map161_m.y.push_back(ny[1]);
        mapping_qualification_map161_y.x.push_back(nx[2]); mapping_qualification_map161_y.y.push_back(ny[2]);
    }
    sort_mapping(mapping_qualification_map161_c); sort_mapping(mapping_qualification_map161_m); sort_mapping(mapping_qualification_map161_y);

    struct MappingMethodErrors {
        explicit MappingMethodErrors(const char* method_name)
            : name(method_name)
        {
        }
        const char* name;
        SampledCurve xy;
        SampledCurve y;
        SampledCurve amplitude;
        float max_xy = 0.0f;
        float max_y = 0.0f;
        float max_amplitude = 0.0f;
        float max_xy_stop = 0.0f;
    };
    MappingMethodErrors mapping_qualification_linear81("81 LINEAR");
    MappingMethodErrors mapping_qualification_cubic81("81 MONOTONE CUBIC");
    MappingMethodErrors mapping_qualification_linear161("161 LINEAR");
    MappingMethodErrors mapping_qualification_cubic161("161 MONOTONE CUBIC");

    auto mapping_qualification_sample_linear = [](const SampledCurve& curve, float x) {
        if (!curve.valid()) return 0.0f;
        if (x <= curve.x.front()) return curve.y.front();
        if (x >= curve.x.back()) return curve.y.back();
        return curve.sample(x, curve.y.front());
    };

    auto mapping_qualification_eval_method = [&](MappingMethodErrors& errors,
                               const SampledCurve& mc,
                               const SampledCurve& mm,
                               const SampledCurve& my,
                               bool cubic,
                               float stop,
                               const Vector3& linear,
                               const Vector3& direct,
                               const PrintViewer::Result& direct_viewed) {
        const float nr = dense_mapping_ref_amp.red > 1e-12f ? linear[0] / dense_mapping_ref_amp.red : 0.0f;
        const float ng = dense_mapping_ref_amp.green > 1e-12f ? linear[1] / dense_mapping_ref_amp.green : 0.0f;
        const float nb = dense_mapping_ref_amp.blue > 1e-12f ? linear[2] / dense_mapping_ref_amp.blue : 0.0f;
        const auto sample = [&](const SampledCurve& curve, float x) {
            return cubic ? monotone_cubic_sample(curve, x) : mapping_qualification_sample_linear(curve, x);
        };
        Vector3 lut = {{
            dense_mapping_ref_amp.red * sample(mc, nr),
            dense_mapping_ref_amp.green * sample(mm, ng),
            dense_mapping_ref_amp.blue * sample(my, nb)
        }};
        for (float& v : lut) v = std::max(0.0f, v);
        const PrintViewer::Result viewed = print_viewer.view(
            source_transmittance_from_amplitudes(print_stock, lut,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm));
        const float dx = viewed.viewed_xy.x - direct_viewed.viewed_xy.x;
        const float dy = viewed.viewed_xy.y - direct_viewed.viewed_xy.y;
        const float xy_err = std::sqrt(dx * dx + dy * dy);
        const float y_err = direct_viewed.viewed_xyz.y > 1e-12f
            ? std::abs(viewed.viewed_xyz.y - direct_viewed.viewed_xyz.y) / direct_viewed.viewed_xyz.y
            : 0.0f;
        float amp_err = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float denom = std::max(1e-6f, std::abs(direct[c]));
            amp_err = std::max(amp_err, std::abs(lut[c] - direct[c]) / denom);
        }
        errors.xy.x.push_back(stop); errors.xy.y.push_back(xy_err);
        errors.y.x.push_back(stop); errors.y.y.push_back(y_err);
        errors.amplitude.x.push_back(stop); errors.amplitude.y.push_back(amp_err);
        if (xy_err > errors.max_xy) { errors.max_xy = xy_err; errors.max_xy_stop = stop; }
        errors.max_y = std::max(errors.max_y, y_err);
        errors.max_amplitude = std::max(errors.max_amplitude, amp_err);
    };

    // 1281 independent direct solves: 0.00625-stop spacing.  This grid is
    // intentionally offset-rich relative to both calibration tables, so the
    // maxima expose interpolation behavior rather than simply re-hitting
    // calibration samples.
    for (int i = 0; i <= 1280; ++i) {
        const float stop = -4.0f + 8.0f * static_cast<float>(i) / 1280.0f;
        FilmDensity density;
        Vector3 linear = {{0.0f,0.0f,0.0f}};
        PrintViewer::Result linear_reference_result;
        dense_mapping_state_at_stop(stop, density, linear, linear_reference_result);
        Vector3 direct = linear;
        float direct_xy_err = 0.0f, direct_y_err = 0.0f;
        if (!dense_mapping_solve_neutral(linear_reference_result, linear, direct, direct_xy_err, direct_y_err)) {
            ++mapping_qualification_validation_failures;
        }
        const PrintViewer::Result direct_viewed = print_viewer.view(
            source_transmittance_from_amplitudes(print_stock, direct,
                print_viewer.settings().wavelength_min_nm,
                print_viewer.settings().wavelength_max_nm,
                print_viewer.settings().wavelength_step_nm));

        mapping_qualification_eval_method(mapping_qualification_linear81, dense_mapping_map_c, dense_mapping_map_m, dense_mapping_map_y, false,
            stop, linear, direct, direct_viewed);
        mapping_qualification_eval_method(mapping_qualification_cubic81, dense_mapping_map_c, dense_mapping_map_m, dense_mapping_map_y, true,
            stop, linear, direct, direct_viewed);
        mapping_qualification_eval_method(mapping_qualification_linear161, mapping_qualification_map161_c, mapping_qualification_map161_m, mapping_qualification_map161_y, false,
            stop, linear, direct, direct_viewed);
        mapping_qualification_eval_method(mapping_qualification_cubic161, mapping_qualification_map161_c, mapping_qualification_map161_m, mapping_qualification_map161_y, true,
            stop, linear, direct, direct_viewed);
    }

    const MappingMethodErrors* mapping_qualification_methods[] = {
        &mapping_qualification_linear81, &mapping_qualification_cubic81, &mapping_qualification_linear161, &mapping_qualification_cubic161
    };
    const MappingMethodErrors* mapping_qualification_selected = nullptr;
    for (const MappingMethodErrors* method : mapping_qualification_methods) {
        if (method->max_xy <= 1e-4f && method->max_y <= 1e-4f) {
            mapping_qualification_selected = method;
            break;
        }
    }
    if (!mapping_qualification_selected) {
        mapping_qualification_selected = &mapping_qualification_cubic161;
        for (const MappingMethodErrors* method : mapping_qualification_methods) {
            if (method->max_xy < mapping_qualification_selected->max_xy) mapping_qualification_selected = method;
        }
    }

    std::cout << "info: production-mapping qualification production-mapping qualification" << std::endl;
    std::cout << "info:   161-point calibration solve failures: " << mapping_qualification_calibration161_failures << std::endl;
    std::cout << "info:   1281-point validation solve failures: " << mapping_qualification_validation_failures << std::endl;
    for (const MappingMethodErrors* method : mapping_qualification_methods) {
        std::cout << "info:   " << method->name
                  << " max delta xy=" << method->max_xy
                  << " at " << method->max_xy_stop << " stops"
                  << "  max rel Y=" << method->max_y
                  << "  max rel amplitude=" << method->max_amplitude
                  << ((method->max_xy <= 1e-4f && method->max_y <= 1e-4f) ? "  PASS" : "  FAIL")
                  << std::endl;
    }
    std::cout << "info:   selected qualification candidate: " << mapping_qualification_selected->name << std::endl;
    std::cout << "info:   NOTE: MAPPING QUALIFICATION qualification is retained as reference; active rendering is nonlinear dye-growth model" << std::endl;

    {
        PlotOptions options;
        options.title = "KODAK 2383 MAPPING QUALIFICATION INTERPOLATION METHOD COMPARISON";
        options.subtitle = "1281 DIRECT-SOLVER REFERENCES  DELTA XY  LOWER IS BETTER";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "DELTA XY";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"81 LINEAR", &mapping_qualification_linear81.xy, {{0.85f,0.85f,0.85f}}, true, false, 2},
            {"81 MONOTONE CUBIC", &mapping_qualification_cubic81.xy, {{1.0f,0.45f,0.25f}}, true, false, 2},
            {"161 LINEAR", &mapping_qualification_linear161.xy, {{0.35f,1.0f,0.45f}}, true, false, 2},
            {"161 MONOTONE CUBIC", &mapping_qualification_cubic161.xy, {{0.35f,0.60f,1.0f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_mapping_qualification_interpolation_comparison.png", series, options) && success;
    }
    {
        PlotOptions options;
        options.title = "KODAK 2383 MAPPING QUALIFICATION SELECTED REPRESENTATION ERROR";
        options.subtitle = std::string(mapping_qualification_selected->name) + "  VS DIRECT NONLINEAR SOLVER";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "ERROR";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"DELTA XY", &mapping_qualification_selected->xy, {{1.0f,0.45f,0.25f}}, true, false, 2},
            {"RELATIVE Y", &mapping_qualification_selected->y, {{0.35f,1.0f,0.45f}}, true, false, 2},
            {"RELATIVE AMPLITUDE", &mapping_qualification_selected->amplitude, {{0.35f,0.60f,1.0f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_mapping_qualification_selected_representation_error.png", series, options) && success;
    }
    {
        PlotOptions options;
        options.title = "KODAK 2383 MAPPING QUALIFICATION 161-POINT CALIBRATION MAPPINGS";
        options.subtitle = "DENSE DIRECT NEUTRAL-PRESERVING SOLVES  NORMALIZED RECORD GROWTH -> DYE AMPLITUDE";
        options.x_label = "NORMALIZED LINEAR REFERENCE RECORD-GROWTH INPUT";
        options.y_label = "NORMALIZED DYE AMPLITUDE";
        options.include_zero_y = true;
        const std::vector<Series> series = {
            {"CYAN", &mapping_qualification_map161_c, {{0.25f,1.0f,1.0f}}, true, false, 2},
            {"MAGENTA", &mapping_qualification_map161_m, {{1.0f,0.25f,0.85f}}, true, false, 2},
            {"YELLOW", &mapping_qualification_map161_y, {{1.0f,0.90f,0.20f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_mapping_qualification_161_growth_mapping.png", series, options) && success;
    }

    // ------------------------------------------------------------------
    // Neutral ladder chromaticity before adaptation.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title =
            "KODAK 2383 VIEWED NEUTRAL LADDER CHROMATICITY";
        options.subtitle =
            "VERITA NEUTRAL -4 -2 0 +2 +4 STOPS  BEFORE D60 ADAPTATION";
        options.x_label =
            "NEGATIVE EXPOSURE STOPS";
        options.y_label =
            "CIE 1931 CHROMATICITY";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = false;

        const std::vector<Series> series = {
            {"x", &viewed_x, {{1.0f,0.45f,0.25f}}, true, true, 2},
            {"y", &viewed_y, {{0.35f,1.0f,0.35f}}, true, true, 2}
        };

        success =
            write_plot(
                stem
                    + "_2383_viewed_neutral_xy.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // Neutral ladder after viewing-white -> D60 adaptation and AP0 matrix.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title =
            "KODAK 2383 VIEWED NEUTRAL LADDER AP0";
        options.subtitle =
            "BRADFORD VIEWING WHITE -> D60  LINEAR ACES2065-1";
        options.x_label =
            "NEGATIVE EXPOSURE STOPS";
        options.y_label =
            "LINEAR AP0";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"AP0 R", &ap0_r, {{1.0f,0.25f,0.25f}}, true, true, 2},
            {"AP0 G", &ap0_g, {{0.25f,1.0f,0.25f}}, true, true, 2},
            {"AP0 B", &ap0_b, {{0.25f,0.55f,1.0f}}, true, true, 2}
        };

        success =
            write_plot(
                stem
                    + "_2383_viewed_neutral_ap0.png",
                series,
                options)
            && success;
    }


    // ------------------------------------------------------------------
    // neutral-drift diagnostic: developed print-record separation expressed in the
    // normalized increment coordinates used by PrintDyeModel.
    //
    // If R/G/B remain coincident here, the printer/sensitometric stage is
    // not the source of neutral drift. Separation here means the developed
    // records themselves are already moving away from the Kodak density-1
    // calibration ratio before spectral viewing.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title =
            "KODAK 2383 NEUTRAL RECORD INCREMENT DECOMPOSITION";
        options.subtitle =
            "(D - DMIN) / (DREF - DMIN)  REAL VERITA NEUTRAL LADDER";
        options.x_label =
            "NEGATIVE EXPOSURE STOPS";
        options.y_label =
            "NORMALIZED RECORD INCREMENT";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"RED RECORD", &normalized_increment_r, {{1.0f,0.25f,0.25f}}, true, true, 2},
            {"GREEN RECORD", &normalized_increment_g, {{0.25f,1.0f,0.25f}}, true, true, 2},
            {"BLUE RECORD", &normalized_increment_b, {{0.25f,0.55f,1.0f}}, true, true, 2},
            {"MEAN", &normalized_increment_mean, {{1.0f,1.0f,1.0f}}, true, true, 2}
        };

        success =
            write_plot(
                stem
                    + "_2383_neutral_record_increments.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // neutral-drift diagnostic: neutral chromaticity-drift attribution.
    //
    // The four curves progressively remove degrees of freedom from the real
    // pipeline without changing the active model:
    //
    //   REAL PIPELINE
    //       full printer + sensitometry + PrintDyeModel behavior
    //
    //   EQUAL STATUS-A
    //       removes developed R/G/B record-density separation
    //
    //   MATCHED DYE INCREMENT
    //       preserves the Kodak density-1 C/M/Y calibration ratio at all
    //       levels, leaving the model's fixed neutral residual/support term
    //
    //   SCALED KODAK VISUAL NEUTRAL
    //       scales the complete measured visual-neutral density shape,
    //       including residual/support; remaining drift belongs to the
    //       published neutral spectral shape under this viewing illuminant
    //
    // Distance is measured in the CIE xy plane from the actual viewing-white
    // chromaticity. This is a diagnostic metric, not a perceptual Delta E.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title =
            "KODAK 2383 NEUTRAL CHROMATICITY DRIFT DECOMPOSITION";
        options.subtitle =
            "CIE XY DISTANCE FROM D55 VIEWING WHITE  DIAGNOSTIC ONLY";
        options.x_label =
            "NEGATIVE EXPOSURE STOPS";
        options.y_label =
            "DELTA XY";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"REAL PIPELINE", &real_delta_xy, {{1.0f,1.0f,1.0f}}, true, true, 2},
            {"EQUAL STATUS-A", &equal_status_a_delta_xy, {{1.0f,0.65f,0.25f}}, true, true, 2},
            {"MATCHED DYE INCREMENT", &matched_increment_delta_xy, {{0.35f,1.0f,0.45f}}, true, true, 2},
            {"SCALED KODAK VISUAL NEUTRAL", &scaled_visual_neutral_delta_xy, {{0.35f,0.60f,1.0f}}, true, true, 2}
        };

        success =
            write_plot(
                stem
                    + "_2383_neutral_drift_decomposition.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // linear C/M/Y reference model: explicit legacy-vs-new neutral comparison.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title = "KODAK 2383 LINEAR REFERENCE NEUTRAL DRIFT A/B";
        options.subtitle = "LEGACY RESIDUAL MODEL VS RESIDUAL-FREE CALIBRATED C/M/Y";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "DELTA XY FROM D55 WHITE";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"LEGACY RESIDUAL", &legacy_delta_xy, {{1.0f,0.55f,0.25f}}, true, true, 2},
            {"LINEAR REFERENCE C/M/Y", &new_delta_xy, {{0.35f,1.0f,0.45f}}, true, true, 2}
        };

        success = write_plot(stem + "_2383_linear_reference_neutral_drift_ab.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 LINEAR REFERENCE AP0 NEUTRAL BALANCE A/B";
        options.subtitle = "(MAX - MIN) / MEAN AP0  LOWER IS MORE NEUTRAL";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "RELATIVE AP0 CHANNEL SPREAD";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"LEGACY RESIDUAL", &legacy_ap0_spread, {{1.0f,0.55f,0.25f}}, true, true, 2},
            {"LINEAR REFERENCE C/M/Y", &new_ap0_spread, {{0.35f,1.0f,0.45f}}, true, true, 2}
        };

        success = write_plot(stem + "_2383_linear_reference_ap0_neutral_balance_ab.png", series, options) && success;
    }

    // ------------------------------------------------------------------
    // Status-A mapping audit: legacy vs linear C/M/Y reference model diagonal mapping vs the local
    // ANSI Status-A matrix mapping. The matrix curve is diagnostic only and
    // uses a first-order inverse around the synthetic Status-A 1/1/1 point.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title = "KODAK 2383 STATUS-A AUDIT NEUTRAL DRIFT A/B/C";
        options.subtitle = "LEGACY VS LINEAR REFERENCE DIAGONAL VS ANSI STATUS-A LOCAL MATRIX";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "DELTA XY FROM D55 WHITE";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"LEGACY RESIDUAL", &legacy_delta_xy, {{1.0f,0.55f,0.25f}}, true, true, 2},
            {"LINEAR REFERENCE DIAGONAL C/M/Y", &new_delta_xy, {{0.35f,1.0f,0.45f}}, true, true, 2},
            {"STATUS-A AUDIT STATUS-A MATRIX", &matrix_delta_xy, {{0.35f,0.60f,1.0f}}, true, true, 2}
        };

        success =
            write_plot(
                stem + "_2383_status_a_neutral_drift_abc.png",
                series,
                options)
            && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 STATUS-A AUDIT AP0 NEUTRAL BALANCE A/B/C";
        options.subtitle = "LEGACY VS LINEAR REFERENCE DIAGONAL VS STATUS-A MATRIX  LOWER IS MORE NEUTRAL";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "RELATIVE AP0 CHANNEL SPREAD";
        options.has_x_range = true;
        options.x_min = -4.0f;
        options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f;
        options.include_zero_y = true;

        const std::vector<Series> series = {
            {"LEGACY RESIDUAL", &legacy_ap0_spread, {{1.0f,0.55f,0.25f}}, true, true, 2},
            {"LINEAR REFERENCE DIAGONAL C/M/Y", &new_ap0_spread, {{0.35f,1.0f,0.45f}}, true, true, 2},
            {"STATUS-A AUDIT STATUS-A MATRIX", &matrix_ap0_spread, {{0.35f,0.60f,1.0f}}, true, true, 2}
        };

        success =
            write_plot(
                stem + "_2383_status_a_ap0_neutral_balance_abc.png",
                series,
                options)
            && success;
    }

    // ------------------------------------------------------------------
    // neutral-preserving inverse: nonlinear neutral-preserving inverse, diagnostic only.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title = "KODAK 2383 NEUTRAL INVERSE NEUTRAL DRIFT A/B/C/D";
        options.subtitle = "LEGACY VS LINEAR REFERENCE DIAGONAL VS STATUS-A AUDIT STATUS-A VS NEUTRAL INVERSE NEUTRAL-PRESERVING INVERSE";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "DELTA XY FROM D55 WHITE";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"LEGACY RESIDUAL", &legacy_delta_xy, {{1.0f,0.55f,0.25f}}, true, true, 2},
            {"LINEAR REFERENCE DIAGONAL C/M/Y", &new_delta_xy, {{0.35f,1.0f,0.45f}}, true, true, 2},
            {"STATUS-A AUDIT STATUS-A MATRIX", &matrix_delta_xy, {{0.35f,0.60f,1.0f}}, true, true, 2},
            {"NEUTRAL INVERSE NEUTRAL-PRESERVING", &neutral_inverse_delta_xy, {{1.0f,0.25f,0.85f}}, true, true, 2}
        };
        success = write_plot(stem + "_2383_neutral_inverse_neutral_drift_abcd.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 NEUTRAL INVERSE AP0 NEUTRAL BALANCE A/B/C/D";
        options.subtitle = "LOWER IS MORE AP0-NEUTRAL; NEUTRAL INVERSE PRESERVES KODAK VISUAL-NEUTRAL XY";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "RELATIVE AP0 CHANNEL SPREAD";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"LEGACY RESIDUAL", &legacy_ap0_spread, {{1.0f,0.55f,0.25f}}, true, true, 2},
            {"LINEAR REFERENCE DIAGONAL C/M/Y", &new_ap0_spread, {{0.35f,1.0f,0.45f}}, true, true, 2},
            {"STATUS-A AUDIT STATUS-A MATRIX", &matrix_ap0_spread, {{0.35f,0.60f,1.0f}}, true, true, 2},
            {"NEUTRAL INVERSE NEUTRAL-PRESERVING", &neutral_inverse_ap0_spread, {{1.0f,0.25f,0.85f}}, true, true, 2}
        };
        success = write_plot(stem + "_2383_neutral_inverse_ap0_neutral_balance_abcd.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 NEUTRAL INVERSE INFERRED DYE-GROWTH AMPLITUDES";
        options.subtitle = "LINEAR REFERENCE MAPPING VS NEUTRAL INVERSE NEUTRAL-PRESERVING INVERSE";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "DYE-SHAPE AMPLITUDE";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"LINEAR REFERENCE C", &neutral_inverse_linear_c, {{0.20f,0.55f,0.55f}}, true, true, 2},
            {"NEUTRAL INVERSE C", &neutral_inverse_inferred_c, {{0.25f,1.0f,1.0f}}, true, true, 2},
            {"LINEAR REFERENCE M", &neutral_inverse_linear_m, {{0.55f,0.20f,0.50f}}, true, true, 2},
            {"NEUTRAL INVERSE M", &neutral_inverse_inferred_m, {{1.0f,0.25f,0.85f}}, true, true, 2},
            {"LINEAR REFERENCE Y", &neutral_inverse_linear_y, {{0.55f,0.50f,0.15f}}, true, true, 2},
            {"NEUTRAL INVERSE Y", &neutral_inverse_inferred_y, {{1.0f,0.90f,0.20f}}, true, true, 2}
        };
        success = write_plot(stem + "_2383_neutral_inverse_inferred_dye_growth.png", series, options) && success;
    }


    // ------------------------------------------------------------------
    // nonlinear dye-growth model: active nonlinear-growth production A/B against LINEAR REFERENCE.
    // The neutral ramp is generated internally so the comparison is not
    // contaminated by image sampling or input-transform uncertainty.
    // ------------------------------------------------------------------
    SampledCurve nonlinear_growth_linear_reference_delta_xy;
    SampledCurve nonlinear_growth_active_delta_xy;
    SampledCurve nonlinear_growth_linear_reference_ap0_spread;
    SampledCurve nonlinear_growth_active_ap0_spread;
    SampledCurve nonlinear_growth_linear_c;
    SampledCurve nonlinear_growth_active_c;
    SampledCurve nonlinear_growth_linear_m;
    SampledCurve nonlinear_growth_active_m;
    SampledCurve nonlinear_growth_linear_y;
    SampledCurve nonlinear_growth_active_y;

    float nonlinear_growth_max_xy = 0.0f;
    float nonlinear_growth_max_linear_reference_xy = 0.0f;
    float nonlinear_growth_max_relative_y_change = 0.0f;

    const auto nonlinear_growth_spread = [](const std::array<float, 3>& rgb) {
        const float minimum = std::min(rgb[0], std::min(rgb[1], rgb[2]));
        const float maximum = std::max(rgb[0], std::max(rgb[1], rgb[2]));
        const float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
        return mean > 1e-12f ? (maximum - minimum) / mean : 0.0f;
    };

    for (int i = 0; i <= 80; ++i) {
        const float stop = -4.0f + 8.0f * static_cast<float>(i) / 80.0f;
        FilmDensity density;
        Vector3 linear_unused = {{0.0f,0.0f,0.0f}};
        PrintViewer::Result linear_reference_viewed;
        dense_mapping_state_at_stop(stop, density, linear_unused, linear_reference_viewed);

        const PrintViewer::Result nonlinear_growth_viewed = print_viewer.view(
            print_dye_model.synthesize_transmittance(density));

        const float linear_reference_dx = linear_reference_viewed.viewed_xy.x
            - status_a_audit_measured_visual_neutral_viewed.viewed_xy.x;
        const float linear_reference_dy = linear_reference_viewed.viewed_xy.y
            - status_a_audit_measured_visual_neutral_viewed.viewed_xy.y;
        const float linear_reference_xy = std::sqrt(linear_reference_dx * linear_reference_dx + linear_reference_dy * linear_reference_dy);

        const float nonlinear_growth_dx = nonlinear_growth_viewed.viewed_xy.x
            - status_a_audit_measured_visual_neutral_viewed.viewed_xy.x;
        const float nonlinear_growth_dy = nonlinear_growth_viewed.viewed_xy.y
            - status_a_audit_measured_visual_neutral_viewed.viewed_xy.y;
        const float nonlinear_growth_xy = std::sqrt(nonlinear_growth_dx * nonlinear_growth_dx + nonlinear_growth_dy * nonlinear_growth_dy);

        const float relative_y_change = linear_reference_viewed.viewed_xyz.y > 1e-12f
            ? std::abs(nonlinear_growth_viewed.viewed_xyz.y - linear_reference_viewed.viewed_xyz.y)
                / linear_reference_viewed.viewed_xyz.y
            : 0.0f;

        nonlinear_growth_linear_reference_delta_xy.x.push_back(stop); nonlinear_growth_linear_reference_delta_xy.y.push_back(linear_reference_xy);
        nonlinear_growth_active_delta_xy.x.push_back(stop); nonlinear_growth_active_delta_xy.y.push_back(nonlinear_growth_xy);
        nonlinear_growth_linear_reference_ap0_spread.x.push_back(stop); nonlinear_growth_linear_reference_ap0_spread.y.push_back(nonlinear_growth_spread(linear_reference_viewed.aces2065_1));
        nonlinear_growth_active_ap0_spread.x.push_back(stop); nonlinear_growth_active_ap0_spread.y.push_back(nonlinear_growth_spread(nonlinear_growth_viewed.aces2065_1));

        const FilmDensity linear_amp = print_dye_model.linear_reference_amplitudes(density);
        const FilmDensity active_amp = print_dye_model.mapped_reference_amplitudes(density);
        nonlinear_growth_linear_c.x.push_back(stop); nonlinear_growth_linear_c.y.push_back(linear_amp.red);
        nonlinear_growth_active_c.x.push_back(stop); nonlinear_growth_active_c.y.push_back(active_amp.red);
        nonlinear_growth_linear_m.x.push_back(stop); nonlinear_growth_linear_m.y.push_back(linear_amp.green);
        nonlinear_growth_active_m.x.push_back(stop); nonlinear_growth_active_m.y.push_back(active_amp.green);
        nonlinear_growth_linear_y.x.push_back(stop); nonlinear_growth_linear_y.y.push_back(linear_amp.blue);
        nonlinear_growth_active_y.x.push_back(stop); nonlinear_growth_active_y.y.push_back(active_amp.blue);

        nonlinear_growth_max_linear_reference_xy = std::max(nonlinear_growth_max_linear_reference_xy, linear_reference_xy);
        nonlinear_growth_max_xy = std::max(nonlinear_growth_max_xy, nonlinear_growth_xy);
        nonlinear_growth_max_relative_y_change = std::max(nonlinear_growth_max_relative_y_change, relative_y_change);
    }

    std::cout << "info: nonlinear dye-growth model active nonlinear-growth A/B" << std::endl;
    std::cout << "info:   internal neutral ramp: 81 points (-4..+4 stops)" << std::endl;
    std::cout << "info:   LINEAR REFERENCE max delta xy from Kodak Visual Neutral: " << nonlinear_growth_max_linear_reference_xy << std::endl;
    std::cout << "info:   NONLINEAR GROWTH max delta xy from Kodak Visual Neutral: " << nonlinear_growth_max_xy << std::endl;
    std::cout << "info:   NONLINEAR GROWTH max relative Y change vs LINEAR REFERENCE tone: " << nonlinear_growth_max_relative_y_change << std::endl;
    std::cout << "info:   NOTE: NONLINEAR GROWTH production mapping is the MAPPING QUALIFICATION-qualified 161-point monotone-cubic calibration" << std::endl;

    {
        PlotOptions options;
        options.title = "KODAK 2383 NONLINEAR GROWTH NEUTRAL DRIFT A/B";
        options.subtitle = "LINEAR REFERENCE VS NONLINEAR GROWTH QUALIFIED NONLINEAR GROWTH  LOWER IS BETTER";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "DELTA XY FROM KODAK VISUAL NEUTRAL";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"LINEAR REFERENCE", &nonlinear_growth_linear_reference_delta_xy, {{0.85f,0.55f,0.25f}}, true, false, 2},
            {"NONLINEAR GROWTH NONLINEAR", &nonlinear_growth_active_delta_xy, {{0.25f,1.0f,0.55f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_nonlinear_growth_neutral_drift_ab.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 NONLINEAR GROWTH AP0 NEUTRAL BALANCE A/B";
        options.subtitle = "LINEAR REFERENCE VS NONLINEAR GROWTH QUALIFIED NONLINEAR GROWTH  LOWER IS BETTER";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "RELATIVE AP0 CHANNEL SPREAD";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"LINEAR REFERENCE", &nonlinear_growth_linear_reference_ap0_spread, {{0.85f,0.55f,0.25f}}, true, false, 2},
            {"NONLINEAR GROWTH NONLINEAR", &nonlinear_growth_active_ap0_spread, {{0.25f,1.0f,0.55f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_nonlinear_growth_ap0_neutral_balance_ab.png", series, options) && success;
    }

    {
        PlotOptions options;
        options.title = "KODAK 2383 NONLINEAR GROWTH DYE AMPLITUDE A/B";
        options.subtitle = "LINEAR REFERENCE AMPLITUDES VS NONLINEAR GROWTH QUALIFIED NONLINEAR AMPLITUDES";
        options.x_label = "NEGATIVE EXPOSURE STOPS";
        options.y_label = "DYE-SHAPE AMPLITUDE";
        options.has_x_range = true; options.x_min = -4.0f; options.x_max = 4.0f;
        options.x_padding_fraction = 0.0f; options.include_zero_y = true;
        const std::vector<Series> series = {
            {"LINEAR REFERENCE C", &nonlinear_growth_linear_c, {{0.20f,0.55f,0.55f}}, true, false, 2},
            {"NONLINEAR GROWTH C", &nonlinear_growth_active_c, {{0.25f,1.0f,1.0f}}, true, false, 2},
            {"LINEAR REFERENCE M", &nonlinear_growth_linear_m, {{0.55f,0.20f,0.50f}}, true, false, 2},
            {"NONLINEAR GROWTH M", &nonlinear_growth_active_m, {{1.0f,0.25f,0.85f}}, true, false, 2},
            {"LINEAR REFERENCE Y", &nonlinear_growth_linear_y, {{0.55f,0.50f,0.15f}}, true, false, 2},
            {"NONLINEAR GROWTH Y", &nonlinear_growth_active_y, {{1.0f,0.90f,0.20f}}, true, false, 2}
        };
        success = write_plot(stem + "_2383_nonlinear_growth_dye_amplitude_ab.png", series, options) && success;
    }

    // ------------------------------------------------------------------
    // Viewed spectral power for selected neutral states.
    // ------------------------------------------------------------------
    {
        PlotOptions options;
        options.title =
            "KODAK 2383 VIEWED NEUTRAL SPECTRA";
        options.subtitle =
            "VIEWING SPD X PRINT TRANSMITTANCE  VERITA -4 / 0 / +4 STOPS";
        options.x_label =
            "WAVELENGTH NM";
        options.y_label =
            "RELATIVE VIEWED POWER";
        options.has_x_range = true;
        options.x_min =
            print_viewer.settings().wavelength_min_nm;
        options.x_max =
            print_viewer.settings().wavelength_max_nm;
        options.include_zero_y = true;
        options.x_padding_fraction = 0.0f;

        const std::vector<Series> series = {
            {"-4 STOPS", &viewed_spectra[0], {{0.55f,0.55f,0.55f}}, true, false, 2},
            {"0 STOPS",  &viewed_spectra[1], {{1.00f,1.00f,1.00f}}, true, false, 2},
            {"+4 STOPS", &viewed_spectra[2], {{0.35f,0.50f,1.00f}}, true, false, 2}
        };

        success =
            write_plot(
                stem
                    + "_2383_viewed_neutral_spectra.png",
                series,
                options)
            && success;
    }

    return success;
}
