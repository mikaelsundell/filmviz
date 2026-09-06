// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "negativeprofile.h"

#include <algorithm>

const std::vector<NegativeProfileCatalog::Profile>&
NegativeProfileCatalog::profiles()
{
    static const std::vector<Profile> supported = {
        {
            "verita-200d",
            "Kodak Verita 200D 5206/7206",
            "profiles/verita_200d",
            "kodak_verita_200d"
        },
        {
            "kodak-50d",
            "Kodak Vision3 50D 5203/7203",
            "profiles/kodak_50d",
            "kodak_50d"
        }
    };

    return supported;
}

const NegativeProfileCatalog::Profile*
NegativeProfileCatalog::find(
    const std::string& identifier)
{
    const auto& supported = profiles();
    const auto found =
        std::find_if(
            supported.begin(),
            supported.end(),
            [&](const Profile& profile) {
                return profile.identifier == identifier;
            });

    return
        found != supported.end()
            ? &*found
            : nullptr;
}

const NegativeProfileCatalog::Profile&
NegativeProfileCatalog::default_profile()
{
    return profiles().front();
}
