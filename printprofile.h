// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <string>
#include <vector>

// Canonical metadata for print-film profiles supported by FilmPipeline.
class PrintProfileCatalog
{
public:
    struct Profile
    {
        std::string identifier;
        std::string display_name;
        std::string resource_directory;
        std::string sensitivity_filename;
        std::string characteristic_filename;
        std::string dye_density_filename;
        std::string mtf_filename;
        std::string granularity_filename;
    };

    static const std::vector<Profile>& profiles();

    static const Profile* find(
        const std::string& identifier);

    static const Profile& default_profile();
};
