// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// OCIO input colour spaces -> scene-linear ACES2065-1 (AP0/D60).
class InputTransform {
public:
    // The first two catalog indices retain compatibility with existing callers.
    enum class Encoding : int { AWG3_LogC3_EI800 = 0, ACES2065_1_Linear = 1 };
    explicit InputTransform(Encoding encoding = Encoding::AWG3_LogC3_EI800,
                            const std::string& resources = "");
    std::array<float, 3> to_ap0(const std::array<float, 3>& rgb) const;
    void apply_rgba(float* pixels, int width, int height, std::ptrdiff_t row_bytes) const;
    Encoding encoding() const;
    static const std::vector<std::string>& profiles(const std::string& resources = "");
    static bool parse_encoding(const std::string& name, Encoding& encoding,
                               const std::string& resources = "");
    static const char* name(Encoding encoding);
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    Encoding encoding_;
};
