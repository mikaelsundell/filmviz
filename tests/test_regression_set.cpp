// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "negativeprofile.h"
#include "printprofile.h"
#include "spatialresponsemodel.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Stamp = std::map<std::string, double>;

void
record_triplet(
    Stamp& stamp,
    const std::string& prefix,
    const std::array<float, 3>& values)
{
    static const char* channels[] = {"r", "g", "b"};
    for (int channel = 0; channel < 3; ++channel) {
        stamp[prefix + "." + channels[channel]] = values[channel];
    }
}

void
record_density(
    Stamp& stamp,
    const std::string& prefix,
    const FilmDensity& density)
{
    stamp[prefix + ".r"] = density.red;
    stamp[prefix + ".g"] = density.green;
    stamp[prefix + ".b"] = density.blue;
}

void
record_exposure(
    Stamp& stamp,
    const std::string& prefix,
    const FilmExposure& exposure)
{
    stamp[prefix + ".r"] = exposure.red;
    stamp[prefix + ".g"] = exposure.green;
    stamp[prefix + ".b"] = exposure.blue;
}

bool
run_pipeline_set(
    const std::string& name,
    const FilmPipeline::Settings& settings,
    Stamp& stamp,
    std::string& error)
{
    FilmPipeline pipeline;
    if (!pipeline.initialize(settings)) {
        error = pipeline.error();
        return false;
    }

    const std::array<std::array<float, 3>, 6> samples = {{
        {{0.018f, 0.018f, 0.018f}},
        {{0.18f, 0.18f, 0.18f}},
        {{0.90f, 0.90f, 0.90f}},
        {{0.84f, 0.25f, 0.12f}},
        {{0.28f, 0.084f, 0.048f}},
        {{0.10f, 0.32f, 0.58f}}
    }};

    for (std::size_t index = 0; index < samples.size(); ++index) {
        const FilmPipeline::Result result = pipeline.process(samples[index]);
        if (!result.valid) {
            error = "pipeline sample failed";
            return false;
        }

        const std::string prefix =
            name + ".sample" + std::to_string(index);
        record_exposure(stamp, prefix + ".negative_exposure", result.negative_exposure);
        record_density(stamp, prefix + ".negative_status_m", result.negative_status_m_density);
        record_density(stamp, prefix + ".calibrated_negative", result.calibrated_negative_density);
        record_exposure(stamp, prefix + ".print_exposure", result.print_exposure);
        record_density(stamp, prefix + ".print_density", result.print_density);
        record_triplet(stamp, prefix + ".output_ap0", result.ap0);
        record_triplet(stamp, prefix + ".output_rec709", result.rec709_gamma24);
    }

    return true;
}

bool
run_spatial_set(
    Stamp& stamp,
    std::vector<float>& image,
    int width,
    int height,
    std::string& error)
{
    const auto& negative = NegativeProfileCatalog::default_profile();
    const auto& print = PrintProfileCatalog::default_profile();
    const std::filesystem::path resources(FILMVIZ_TEST_RESOURCE_DIR);
    const std::filesystem::path negative_mtf =
        resources / negative.resource_directory
        / (negative.resource_prefix + "_modulation_transfer_function_curves.csv");
    const std::filesystem::path print_mtf =
        resources / print.resource_directory / print.mtf_filename;

    SpatialResponseModel model;
    if (!model.load(negative_mtf.string(), print_mtf.string())) {
        error = "could not load regression MTF data";
        return false;
    }

    image.assign(
        static_cast<std::size_t>(width * height * 3),
        0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool right = x >= width / 2;
            const bool stripe = ((x / 3) + (y / 2)) % 2 == 0;
            const std::size_t offset =
                static_cast<std::size_t>((y * width + x) * 3);
            image[offset + 0] = right ? 0.78f : (stripe ? 0.14f : 0.28f);
            image[offset + 1] = right ? 0.34f : (stripe ? 0.30f : 0.18f);
            image[offset + 2] = right ? 0.12f : (stripe ? 0.52f : 0.22f);
        }
    }

    SpatialResponseModel::Settings settings;
    settings.image_width_mm = 24.89f;
    settings.negative_amount = 1.0f;
    settings.print_amount = 1.0f;
    settings.gamma24_encoded = false;

    if (!model.apply(image, width, height, settings)) {
        error = "could not apply regression MTF data";
        return false;
    }

    for (int channel = 0; channel < 3; ++channel) {
        double mean = 0.0;
        double energy = 0.0;
        for (std::size_t pixel = 0;
             pixel < static_cast<std::size_t>(width * height);
             ++pixel) {
            const double value = image[pixel * 3u + channel];
            mean += value;
            energy += value * value;
        }
        mean /= static_cast<double>(width * height);
        energy /= static_cast<double>(width * height);
        const char* channels[] = {"r", "g", "b"};
        stamp[std::string("spatial.mean.") + channels[channel]] = mean;
        stamp[std::string("spatial.energy.") + channels[channel]] = energy;
    }

    const std::array<std::array<int, 2>, 4> probes = {{
        {{2, 2}},
        {{width / 2 - 1, height / 2}},
        {{width / 2, height / 2}},
        {{width - 3, height - 3}}
    }};
    for (std::size_t probe = 0; probe < probes.size(); ++probe) {
        const std::size_t offset =
            static_cast<std::size_t>(
                (probes[probe][1] * width + probes[probe][0]) * 3);
        record_triplet(
            stamp,
            "spatial.probe" + std::to_string(probe),
            {{image[offset], image[offset + 1], image[offset + 2]}});
    }

    return true;
}

bool
write_stamp(
    const std::filesystem::path& filename,
    const Stamp& stamp)
{
    std::ofstream file(filename);
    if (!file) {
        return false;
    }
    file << "# FilmViz regression stamp v1\n";
    file << std::setprecision(10);
    for (const auto& entry : stamp) {
        file << entry.first << "," << entry.second << "\n";
    }
    return static_cast<bool>(file);
}

bool
read_stamp(
    const std::filesystem::path& filename,
    Stamp& stamp)
{
    std::ifstream file(filename);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t comma = line.find(',');
        if (comma == std::string::npos) {
            return false;
        }
        try {
            stamp[line.substr(0, comma)] = std::stod(line.substr(comma + 1));
        }
        catch (...) {
            return false;
        }
    }
    return !stamp.empty();
}

bool
write_ppm(
    const std::filesystem::path& filename,
    const std::vector<float>& image,
    int width,
    int height)
{
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        return false;
    }
    file << "P6\n" << width << " " << height << "\n255\n";
    for (float value : image) {
        const unsigned char byte = static_cast<unsigned char>(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        file.write(reinterpret_cast<const char*>(&byte), 1);
    }
    return static_cast<bool>(file);
}

} // namespace

int
main(
    int argc,
    char** argv)
{
    Stamp actual;
    std::string error;

    FilmPipeline::Settings baseline;
    baseline.resources_directory = FILMVIZ_TEST_RESOURCE_DIR;
    if (!run_pipeline_set("baseline", baseline, actual, error)) {
        return test::finish(false, "checked-in regression set: " + error);
    }

    FilmPipeline::Settings shaped = baseline;
    shaped.negative_profile = "kodak-50d";
    shaped.negative_flash_percent = 2.0f;
    shaped.print_flash_percent = 1.0f;
    shaped.printer_light_master = 0.5f;
    shaped.push_pull_stops = 0.35f;
    if (!run_pipeline_set("shaped", shaped, actual, error)) {
        return test::finish(false, "checked-in regression set: " + error);
    }

    constexpr int width = 32;
    constexpr int height = 18;
    std::vector<float> image;
    if (!run_spatial_set(actual, image, width, height, error)) {
        return test::finish(false, "checked-in regression set: " + error);
    }

    const std::filesystem::path directory(FILMVIZ_REGRESSION_DIR);
    const std::filesystem::path stamp_file = directory / "reference_stats.csv";
    const std::filesystem::path image_file = directory / "reference_mtf.ppm";

    if (argc == 2 && std::string(argv[1]) == "--update") {
        const bool written =
            write_stamp(stamp_file, actual)
            && write_ppm(image_file, image, width, height);
        return test::finish(written, "updated checked-in regression set");
    }

    Stamp expected;
    bool passed = test::check(
        read_stamp(stamp_file, expected),
        "regression reference stamp can be read");
    passed &= test::check(
        expected.size() == actual.size(),
        "regression reference has the expected number of values");

    for (const auto& entry : actual) {
        const auto found = expected.find(entry.first);
        passed &= test::check(
            found != expected.end(),
            "regression reference contains " + entry.first);
        if (found != expected.end()) {
            passed &= test::near(
                entry.second,
                found->second,
                2e-5,
                entry.first);
        }
    }

    return test::finish(passed, "checked-in regression set");
}
