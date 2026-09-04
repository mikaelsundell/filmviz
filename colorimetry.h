// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <array>

class Colorimetry
{
public:
    struct XYZ
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct xy
    {
        double x = 0.0;
        double y = 0.0;
    };

    struct Lab
    {
        double l = 0.0;
        double a = 0.0;
        double b = 0.0;
    };

    struct Matrix3
    {
        double m[3][3] = {
            {0.0, 0.0, 0.0},
            {0.0, 0.0, 0.0},
            {0.0, 0.0, 0.0}
        };
    };

    static XYZ xy_to_xyz_white(
        double x,
        double y);

    static xy xyz_to_xy(
        const XYZ& xyz);

    static XYZ scale_xyz_to_y(
        const XYZ& xyz,
        double target_y);

    static double chromaticity_distance(
        const XYZ& a,
        const XYZ& b);

    static XYZ multiply(
        const Matrix3& matrix,
        const XYZ& value);

    static std::array<double, 3> multiply(
        const Matrix3& matrix,
        const std::array<double, 3>& value);

    static Matrix3 multiply(
        const Matrix3& a,
        const Matrix3& b);

    static double determinant(
        const Matrix3& matrix);

    static bool inverse(
        const Matrix3& matrix,
        Matrix3& result);

    static Matrix3 identity();

    static Matrix3 bradford_adaptation(
        const XYZ& source_white,
        const XYZ& target_white);

    static Lab xyz_to_lab(
        const XYZ& xyz,
        const XYZ& white);

    static XYZ lab_to_xyz(
        const Lab& lab,
        const XYZ& white);

    static double hue_degrees(
        double a,
        double b);

    static double signed_hue_delta_degrees(
        double from_degrees,
        double to_degrees);

    static XYZ aces_d60_white();
    static xy aces_d60_xy();

    static XYZ ap0_to_xyz_d60(
        const std::array<double, 3>& ap0);

    static std::array<double, 3> xyz_d60_to_ap0(
        const XYZ& xyz);
};
