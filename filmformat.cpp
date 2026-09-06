// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmformat.h"

#include <algorithm>

const std::vector<FilmFormatCatalog::Format>&
FilmFormatCatalog::formats()
{
    static const std::vector<Format> supported = {
        {"regular-8", "Regular 8", 4.80f},
        {"super-8", "Super 8", 5.79f},
        {"16mm", "16 mm", 10.26f},
        {"super-16", "Super 16", 12.52f},
        {"35mm", "35 mm", 21.95f},
        {"super-35", "Super 35", 24.89f},
        {"65mm", "65 mm", 52.48f},
        {"custom", "Custom", 24.89f}
    };

    return supported;
}

const FilmFormatCatalog::Format*
FilmFormatCatalog::find(
    const std::string& identifier)
{
    const auto& supported = formats();
    const auto found =
        std::find_if(
            supported.begin(),
            supported.end(),
            [&](const Format& format) {
                return format.identifier == identifier;
            });

    return
        found != supported.end()
            ? &*found
            : nullptr;
}

const FilmFormatCatalog::Format&
FilmFormatCatalog::default_format()
{
    const Format* format = find("super-35");
    return format ? *format : formats().front();
}
