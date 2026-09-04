// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <array>
#include <string>

// Camera/input encoding transforms used before the spectral pipeline.
//
// Production FilmViz currently supports the Final1d reference input:
// ARRI Wide Gamut 3 / LogC3 EI800. The output of this class is always
// linear ACES2065-1 (AP0, D60), which is the scene RGB domain used by the
// spectral reconstructor.
class InputTransform
{
public:
    enum class Encoding
    {
        AWG3_LogC3_EI800,
        ACES2065_1_Linear
    };

    explicit InputTransform(
        Encoding encoding = Encoding::AWG3_LogC3_EI800);

    std::array<float, 3> to_ap0(
        const std::array<float, 3>& encoded_rgb) const;

    Encoding encoding() const;

    static bool parse_encoding(
        const std::string& name,
        Encoding& encoding);

    static const char* name(
        Encoding encoding);

private:
    static std::array<float, 3> arri_logc3_ei800_to_linear(
        const std::array<float, 3>& value);

    static std::array<float, 3> awg3_linear_to_ap0(
        const std::array<float, 3>& awg3);

    Encoding encoding_;
};
