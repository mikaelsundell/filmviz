// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Viewed AP0 -> selected output colour space. No display/view transform is added.
class OutputTransform {
public:
    // Preserve the original output indices for saved settings and C++ callers.
    enum class Encoding : int { AP0Linear = 0, Rec709Gamma24 = 1 };
    explicit OutputTransform(Encoding encoding = Encoding::AP0Linear, const std::string& resources = "");
    std::array<float, 3> from_ap0(const std::array<float, 3>& rgb) const;
    void apply(float* pixels, int width, int height, int channels, std::ptrdiff_t row_bytes) const;
    static const std::vector<std::string>& profiles(const std::string& resources = "");
    static bool parse_encoding(const std::string& name, Encoding& encoding, const std::string& resources = "");
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    Encoding encoding_;
};
