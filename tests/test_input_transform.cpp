// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "inputtransform.h"
#include "test_common.h"

#include <array>
#include <cmath>
#include <string>

int
main()
{
    bool passed = true;

    InputTransform::Encoding encoding;

    passed &= test::check(
        InputTransform::parse_encoding(
            "awg3-logc3-ei800",
            encoding)
        && encoding == InputTransform::Encoding::AWG3_LogC3_EI800,
        "canonical ARRI encoding parses");

    passed &= test::check(
        InputTransform::parse_encoding(
            "ap0-linear",
            encoding)
        && encoding == InputTransform::Encoding::ACES2065_1_Linear,
        "canonical AP0 encoding parses");

    passed &= test::check(
        !InputTransform::parse_encoding(
            "unknown",
            encoding),
        "unknown encoding is rejected");

    const std::array<float, 3> source = {{
        -0.25f,
        0.18f,
        1.25f
    }};

    const InputTransform ap0_transform(
        InputTransform::Encoding::ACES2065_1_Linear);

    const std::array<float, 3> identity =
        ap0_transform.to_ap0(
            source);

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            identity[channel],
            source[channel],
            0.0,
            "AP0 input remains unchanged");
    }

    // ARRI LogC3 EI800 encodes 18% scene-linear neutral near 0.391007.
    const InputTransform logc_transform(
        InputTransform::Encoding::AWG3_LogC3_EI800);

    const std::array<float, 3> middle_gray =
        logc_transform.to_ap0(
            {{0.391007f, 0.391007f, 0.391007f}});

    for (float value : middle_gray) {
        passed &= test::near(
            value,
            0.18,
            2e-3,
            "LogC3 middle gray maps to AP0 middle gray");
    }

    return
        test::finish(
            passed,
            "input transform");
}
