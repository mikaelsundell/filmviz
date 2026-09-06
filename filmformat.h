// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <string>
#include <vector>

// Physical active-image widths used to map measured cycles/mm responses into
// image-pixel frequencies. Exact camera apertures and crops vary, so every
// preset resolves to a shared physical width and Custom remains available.
class FilmFormatCatalog
{
public:
    struct Format
    {
        std::string identifier;
        std::string display_name;
        float image_width_mm = 0.0f;
    };

    static const std::vector<Format>& formats();

    static const Format* find(
        const std::string& identifier);

    static const Format& default_format();
};
