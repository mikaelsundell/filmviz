// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace test {

inline bool
check(
    bool condition,
    const std::string& message)
{
    if (!condition) {
        std::cerr
            << "[FAIL] "
            << message
            << "\n";
    }

    return condition;
}

inline bool
near(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (!std::isfinite(actual)
        || std::abs(actual - expected) > tolerance) {

        std::cerr
            << "[FAIL] "
            << message
            << ": expected "
            << expected
            << " +/- "
            << tolerance
            << ", got "
            << actual
            << "\n";

        return false;
    }

    return true;
}

inline int
finish(
    bool passed,
    const std::string& name)
{
    if (passed) {
        std::cout
            << "[PASS] "
            << name
            << "\n";
    }

    return passed
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

} // namespace test
