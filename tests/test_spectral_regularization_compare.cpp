// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "spectralreconstructor.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Family
{
    const char* name;
    std::array<float, 3> ap0;
};

struct Model
{
    std::string name;
    std::filesystem::path filename;
};

double
nrmse(
    const std::array<float, 3>& actual,
    const std::array<float, 3>& expected)
{
    double numerator = 0.0;
    double denominator = 0.0;

    for (int i = 0; i < 3; ++i) {
        const double d =
            static_cast<double>(actual[i])
            - static_cast<double>(expected[i]);

        numerator += d * d;
        denominator +=
            static_cast<double>(expected[i])
            * static_cast<double>(expected[i]);
    }

    return
        std::sqrt(
            numerator
            / std::max(
                denominator,
                1e-20));
}

double
band_energy(
    const SpectralReconstructor& reconstructor,
    const SpectralReconstructor::Spectrum& spectrum,
    float minimum,
    float maximum)
{
    double sum = 0.0;

    for (float wavelength = minimum;
         wavelength <= maximum + 0.001f;
         wavelength += 5.0f) {

        sum +=
            std::max(
                0.0f,
                reconstructor.evaluate(
                    spectrum,
                    wavelength));
    }

    return sum;
}

} // namespace

int
main()
{
    namespace fs = std::filesystem;

    const fs::path resources(
        FILMVIZ_TEST_RESOURCE_DIR);

    const fs::path reconstruction =
        resources
        / "spectral/reconstruction";

    const std::vector<Model> models = {
        {
            "production",
            reconstruction
                / "ACES2065_1.spec"
        },
        {
            "smooth_w1_r32",
            reconstruction
                / "ACES2065_1_smooth_w1_r32.spec"
        },
        {
            "smooth_w10_r32",
            reconstruction
                / "ACES2065_1_smooth_w10_r32.spec"
        },
        {
            "smooth_w100_r32",
            reconstruction
                / "ACES2065_1_smooth_w100_r32.spec"
        }
    };

    const std::vector<Family> families = {
        {
            "neutral",
            {{0.18f, 0.18f, 0.18f}}
        },
        {
            "orange_red",
            {{0.18f, 0.08f, 0.01f}}
        },
        {
            "warm_red",
            {{0.18f, 0.045f, 0.018f}}
        },
        {
            "red",
            {{0.18f, 0.018f, 0.009f}}
        },
        {
            "deep_red",
            {{0.18f, 0.0036f, 0.0018f}}
        }
    };

    bool passed = true;

    std::cout
        << std::setprecision(9);

    for (const Model& model : models) {
        SpectralReconstructor reconstructor(
            model.filename.string());

        passed &= test::check(
            reconstructor.valid()
            && reconstructor.has_forward_model(),
            std::string("reconstruction model loads: ")
                + model.name);

        if (!reconstructor.valid()
            || !reconstructor.has_forward_model()) {
            continue;
        }

        std::cout
            << "\nModel: "
            << model.name
            << "\n";

        for (const Family& family : families) {
            // Use Lookup deliberately. rgb2spec_fetch_opt() is still the old
            // Lab-only refinement and would partially undo the experimental
            // smoothness objective stored in these tables.
            const auto spectrum =
                reconstructor.reconstruct(
                    family.ap0,
                    SpectralReconstructor::Method::Lookup);

            const auto forward =
                reconstructor.forward_rgb(
                    spectrum);

            const double short_energy =
                band_energy(
                    reconstructor,
                    spectrum,
                    380.0f,
                    450.0f);

            const double middle_energy =
                band_energy(
                    reconstructor,
                    spectrum,
                    500.0f,
                    600.0f);

            const double long_energy =
                band_energy(
                    reconstructor,
                    spectrum,
                    600.0f,
                    700.0f);

            const double short_long =
                short_energy
                / std::max(
                    long_energy,
                    1e-20);

            std::cout
                << "  "
                << std::setw(10)
                << family.name
                << " NRMSE="
                << nrmse(
                    forward,
                    family.ap0)
                << " short/long="
                << short_long
                << " bands[S/M/L]=(" 
                << short_energy
                << ", "
                << middle_energy
                << ", "
                << long_energy
                << ") S400="
                << reconstructor.evaluate(
                    spectrum,
                    400.0f)
                << " S420="
                << reconstructor.evaluate(
                    spectrum,
                    420.0f)
                << " S550="
                << reconstructor.evaluate(
                    spectrum,
                    550.0f)
                << " S650="
                << reconstructor.evaluate(
                    spectrum,
                    650.0f)
                << "\n";
        }
    }

    return
        test::finish(
            passed,
            "spectral regularization compare");
}
