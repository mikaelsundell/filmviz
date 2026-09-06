// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizofxcache.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{

constexpr char kMagic[8] = {'F','V','O','F','X','C','H','E'};
// Increment whenever a core pipeline change alters cached transform values,
// even if the binary payload layout itself is unchanged. Version 4 adopts the
// exposure-separated rgb2spec reconstruction used by FilmPipeline. Version 5
// adds negative/print flashing and linked master printer timing.
constexpr std::uint32_t kVersion = 5u;
constexpr std::uint32_t kHasNegativeExposure = 1u << 0u;

void
hash_bytes(
    std::uint64_t& hash,
    const void* data,
    std::size_t size)
{
    const auto* bytes =
        static_cast<const unsigned char*>(data);

    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

template <typename T>
void
hash_value(
    std::uint64_t& hash,
    const T& value)
{
    hash_bytes(hash, &value, sizeof(value));
}

void
hash_string(
    std::uint64_t& hash,
    const std::string& value)
{
    hash_bytes(hash, value.data(), value.size());
    const unsigned char terminator = 0;
    hash_bytes(hash, &terminator, 1);
}

template <typename T>
bool
write_value(
    std::ofstream& stream,
    const T& value)
{
    stream.write(
        reinterpret_cast<const char*>(&value),
        sizeof(T));
    return static_cast<bool>(stream);
}

template <typename T>
bool
read_value(
    std::ifstream& stream,
    T& value)
{
    stream.read(
        reinterpret_cast<char*>(&value),
        sizeof(T));
    return static_cast<bool>(stream);
}

} // namespace

bool
FilmVizOfxTransformKey::operator==(
    const FilmVizOfxTransformKey& other) const
{
    return
        negative_profile == other.negative_profile
        && print_profile == other.print_profile
        && input_profile == other.input_profile
        && output_profile == other.output_profile
        && lut_size == other.lut_size
        && push_pull_stops == other.push_pull_stops
        && negative_flash_percent == other.negative_flash_percent
        && print_flash_percent == other.print_flash_percent
        && middle_gray == other.middle_gray
        && printer_temperature == other.printer_temperature
        && negative_bleach_bypass == other.negative_bleach_bypass
        && print_bleach_bypass == other.print_bleach_bypass
        && printer_light_red == other.printer_light_red
        && printer_light_green == other.printer_light_green
        && printer_light_blue == other.printer_light_blue
        && printer_light_master == other.printer_light_master;
}

std::uint64_t
filmviz_ofx_transform_hash(
    const FilmVizOfxTransformKey& key)
{
    std::uint64_t hash = 1469598103934665603ull;

    hash_string(hash, key.negative_profile);
    hash_string(hash, key.print_profile);
    hash_value(hash, key.input_profile);
    hash_value(hash, key.output_profile);
    hash_value(hash, key.lut_size);
    hash_value(hash, key.push_pull_stops);
    hash_value(hash, key.negative_flash_percent);
    hash_value(hash, key.print_flash_percent);
    hash_value(hash, key.middle_gray);
    hash_value(hash, key.printer_temperature);
    hash_value(hash, key.negative_bleach_bypass);
    hash_value(hash, key.print_bleach_bypass);
    hash_value(hash, key.printer_light_red);
    hash_value(hash, key.printer_light_green);
    hash_value(hash, key.printer_light_blue);
    hash_value(hash, key.printer_light_master);

    return hash;
}

std::string
filmviz_ofx_transform_name(
    const FilmVizOfxTransformKey& key)
{
    std::ostringstream stream;
    stream
        << "i" << key.input_profile
        << "_" << key.negative_profile
        << "_" << key.print_profile
        << "_o" << key.output_profile
        << "_l" << key.lut_size
        << "_"
        << std::hex
        << std::setw(16)
        << std::setfill('0')
        << filmviz_ofx_transform_hash(key);

    return stream.str();
}

std::string
filmviz_ofx_prebaked_filename(
    const std::string& directory,
    const FilmVizOfxTransformKey& key)
{
    return
        (std::filesystem::path(directory)
         / (filmviz_ofx_transform_name(key) + ".fvcache"))
            .string();
}

bool
filmviz_ofx_save_prebaked(
    const std::string& filename,
    const FilmVizOfxTransformKey& key,
    const FilmVizOfxPrebakedData& data,
    std::string& error)
{
    error.clear();

    const std::size_t expected =
        static_cast<std::size_t>(data.lut_size)
        * static_cast<std::size_t>(data.lut_size)
        * static_cast<std::size_t>(data.lut_size);

    if (data.lut_size < 2
        || data.color_lut.size() != expected
        || data.grain_field.size() != expected
        || (!data.negative_exposure_lut.empty()
            && data.negative_exposure_lut.size() != expected)) {

        error = "invalid FilmViz OFX prebaked cache data";
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(
        std::filesystem::path(filename).parent_path(),
        filesystem_error);

    std::ofstream stream(
        filename,
        std::ios::binary | std::ios::trunc);

    if (!stream) {
        error = "could not open FilmViz OFX prebaked cache for writing";
        return false;
    }

    stream.write(kMagic, sizeof(kMagic));

    const std::uint64_t key_hash =
        filmviz_ofx_transform_hash(key);

    const std::uint32_t flags =
        data.negative_exposure_lut.empty()
            ? 0u
            : kHasNegativeExposure;

    const std::uint64_t count =
        static_cast<std::uint64_t>(expected);

    if (!write_value(stream, kVersion)
        || !write_value(stream, key_hash)
        || !write_value(stream, data.lut_size)
        || !write_value(stream, flags)
        || !write_value(stream, count)) {

        error = "could not write FilmViz OFX prebaked cache header";
        return false;
    }

    stream.write(
        reinterpret_cast<const char*>(data.development_log_min.data()),
        static_cast<std::streamsize>(3 * sizeof(float)));
    stream.write(
        reinterpret_cast<const char*>(data.development_log_max.data()),
        static_cast<std::streamsize>(3 * sizeof(float)));

    stream.write(
        reinterpret_cast<const char*>(data.color_lut.data()),
        static_cast<std::streamsize>(data.color_lut.size() * sizeof(FilmVizOfxCachedRGB)));

    stream.write(
        reinterpret_cast<const char*>(data.grain_field.data()),
        static_cast<std::streamsize>(data.grain_field.size() * sizeof(FilmVizOfxCachedGrain)));

    if (!data.negative_exposure_lut.empty()) {
        stream.write(
            reinterpret_cast<const char*>(data.negative_exposure_lut.data()),
            static_cast<std::streamsize>(data.negative_exposure_lut.size() * sizeof(FilmVizOfxCachedRGB)));
    }

    if (!stream) {
        error = "could not write FilmViz OFX prebaked cache payload";
        return false;
    }

    return true;
}

bool
filmviz_ofx_load_prebaked(
    const std::string& filename,
    const FilmVizOfxTransformKey& key,
    FilmVizOfxPrebakedData& data,
    std::string& error)
{
    error.clear();
    data = FilmVizOfxPrebakedData();

    std::ifstream stream(
        filename,
        std::ios::binary);

    if (!stream) {
        return false;
    }

    char magic[sizeof(kMagic)] = {};
    stream.read(magic, sizeof(magic));

    std::uint32_t version = 0;
    std::uint64_t key_hash = 0;
    int lut_size = 0;
    std::uint32_t flags = 0;
    std::uint64_t count = 0;

    if (!stream
        || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0
        || !read_value(stream, version)
        || !read_value(stream, key_hash)
        || !read_value(stream, lut_size)
        || !read_value(stream, flags)
        || !read_value(stream, count)
        || version != kVersion
        || key_hash != filmviz_ofx_transform_hash(key)
        || lut_size != key.lut_size) {

        error = "invalid or stale FilmViz OFX prebaked cache header";
        return false;
    }

    const std::size_t expected =
        static_cast<std::size_t>(lut_size)
        * static_cast<std::size_t>(lut_size)
        * static_cast<std::size_t>(lut_size);

    if (count != static_cast<std::uint64_t>(expected)) {
        error = "invalid FilmViz OFX prebaked cache size";
        return false;
    }

    data.lut_size = lut_size;

    stream.read(
        reinterpret_cast<char*>(data.development_log_min.data()),
        static_cast<std::streamsize>(3 * sizeof(float)));
    stream.read(
        reinterpret_cast<char*>(data.development_log_max.data()),
        static_cast<std::streamsize>(3 * sizeof(float)));

    if (!stream) {
        error = "could not read FilmViz OFX prebaked exposure domain";
        return false;
    }

    data.color_lut.resize(expected);
    data.grain_field.resize(expected);

    stream.read(
        reinterpret_cast<char*>(data.color_lut.data()),
        static_cast<std::streamsize>(data.color_lut.size() * sizeof(FilmVizOfxCachedRGB)));

    stream.read(
        reinterpret_cast<char*>(data.grain_field.data()),
        static_cast<std::streamsize>(data.grain_field.size() * sizeof(FilmVizOfxCachedGrain)));

    if ((flags & kHasNegativeExposure) != 0u) {
        data.negative_exposure_lut.resize(expected);
        stream.read(
            reinterpret_cast<char*>(data.negative_exposure_lut.data()),
            static_cast<std::streamsize>(data.negative_exposure_lut.size() * sizeof(FilmVizOfxCachedRGB)));
    }

    if (!stream) {
        error = "could not read FilmViz OFX prebaked cache payload";
        data = FilmVizOfxPrebakedData();
        return false;
    }

    return true;
}
