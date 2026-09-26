// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#include "ocioconfig.h"
#include <OpenColorIO/OpenColorIO.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace OCIO = OCIO_NAMESPACE;
namespace FilmVizOCIO {
constexpr const char* config_file = "studio-config-v4.0.0_aces-v2.0_ocio-v2.5.ocio";
std::string config_path(const std::string& resources)
{
    namespace fs = std::filesystem;
    if (!resources.empty()) return (fs::path(resources) / "configs" / config_file).string();
    if (const char* root = std::getenv("FILMVIZ_RESOURCES"))
        if (*root) return (fs::path(root) / "configs" / config_file).string();
    fs::path binary;
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&config_path), &module)) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, MAX_PATH)) binary = path;
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&config_path), &info) && info.dli_fname) binary = info.dli_fname;
#endif
    for (const auto& root : {binary.parent_path() / "resources",
                            binary.parent_path().parent_path() / "Resources" / "filmviz",
                            fs::path("resources"), fs::path(FILMVIZ_SOURCE_RESOURCE_DIR)}) {
        const auto path = root / "configs" / config_file;
        if (fs::exists(path)) return path.string();
    }
    throw std::runtime_error("FilmViz OCIO config not found: " + std::string(config_file));
}
const Catalog& catalog(const std::string& resources)
{
    static std::mutex mutex;
    static std::map<std::string, Catalog> catalogs;
    const auto path = config_path(resources);
    const std::lock_guard<std::mutex> lock(mutex);
    auto found = catalogs.find(path);
    if (found != catalogs.end()) return found->second;
    Catalog value;
    value.config = OCIO::Config::CreateFromFile(path.c_str());
    value.names = {"ARRI LogC3 (EI800)", "ACES2065-1"};
    const int count = value.config->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ALL);
    for (int i = 0; i < count; ++i) {
        const std::string name = value.config->getColorSpaceNameByIndex(
            OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ALL, i);
        if (std::find(value.names.begin(), value.names.end(), name) == value.names.end())
            value.names.push_back(name);
    }
    return catalogs.emplace(path, std::move(value)).first->second;
}
}
