// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#pragma once
#include <string>
#include <vector>

// UI metadata only: pairs select existing OCIO transforms, never compose new ones.
class ColorProfileCatalog {
public:
    struct Entry {
        std::string color_space;
        std::string transfer_function;
        std::string profile;
        int profile_index;
        std::vector<int> equivalent_indices;
    };
    // Alphabetical by color space, then transfer function (case-insensitive).
    // profile_index and equivalent_indices retain the underlying config ordering.
    static std::vector<Entry> profiles(bool output, const std::string& resources = "");
    static std::vector<std::string> color_spaces(const std::vector<Entry>& entries);
    static const Entry* find(const std::vector<Entry>& entries, int profile_index);
    static std::string resolve(bool output, const std::string& color_space,
        const std::string& transfer_function, const std::string& resources = "");
};
