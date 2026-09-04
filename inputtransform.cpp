// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "inputtransform.h"

#include "colorimetry.h"

#include <algorithm>
#include <cmath>

InputTransform::InputTransform(
    Encoding encoding)
    : encoding_(encoding)
{
}

std::array<float, 3>
InputTransform::to_ap0(
    const std::array<float, 3>& encoded_rgb) const
{
    if (encoding_
        == Encoding::ACES2065_1_Linear) {

        return encoded_rgb;
    }

    return
        awg3_linear_to_ap0(
            arri_logc3_ei800_to_linear(
                encoded_rgb));
}

InputTransform::Encoding
InputTransform::encoding() const
{
    return encoding_;
}

bool
InputTransform::parse_encoding(
    const std::string& name,
    Encoding& encoding)
{
    if (name == "awg3-logc3-ei800"
        || name == "arri-awg3-logc3-ei800") {

        encoding =
            Encoding::AWG3_LogC3_EI800;

        return true;
    }

    if (name == "ap0-linear"
        || name == "aces2065-1-linear") {

        encoding =
            Encoding::ACES2065_1_Linear;

        return true;
    }

    return false;
}

const char*
InputTransform::name(
    Encoding encoding)
{
    switch (encoding) {
        case Encoding::AWG3_LogC3_EI800:
            return "awg3-logc3-ei800";

        case Encoding::ACES2065_1_Linear:
            return "ap0-linear";
    }

    return "unknown";
}

std::array<float, 3>
InputTransform::arri_logc3_ei800_to_linear(
    const std::array<float, 3>& value)
{
    constexpr double cut = 0.010591;
    constexpr double a = 5.555556;
    constexpr double b = 0.052272;
    constexpr double c = 0.247190;
    constexpr double d = 0.385537;
    constexpr double e = 5.367655;
    constexpr double f = 0.092809;

    const double encoded_cut =
        e * cut
        + f;

    std::array<float, 3> result = value;

    for (int channel = 0;
         channel < 3;
         ++channel) {

        const double y =
            static_cast<double>(
                value[channel]);

        double x = 0.0;

        if (y > encoded_cut) {
            x =
                (std::pow(
                     10.0,
                     (y - d) / c)
                 - b)
                / a;
        }
        else {
            x =
                (y - f)
                / e;
        }

        result[channel] =
            static_cast<float>(
                std::max(
                    0.0,
                    x));
    }

    return result;
}

std::array<float, 3>
InputTransform::awg3_linear_to_ap0(
    const std::array<float, 3>& awg3)
{
    const double X =
        0.638008 * awg3[0]
        + 0.214704 * awg3[1]
        + 0.097744 * awg3[2];

    const double Y =
        0.291954 * awg3[0]
        + 0.823841 * awg3[1]
        - 0.115795 * awg3[2];

    const double Z =
        0.002798 * awg3[0]
        - 0.067034 * awg3[1]
        + 1.153294 * awg3[2];

    Colorimetry::XYZ xyz_d65;

    xyz_d65.x = X;
    xyz_d65.y = Y;
    xyz_d65.z = Z;

    const Colorimetry::XYZ d65_white =
        Colorimetry::xy_to_xyz_white(
            0.3127,
            0.3290);

    const Colorimetry::XYZ d60_white =
        Colorimetry::aces_d60_white();

    const Colorimetry::Matrix3 d65_to_d60 =
        Colorimetry::bradford_adaptation(
            d65_white,
            d60_white);

    const Colorimetry::XYZ xyz_d60 =
        Colorimetry::multiply(
            d65_to_d60,
            xyz_d65);

    const std::array<double, 3> ap0 =
        Colorimetry::xyz_d60_to_ap0(
            xyz_d60);

    return {{
        static_cast<float>(ap0[0]),
        static_cast<float>(ap0[1]),
        static_cast<float>(ap0[2])
    }};
}
