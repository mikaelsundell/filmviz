// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmstock.h"

#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <cmath>
#include <sstream>
#include <vector>

namespace {


CurveValidationResult
validate_curve(
    const std::string& name,
    const SampledCurve& curve,
    float tolerance)
{
    CurveValidationResult result;
    result.name = name;
    result.sample_count = curve.x.size();

    if (!curve.valid()) {
        result.passed = false;
        result.max_abs_error =
            std::numeric_limits<float>::infinity();
        return result;
    }

    for (std::size_t i = 0;
         i < curve.x.size();
         ++i) {

        const float sampled =
            curve.sample(
                curve.x[i],
                std::numeric_limits<float>::quiet_NaN());

        const float error =
            std::abs(
                sampled - curve.y[i]);

        if (error > result.max_abs_error) {
            result.max_abs_error = error;
            result.worst_index = i;
            result.x = curve.x[i];
            result.expected = curve.y[i];
            result.sampled = sampled;
        }
    }

    result.passed =
        std::isfinite(
            result.max_abs_error)
        && result.max_abs_error <= tolerance;

    return result;
}


std::vector<std::string>
split_csv_line(const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }

    return fields;
}

bool
parse_float(
    const std::string& text,
    float& value)
{
    if (text.empty()) {
        return false;
    }

    try {
        std::size_t pos = 0;
        value = std::stof(text, &pos);

        while (pos < text.size()
               && std::isspace(
                   static_cast<unsigned char>(
                       text[pos]))) {
            ++pos;
        }

        return pos == text.size();
    }
    catch (...) {
        return false;
    }
}

} // namespace

FilmStock::FilmStock(
    const std::string& name)
    : name_(name)
{
}

bool
FilmStock::load(
    const std::string& sensitivity_filename,
    const std::string& characteristic_filename)
{
    sensitivity_ = FilmSpectralSensitivity();
    characteristic_ = FilmCharacteristicCurves();

    if (!load_sensitivity(
            sensitivity_filename)) {
        return false;
    }

    if (!load_characteristic(
            characteristic_filename)) {
        return false;
    }

    return valid();
}

bool
FilmStock::valid() const
{
    return
        sensitivity_.blue_sensitive_log.valid()
        && sensitivity_.green_sensitive_log.valid()
        && sensitivity_.red_sensitive_log.valid()
        && characteristic_.blue_density.valid()
        && characteristic_.green_density.valid()
        && characteristic_.red_density.valid();
}

const std::string&
FilmStock::name() const
{
    return name_;
}

const FilmSpectralSensitivity&
FilmStock::sensitivity() const
{
    return sensitivity_;
}

const FilmCharacteristicCurves&
FilmStock::characteristic() const
{
    return characteristic_;
}


std::vector<CurveValidationResult>
FilmStock::validate_interpolation(
    float tolerance) const
{
    std::vector<CurveValidationResult> results;

    results.push_back(
        validate_curve(
            "red spectral sensitivity",
            sensitivity_.red_sensitive_log,
            tolerance));

    results.push_back(
        validate_curve(
            "green spectral sensitivity",
            sensitivity_.green_sensitive_log,
            tolerance));

    results.push_back(
        validate_curve(
            "blue spectral sensitivity",
            sensitivity_.blue_sensitive_log,
            tolerance));

    results.push_back(
        validate_curve(
            "red characteristic",
            characteristic_.red_density,
            tolerance));

    results.push_back(
        validate_curve(
            "green characteristic",
            characteristic_.green_density,
            tolerance));

    results.push_back(
        validate_curve(
            "blue characteristic",
            characteristic_.blue_density,
            tolerance));

    return results;
}

bool
FilmStock::load_sensitivity(
    const std::string& filename)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open spectral sensitivity CSV: "
            << filename
            << std::endl;
        return false;
    }

    std::string line;

    // Header.
    if (!std::getline(file, line)) {
        std::cerr
            << "error: empty spectral sensitivity CSV: "
            << filename
            << std::endl;
        return false;
    }

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.size() < 4) {
            continue;
        }

        float wavelength = 0.0f;

        if (!parse_float(
                fields[0],
                wavelength)) {
            continue;
        }

        float value = 0.0f;

        // yellow-forming -> blue-sensitive
        if (parse_float(fields[1], value)) {
            sensitivity_.blue_sensitive_log.x.push_back(
                wavelength);
            sensitivity_.blue_sensitive_log.y.push_back(
                value);
        }

        // magenta-forming -> green-sensitive
        if (parse_float(fields[2], value)) {
            sensitivity_.green_sensitive_log.x.push_back(
                wavelength);
            sensitivity_.green_sensitive_log.y.push_back(
                value);
        }

        // cyan-forming -> red-sensitive
        if (parse_float(fields[3], value)) {
            sensitivity_.red_sensitive_log.x.push_back(
                wavelength);
            sensitivity_.red_sensitive_log.y.push_back(
                value);
        }
    }

    if (!sensitivity_.blue_sensitive_log.valid()
        || !sensitivity_.green_sensitive_log.valid()
        || !sensitivity_.red_sensitive_log.valid()) {

        std::cerr
            << "error: spectral sensitivity CSV did not contain "
            << "all three film records"
            << std::endl;

        return false;
    }

    return true;
}

bool
FilmStock::load_characteristic(
    const std::string& filename)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open characteristic curve CSV: "
            << filename
            << std::endl;
        return false;
    }

    std::string line;

    // Header.
    if (!std::getline(file, line)) {
        std::cerr
            << "error: empty characteristic curve CSV: "
            << filename
            << std::endl;
        return false;
    }

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.size() < 5) {
            continue;
        }

        float log_exposure = 0.0f;
        float high_density = 0.0f;
        float mid_density = 0.0f;
        float low_density = 0.0f;

        if (!parse_float(
                fields[1],
                log_exposure)
            || !parse_float(
                fields[2],
                high_density)
            || !parse_float(
                fields[3],
                mid_density)
            || !parse_float(
                fields[4],
                low_density)) {
            continue;
        }

        // Same validated validated record-to-density mapping mapping:
        //
        // high -> blue-sensitive
        // mid  -> green-sensitive
        // low  -> red-sensitive
        characteristic_.blue_density.x.push_back(
            log_exposure);
        characteristic_.blue_density.y.push_back(
            high_density);

        characteristic_.green_density.x.push_back(
            log_exposure);
        characteristic_.green_density.y.push_back(
            mid_density);

        characteristic_.red_density.x.push_back(
            log_exposure);
        characteristic_.red_density.y.push_back(
            low_density);
    }

    if (!characteristic_.blue_density.valid()
        || !characteristic_.green_density.valid()
        || !characteristic_.red_density.valid()) {

        std::cerr
            << "error: characteristic CSV did not contain "
            << "all three density curves"
            << std::endl;

        return false;
    }

    return true;
}
