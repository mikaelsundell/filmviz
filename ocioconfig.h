// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#pragma once
#include <OpenColorIO/OpenColorIO.h>
#include <string>
#include <vector>
namespace FilmVizOCIO {
struct Catalog {
    OCIO_NAMESPACE::ConstConfigRcPtr config;
    std::vector<std::string> names;
};
const Catalog& catalog(const std::string& resources);
}
