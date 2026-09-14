// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmdirectdata.h"

bool
FilmDirectData::valid() const
{
    const std::size_t samples = spectral_count;
    return samples > 0 && rgb2spec_resolution >= 2 && rgb2spec_scale.size() == rgb2spec_resolution
           && rgb2spec_data.size()
                  == static_cast<std::size_t>(rgb2spec_resolution) * rgb2spec_resolution * rgb2spec_resolution * 9u
           && negative_exposure_samples.size() == samples * 4u && negative_density_samples.size() == samples * 4u
           && status_m_samples.size() == samples * 4u && print_exposure_samples.size() == samples * 4u
           && print_density_samples.size() == samples * 4u && viewer_ap0_samples.size() == samples * 4u
           && negative_granularity_samples.size() >= 8u
           && print_granularity_samples.size() == negative_granularity_samples.size() && !characteristic_points.empty();
}
