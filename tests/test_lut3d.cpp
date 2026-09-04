// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "lut3d.h"
#include "test_common.h"

#include <array>

namespace {

bool
affine_pipeline(
    const Lut3D::RGB& input,
    Lut3D::RGB& output)
{
    output = {{
        0.10f + 0.50f * input[0] + 0.10f * input[1],
        0.20f + 0.25f * input[1] + 0.05f * input[2],
        0.05f + 0.30f * input[0] + 0.40f * input[2]
    }};

    return true;
}

} // namespace

int
main()
{
    bool passed = true;
    Lut3D lut;

    passed &= test::check(
        !lut.generate(
            1,
            affine_pipeline),
        "LUT sizes below two are rejected");

    passed &= test::check(
        lut.generate(
            5,
            affine_pipeline),
        "LUT generation succeeds");

    passed &= test::check(
        lut.valid()
        && lut.size() == 5,
        "generated LUT reports the correct shape");

    const Lut3D::RGB input = {{0.17f, 0.43f, 0.81f}};
    Lut3D::RGB expected;
    affine_pipeline(
        input,
        expected);

    const Lut3D::RGB sampled =
        lut.sample_trilinear(
            input);

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            sampled[channel],
            expected[channel],
            1e-6,
            "trilinear sampling preserves an affine pipeline");
    }

    const Lut3D::Validation validation =
        lut.validate(
            affine_pipeline,
            5);

    passed &= test::check(
        validation.valid
        && validation.samples == 125
        && validation.max_abs_error < 1e-6,
        "direct-versus-LUT validation is accurate");

    return
        test::finish(
            passed,
            "3D LUT pipeline sampling");
}
