// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <array>
#include <string>

// Matrix + transfer-function colour conversion used by diagnostics and simple
// display previews. This class does NOT implement the ACES RRT or an ACES ODT.
class ColorTransform
{
public:
    enum class ColorSpace
    {
        ACES2065_1,
        AWG3,
        Rec709
    };

    enum class TransferFunction
    {
        Linear,
        LogC3,
        Gamma24
    };

    ColorTransform(
        ColorSpace source_space,
        TransferFunction source_transfer,
        ColorSpace destination_space,
        TransferFunction destination_transfer,
        int logc3_ei = 800);

    static bool parse_color_space(
        const std::string& name,
        ColorSpace& color_space);

    static bool parse_transfer_function(
        const std::string& name,
        TransferFunction& transfer_function);

    static const char* color_space_name(ColorSpace color_space);
    static const char* transfer_function_name(TransferFunction transfer_function);
    static bool supported_logc3_ei(int ei);

    std::array<float, 3> transform(
        const std::array<float, 3>& rgb) const;

    std::array<float, 3> decode_transfer(
        const std::array<float, 3>& encoded_rgb) const;

    std::array<float, 3> transform_linear(
        const std::array<float, 3>& linear_rgb) const;

    std::array<float, 3> encode_transfer(
        const std::array<float, 3>& linear_rgb) const;

    ColorSpace source_space() const;
    TransferFunction source_transfer() const;
    ColorSpace destination_space() const;
    TransferFunction destination_transfer() const;
    int logc3_ei() const;

private:
    ColorSpace source_space_;
    TransferFunction source_transfer_;
    ColorSpace destination_space_;
    TransferFunction destination_transfer_;
    int logc3_ei_;
};
