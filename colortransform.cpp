// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "colortransform.h"
#include "colorimetry.h"

#include <algorithm>
#include <cmath>

namespace {

using Matrix3 = Colorimetry::Matrix3;
using XYZ = Colorimetry::XYZ;

struct LogC3Curve
{
    float cut = 0.0f;
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 0.0f;
    float e = 0.0f;
    float f = 0.0f;
};

bool get_logc3_curve(int ei, LogC3Curve& cv)
{
    cv.a = 5.555556f;

    switch (ei) {
        case 160:  cv.cut=0.005561f; cv.b=0.080216f; cv.c=0.269036f; cv.d=0.381991f; cv.e=5.842037f; cv.f=0.092778f; return true;
        case 200:  cv.cut=0.006208f; cv.b=0.076621f; cv.c=0.266007f; cv.d=0.382478f; cv.e=5.776265f; cv.f=0.092782f; return true;
        case 250:  cv.cut=0.006871f; cv.b=0.072941f; cv.c=0.262978f; cv.d=0.382966f; cv.e=5.710494f; cv.f=0.092786f; return true;
        case 320:  cv.cut=0.007622f; cv.b=0.068768f; cv.c=0.259627f; cv.d=0.383508f; cv.e=5.637732f; cv.f=0.092791f; return true;
        case 400:  cv.cut=0.008318f; cv.b=0.064901f; cv.c=0.256598f; cv.d=0.383999f; cv.e=5.571960f; cv.f=0.092795f; return true;
        case 500:  cv.cut=0.009031f; cv.b=0.060939f; cv.c=0.253569f; cv.d=0.384493f; cv.e=5.506188f; cv.f=0.092800f; return true;
        case 640:  cv.cut=0.009840f; cv.b=0.056443f; cv.c=0.250219f; cv.d=0.385040f; cv.e=5.433426f; cv.f=0.092805f; return true;
        case 800:  cv.cut=0.010591f; cv.b=0.052272f; cv.c=0.247190f; cv.d=0.385537f; cv.e=5.367655f; cv.f=0.092809f; return true;
        case 1000: cv.cut=0.011361f; cv.b=0.047996f; cv.c=0.244161f; cv.d=0.386036f; cv.e=5.301883f; cv.f=0.092814f; return true;
        case 1280: cv.cut=0.012235f; cv.b=0.043137f; cv.c=0.240810f; cv.d=0.386590f; cv.e=5.229121f; cv.f=0.092819f; return true;
        case 1600: cv.cut=0.013047f; cv.b=0.038625f; cv.c=0.237781f; cv.d=0.387093f; cv.e=5.163350f; cv.f=0.092824f; return true;
        default: return false;
    }
}

float logc3_to_linear(const LogC3Curve& cv, float value)
{
    const float threshold = cv.e * cv.cut + cv.f;
    if (value > threshold) {
        return (std::pow(10.0f, (value - cv.d) / cv.c) - cv.b) / cv.a;
    }
    return (value - cv.f) / cv.e;
}

float linear_to_logc3(const LogC3Curve& cv, float value)
{
    if (value > cv.cut) {
        const float argument = std::max(1e-20f, cv.a * value + cv.b);
        return cv.c * std::log10(argument) + cv.d;
    }
    return cv.e * value + cv.f;
}

const Matrix3& awg3_to_xyz_d65()
{
    static const Matrix3 matrix = {{
        { 0.638008,  0.214704,  0.097744},
        { 0.291954,  0.823841, -0.115795},
        { 0.002798, -0.067034,  1.153294}
    }};
    return matrix;
}

const Matrix3& rec709_to_xyz_d65()
{
    static const Matrix3 matrix = {{
        {0.4123907993, 0.3575843394, 0.1804807884},
        {0.2126390059, 0.7151686788, 0.0721923154},
        {0.0193308187, 0.1191947798, 0.9505321522}
    }};
    return matrix;
}

XYZ d65_white()
{
    return Colorimetry::xy_to_xyz_white(0.31272, 0.32903);
}

std::array<double, 3> to_double(const std::array<float, 3>& v)
{
    return {{v[0], v[1], v[2]}};
}

std::array<float, 3> to_float(const std::array<double, 3>& v)
{
    return {{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])}};
}

XYZ to_xyz(const std::array<double, 3>& v)
{
    XYZ xyz;
    xyz.x = v[0]; xyz.y = v[1]; xyz.z = v[2];
    return xyz;
}

std::array<double, 3> to_array(const XYZ& xyz)
{
    return {{xyz.x, xyz.y, xyz.z}};
}

XYZ linear_rgb_to_xyz_native(
    ColorTransform::ColorSpace space,
    const std::array<float, 3>& rgb)
{
    switch (space) {
        case ColorTransform::ColorSpace::ACES2065_1:
            return Colorimetry::ap0_to_xyz_d60(to_double(rgb));

        case ColorTransform::ColorSpace::AWG3:
            return to_xyz(Colorimetry::multiply(awg3_to_xyz_d65(), to_double(rgb)));

        case ColorTransform::ColorSpace::Rec709:
            return to_xyz(Colorimetry::multiply(rec709_to_xyz_d65(), to_double(rgb)));
    }

    return XYZ();
}

XYZ native_xyz_to_d60(ColorTransform::ColorSpace space, const XYZ& xyz)
{
    if (space == ColorTransform::ColorSpace::ACES2065_1) {
        return xyz;
    }

    const Matrix3 cat = Colorimetry::bradford_adaptation(
        d65_white(),
        Colorimetry::aces_d60_white());

    return Colorimetry::multiply(cat, xyz);
}

XYZ d60_xyz_to_native(ColorTransform::ColorSpace space, const XYZ& xyz60)
{
    if (space == ColorTransform::ColorSpace::ACES2065_1) {
        return xyz60;
    }

    const Matrix3 cat = Colorimetry::bradford_adaptation(
        Colorimetry::aces_d60_white(),
        d65_white());

    return Colorimetry::multiply(cat, xyz60);
}

std::array<float, 3> xyz_native_to_linear_rgb(
    ColorTransform::ColorSpace space,
    const XYZ& xyz)
{
    if (space == ColorTransform::ColorSpace::ACES2065_1) {
        return to_float(Colorimetry::xyz_d60_to_ap0(xyz));
    }

    const Matrix3* rgb_to_xyz = nullptr;
    if (space == ColorTransform::ColorSpace::AWG3) {
        rgb_to_xyz = &awg3_to_xyz_d65();
    }
    else {
        rgb_to_xyz = &rec709_to_xyz_d65();
    }

    Matrix3 xyz_to_rgb;
    Colorimetry::inverse(*rgb_to_xyz, xyz_to_rgb);
    return to_float(Colorimetry::multiply(xyz_to_rgb, to_array(xyz)));
}

std::array<float, 3> decode_transfer_function(
    ColorTransform::TransferFunction transfer,
    const std::array<float, 3>& rgb,
    int logc3_ei)
{
    if (transfer == ColorTransform::TransferFunction::Linear) {
        return rgb;
    }

    std::array<float, 3> result = rgb;

    if (transfer == ColorTransform::TransferFunction::Gamma24) {
        for (int i = 0; i < 3; ++i) {
            result[i] = std::pow(std::max(0.0f, rgb[i]), 2.4f);
        }
        return result;
    }

    LogC3Curve curve;
    if (!get_logc3_curve(logc3_ei, curve)) {
        return rgb;
    }

    for (int i = 0; i < 3; ++i) {
        result[i] = logc3_to_linear(curve, rgb[i]);
    }
    return result;
}

std::array<float, 3> encode_transfer_function(
    ColorTransform::TransferFunction transfer,
    const std::array<float, 3>& rgb,
    int logc3_ei)
{
    if (transfer == ColorTransform::TransferFunction::Linear) {
        return rgb;
    }

    std::array<float, 3> result = rgb;

    if (transfer == ColorTransform::TransferFunction::Gamma24) {
        for (int i = 0; i < 3; ++i) {
            result[i] = std::pow(std::max(0.0f, rgb[i]), 1.0f / 2.4f);
        }
        return result;
    }

    LogC3Curve curve;
    if (!get_logc3_curve(logc3_ei, curve)) {
        return rgb;
    }

    for (int i = 0; i < 3; ++i) {
        result[i] = linear_to_logc3(curve, rgb[i]);
    }
    return result;
}

} // namespace

ColorTransform::ColorTransform(
    ColorSpace source_space,
    TransferFunction source_transfer,
    ColorSpace destination_space,
    TransferFunction destination_transfer,
    int logc3_ei)
    : source_space_(source_space)
    , source_transfer_(source_transfer)
    , destination_space_(destination_space)
    , destination_transfer_(destination_transfer)
    , logc3_ei_(logc3_ei)
{
}

bool ColorTransform::parse_color_space(const std::string& name, ColorSpace& color_space)
{
    if (name == "aces" || name == "ap0" || name == "aces2065-1") {
        color_space = ColorSpace::ACES2065_1;
        return true;
    }
    if (name == "awg3") {
        color_space = ColorSpace::AWG3;
        return true;
    }
    if (name == "rec709" || name == "bt709") {
        color_space = ColorSpace::Rec709;
        return true;
    }
    return false;
}

bool ColorTransform::parse_transfer_function(
    const std::string& name,
    TransferFunction& transfer_function)
{
    if (name == "linear") {
        transfer_function = TransferFunction::Linear;
        return true;
    }
    if (name == "logc3") {
        transfer_function = TransferFunction::LogC3;
        return true;
    }
    if (name == "gamma24" || name == "gamma2.4") {
        transfer_function = TransferFunction::Gamma24;
        return true;
    }
    return false;
}

const char* ColorTransform::color_space_name(ColorSpace color_space)
{
    switch (color_space) {
        case ColorSpace::ACES2065_1: return "aces2065-1";
        case ColorSpace::AWG3: return "awg3";
        case ColorSpace::Rec709: return "rec709";
    }
    return "unknown";
}

const char* ColorTransform::transfer_function_name(TransferFunction transfer_function)
{
    switch (transfer_function) {
        case TransferFunction::Linear: return "linear";
        case TransferFunction::LogC3: return "logc3";
        case TransferFunction::Gamma24: return "gamma2.4";
    }
    return "unknown";
}

bool ColorTransform::supported_logc3_ei(int ei)
{
    LogC3Curve curve;
    return get_logc3_curve(ei, curve);
}

std::array<float, 3> ColorTransform::decode_transfer(
    const std::array<float, 3>& encoded_rgb) const
{
    return decode_transfer_function(source_transfer_, encoded_rgb, logc3_ei_);
}

std::array<float, 3> ColorTransform::transform_linear(
    const std::array<float, 3>& linear_rgb) const
{
    if (source_space_ == destination_space_) {
        return linear_rgb;
    }

    const XYZ source_native_xyz = linear_rgb_to_xyz_native(source_space_, linear_rgb);
    const XYZ xyz60 = native_xyz_to_d60(source_space_, source_native_xyz);
    const XYZ destination_native_xyz = d60_xyz_to_native(destination_space_, xyz60);
    return xyz_native_to_linear_rgb(destination_space_, destination_native_xyz);
}

std::array<float, 3> ColorTransform::encode_transfer(
    const std::array<float, 3>& linear_rgb) const
{
    return encode_transfer_function(destination_transfer_, linear_rgb, logc3_ei_);
}

std::array<float, 3> ColorTransform::transform(
    const std::array<float, 3>& rgb) const
{
    return encode_transfer(transform_linear(decode_transfer(rgb)));
}

ColorTransform::ColorSpace ColorTransform::source_space() const { return source_space_; }
ColorTransform::TransferFunction ColorTransform::source_transfer() const { return source_transfer_; }
ColorTransform::ColorSpace ColorTransform::destination_space() const { return destination_space_; }
ColorTransform::TransferFunction ColorTransform::destination_transfer() const { return destination_transfer_; }
int ColorTransform::logc3_ei() const { return logc3_ei_; }
