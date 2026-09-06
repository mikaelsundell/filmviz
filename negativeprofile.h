// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <string>
#include <vector>

// Canonical metadata for camera-negative profiles supported by FilmPipeline.
// Interfaces consume this catalog instead of duplicating identifiers, display
// names, or resource-layout knowledge.
class NegativeProfileCatalog
{
public:
    struct Profile
    {
        std::string identifier;
        std::string display_name;
        std::string resource_directory;
        std::string resource_prefix;
    };

    static const std::vector<Profile>& profiles();

    static const Profile* find(
        const std::string& identifier);

    static const Profile& default_profile();
};
