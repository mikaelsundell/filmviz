// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include "filmdata.h"

#include <string>

class CIEObserver
{
public:
    CIEObserver() = default;
    explicit CIEObserver(const std::string& filename);

    bool load(const std::string& filename);
    void clear();

    bool valid() const;
    const std::string& filename() const;

    const SampledCurve& xbar() const;
    const SampledCurve& ybar() const;
    const SampledCurve& zbar() const;

private:
    SampledCurve xbar_;
    SampledCurve ybar_;
    SampledCurve zbar_;
    std::string filename_;
};
