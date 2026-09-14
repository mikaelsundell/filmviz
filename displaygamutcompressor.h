// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <array>

class DisplayGamutCompressor {
public:
    static std::array<float, 3> compress_rec709(const std::array<float, 3>& linear_rgb);
};
