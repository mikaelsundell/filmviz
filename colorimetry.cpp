// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "colorimetry.h"

#include <algorithm>
#include <cmath>

namespace {

double
lab_f(
    double t)
{
    const double delta =
        6.0 / 29.0;

    const double delta3 =
        delta * delta * delta;

    if (t > delta3) {
        return std::cbrt(t);
    }

    return
        t / (3.0 * delta * delta)
        + 4.0 / 29.0;
}

double
lab_f_inverse(
    double value)
{
    const double delta =
        6.0 / 29.0;

    if (value > delta) {
        return
            value
            * value
            * value;
    }

    return
        3.0
        * delta
        * delta
        * (value - 4.0 / 29.0);
}

} // namespace

Colorimetry::XYZ
Colorimetry::xy_to_xyz_white(
    double x,
    double y)
{
    XYZ result;

    if (std::abs(y) <= 1e-20) {
        result.y = 1.0;
        return result;
    }

    result.x =
        x / y;

    result.y =
        1.0;

    result.z =
        (1.0 - x - y)
        / y;

    return result;
}

Colorimetry::xy
Colorimetry::xyz_to_xy(
    const XYZ& xyz)
{
    xy result;

    const double sum =
        xyz.x
        + xyz.y
        + xyz.z;

    if (std::abs(sum) <= 1e-20) {
        return result;
    }

    result.x =
        xyz.x / sum;

    result.y =
        xyz.y / sum;

    return result;
}

Colorimetry::XYZ
Colorimetry::scale_xyz_to_y(
    const XYZ& xyz,
    double target_y)
{
    if (std::abs(xyz.y) <= 1e-20) {
        return xyz;
    }

    const double scale =
        target_y / xyz.y;

    return {
        xyz.x * scale,
        xyz.y * scale,
        xyz.z * scale
    };
}

double
Colorimetry::chromaticity_distance(
    const XYZ& a,
    const XYZ& b)
{
    const xy aa = xyz_to_xy(a);
    const xy bb = xyz_to_xy(b);

    const double dx = aa.x - bb.x;
    const double dy = aa.y - bb.y;

    return std::sqrt(dx * dx + dy * dy);
}

Colorimetry::XYZ
Colorimetry::multiply(
    const Matrix3& matrix,
    const XYZ& value)
{
    XYZ result;

    result.x =
        matrix.m[0][0] * value.x
        + matrix.m[0][1] * value.y
        + matrix.m[0][2] * value.z;

    result.y =
        matrix.m[1][0] * value.x
        + matrix.m[1][1] * value.y
        + matrix.m[1][2] * value.z;

    result.z =
        matrix.m[2][0] * value.x
        + matrix.m[2][1] * value.y
        + matrix.m[2][2] * value.z;

    return result;
}

std::array<double, 3>
Colorimetry::multiply(
    const Matrix3& matrix,
    const std::array<double, 3>& value)
{
    return {{
        matrix.m[0][0] * value[0]
            + matrix.m[0][1] * value[1]
            + matrix.m[0][2] * value[2],

        matrix.m[1][0] * value[0]
            + matrix.m[1][1] * value[1]
            + matrix.m[1][2] * value[2],

        matrix.m[2][0] * value[0]
            + matrix.m[2][1] * value[1]
            + matrix.m[2][2] * value[2]
    }};
}

Colorimetry::Matrix3
Colorimetry::multiply(
    const Matrix3& a,
    const Matrix3& b)
{
    Matrix3 result;

    for (int row = 0;
         row < 3;
         ++row) {

        for (int column = 0;
             column < 3;
             ++column) {

            for (int k = 0;
                 k < 3;
                 ++k) {

                result.m[row][column] +=
                    a.m[row][k]
                    * b.m[k][column];
            }
        }
    }

    return result;
}

double
Colorimetry::determinant(
    const Matrix3& a)
{
    return
        a.m[0][0]
            * (
                a.m[1][1] * a.m[2][2]
                - a.m[1][2] * a.m[2][1]
            )
        - a.m[0][1]
            * (
                a.m[1][0] * a.m[2][2]
                - a.m[1][2] * a.m[2][0]
            )
        + a.m[0][2]
            * (
                a.m[1][0] * a.m[2][1]
                - a.m[1][1] * a.m[2][0]
            );
}

bool
Colorimetry::inverse(
    const Matrix3& a,
    Matrix3& result)
{
    const double det =
        determinant(
            a);

    if (std::abs(det) <= 1e-20) {
        result =
            identity();

        return false;
    }

    const double inv_det =
        1.0 / det;

    result.m[0][0] =
        (a.m[1][1] * a.m[2][2]
         - a.m[1][2] * a.m[2][1])
        * inv_det;

    result.m[0][1] =
        (a.m[0][2] * a.m[2][1]
         - a.m[0][1] * a.m[2][2])
        * inv_det;

    result.m[0][2] =
        (a.m[0][1] * a.m[1][2]
         - a.m[0][2] * a.m[1][1])
        * inv_det;

    result.m[1][0] =
        (a.m[1][2] * a.m[2][0]
         - a.m[1][0] * a.m[2][2])
        * inv_det;

    result.m[1][1] =
        (a.m[0][0] * a.m[2][2]
         - a.m[0][2] * a.m[2][0])
        * inv_det;

    result.m[1][2] =
        (a.m[0][2] * a.m[1][0]
         - a.m[0][0] * a.m[1][2])
        * inv_det;

    result.m[2][0] =
        (a.m[1][0] * a.m[2][1]
         - a.m[1][1] * a.m[2][0])
        * inv_det;

    result.m[2][1] =
        (a.m[0][1] * a.m[2][0]
         - a.m[0][0] * a.m[2][1])
        * inv_det;

    result.m[2][2] =
        (a.m[0][0] * a.m[1][1]
         - a.m[0][1] * a.m[1][0])
        * inv_det;

    return true;
}

Colorimetry::Matrix3
Colorimetry::identity()
{
    Matrix3 result;

    result.m[0][0] = 1.0;
    result.m[1][1] = 1.0;
    result.m[2][2] = 1.0;

    return result;
}

Colorimetry::Matrix3
Colorimetry::bradford_adaptation(
    const XYZ& source_white,
    const XYZ& target_white)
{
    const Matrix3 bradford = {{
        { 0.8951,  0.2664, -0.1614},
        {-0.7502,  1.7135,  0.0367},
        { 0.0389, -0.0685,  1.0296}
    }};

    const XYZ source_lms =
        multiply(
            bradford,
            source_white);

    const XYZ target_lms =
        multiply(
            bradford,
            target_white);

    Matrix3 scale =
        identity();

    scale.m[0][0] =
        std::abs(source_lms.x) > 1e-20
            ? target_lms.x / source_lms.x
            : 1.0;

    scale.m[1][1] =
        std::abs(source_lms.y) > 1e-20
            ? target_lms.y / source_lms.y
            : 1.0;

    scale.m[2][2] =
        std::abs(source_lms.z) > 1e-20
            ? target_lms.z / source_lms.z
            : 1.0;

    Matrix3 inverse_bradford;
    inverse(
        bradford,
        inverse_bradford);

    return
        multiply(
            multiply(
                inverse_bradford,
                scale),
            bradford);
}

Colorimetry::Lab
Colorimetry::xyz_to_lab(
    const XYZ& xyz,
    const XYZ& white)
{
    Lab result;

    const double xr =
        std::abs(white.x) > 1e-20
            ? xyz.x / white.x
            : 0.0;

    const double yr =
        std::abs(white.y) > 1e-20
            ? xyz.y / white.y
            : 0.0;

    const double zr =
        std::abs(white.z) > 1e-20
            ? xyz.z / white.z
            : 0.0;

    const double fx =
        lab_f(
            xr);

    const double fy =
        lab_f(
            yr);

    const double fz =
        lab_f(
            zr);

    result.l =
        116.0 * fy
        - 16.0;

    result.a =
        500.0
        * (fx - fy);

    result.b =
        200.0
        * (fy - fz);

    return result;
}

Colorimetry::XYZ
Colorimetry::lab_to_xyz(
    const Lab& lab,
    const XYZ& white)
{
    const double fy =
        (lab.l + 16.0)
        / 116.0;

    const double fx =
        fy
        + lab.a / 500.0;

    const double fz =
        fy
        - lab.b / 200.0;

    XYZ result;

    result.x =
        white.x
        * lab_f_inverse(
            fx);

    result.y =
        white.y
        * lab_f_inverse(
            fy);

    result.z =
        white.z
        * lab_f_inverse(
            fz);

    return result;
}

double
Colorimetry::hue_degrees(
    double a,
    double b)
{
    double hue =
        std::atan2(
            b,
            a)
        * 180.0
        / 3.14159265358979323846;

    if (hue < 0.0) {
        hue += 360.0;
    }

    return hue;
}

double
Colorimetry::signed_hue_delta_degrees(
    double from_degrees,
    double to_degrees)
{
    double delta =
        to_degrees
        - from_degrees;

    while (delta > 180.0) {
        delta -= 360.0;
    }

    while (delta < -180.0) {
        delta += 360.0;
    }

    return delta;
}

Colorimetry::XYZ
Colorimetry::aces_d60_white()
{
    return
        xy_to_xyz_white(
            0.32168,
            0.33767);
}

Colorimetry::xy
Colorimetry::aces_d60_xy()
{
    return {
        0.32168,
        0.33767
    };
}

Colorimetry::XYZ
Colorimetry::ap0_to_xyz_d60(
    const std::array<double, 3>& ap0)
{
    const Matrix3 ap0_to_xyz = {{
        {0.9525523959, 0.0000000000, 0.0000936786},
        {0.3439664498, 0.7281660966,-0.0721325464},
        {0.0000000000, 0.0000000000, 1.0088251844}
    }};

    const auto xyz =
        multiply(
            ap0_to_xyz,
            ap0);

    return {
        xyz[0],
        xyz[1],
        xyz[2]
    };
}

std::array<double, 3>
Colorimetry::xyz_d60_to_ap0(
    const XYZ& xyz)
{
    const Matrix3 ap0_to_xyz = {{
        {0.9525523959, 0.0000000000, 0.0000936786},
        {0.3439664498, 0.7281660966,-0.0721325464},
        {0.0000000000, 0.0000000000, 1.0088251844}
    }};

    Matrix3 xyz_to_ap0;
    inverse(
        ap0_to_xyz,
        xyz_to_ap0);

    const std::array<double, 3> xyz_array = {{
        xyz.x,
        xyz.y,
        xyz.z
    }};

    const auto ap0 =
        multiply(
            xyz_to_ap0,
            xyz_array);

    return ap0;
}
