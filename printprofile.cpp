// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "printprofile.h"

#include <algorithm>

const std::vector<PrintProfileCatalog::Profile>&
PrintProfileCatalog::profiles()
{
    static const std::vector<Profile> supported = {
        {
            "kodak-2383",
            "Kodak Vision 2383/3383",
            "profiles/kodak_2383",
            "kodak_2383_spectral_sensitivity_curves.csv",
            "kodak_2383_sensitometric_curves.csv",
            "kodak_2383_corrected_spectral_dye_density_curves.csv",
            "kodak_2383_modulation_transfer_function_curves.csv",
            "kodak_2383_diffuse_rms_granularity_curves.csv"
        }
    };

    return supported;
}

const PrintProfileCatalog::Profile*
PrintProfileCatalog::find(
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

const PrintProfileCatalog::Profile&
PrintProfileCatalog::default_profile()
{
    return profiles().front();
}
