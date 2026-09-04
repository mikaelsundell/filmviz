// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "printfilmstock.h"

#include <cmath>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

namespace {

std::vector<std::string>
split_csv_line(
    const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }

    // std::getline() does not preserve a final trailing empty field. None of
    // the current files require it for parsing, but retaining blank interior
    // fields is essential because missing Kodak samples must remain missing.
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

void
append_if_valid(
    const std::vector<std::string>& fields,
    std::size_t field_index,
    float x,
    SampledCurve& curve)
{
    if (field_index >= fields.size()) {
        return;
    }

    float value = 0.0f;

    if (!parse_float(
            fields[field_index],
            value)) {
        return;
    }

    curve.x.push_back(x);
    curve.y.push_back(value);
}

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

bool
read_header(
    std::ifstream& file,
    const std::string& filename,
    const char* description)
{
    std::string line;

    if (!std::getline(file, line)) {
        std::cerr
            << "error: empty "
            << description
            << " CSV: "
            << filename
            << std::endl;
        return false;
    }

    return true;
}

} // namespace

PrintFilmStock::PrintFilmStock(
    const std::string& name)
    : name_(name)
{
}

bool
PrintFilmStock::load(
    const std::string& sensitivity_filename,
    const std::string& characteristic_filename,
    const std::string& dye_density_filename,
    const std::string& mtf_filename,
    const std::string& granularity_filename)
{
    sensitivity_ = SpectralSensitivity();
    characteristic_ = CharacteristicCurves();
    dye_density_ = SpectralDyeDensity();
    mtf_ = ModulationTransferFunction();
    granularity_ = DiffuseRMSGranularity();

    if (!load_sensitivity(
            sensitivity_filename)) {
        return false;
    }

    if (!load_characteristic(
            characteristic_filename)) {
        return false;
    }

    if (!load_dye_density(
            dye_density_filename)) {
        return false;
    }

    if (!load_mtf(
            mtf_filename)) {
        return false;
    }

    if (!load_granularity(
            granularity_filename)) {
        return false;
    }

    return valid();
}

bool
PrintFilmStock::valid() const
{
    return
        sensitivity_.yellow_forming_log.valid()
        && sensitivity_.magenta_forming_log.valid()
        && sensitivity_.cyan_forming_log.valid()
        && sensitivity_.auxiliary_trace_a_log.valid()
        && sensitivity_.auxiliary_trace_b_log.valid()

        && characteristic_.red_density.valid()
        && characteristic_.green_density.valid()
        && characteristic_.blue_density.valid()

        && dye_density_.visual_neutral_density.valid()
        && dye_density_.cyan_density.valid()
        && dye_density_.magenta_density.valid()
        && dye_density_.yellow_density.valid()

        && mtf_.red_response_percent.valid()
        && mtf_.green_response_percent.valid()
        && mtf_.blue_response_percent.valid()

        && granularity_.red_density.valid()
        && granularity_.green_density.valid()
        && granularity_.blue_density.valid()
        && granularity_.red_rms.valid()
        && granularity_.green_rms.valid()
        && granularity_.blue_rms.valid();
}

const std::string&
PrintFilmStock::name() const
{
    return name_;
}

const PrintFilmStock::SpectralSensitivity&
PrintFilmStock::sensitivity() const
{
    return sensitivity_;
}

const PrintFilmStock::CharacteristicCurves&
PrintFilmStock::characteristic() const
{
    return characteristic_;
}

const PrintFilmStock::SpectralDyeDensity&
PrintFilmStock::dye_density() const
{
    return dye_density_;
}

const PrintFilmStock::ModulationTransferFunction&
PrintFilmStock::mtf() const
{
    return mtf_;
}

const PrintFilmStock::DiffuseRMSGranularity&
PrintFilmStock::granularity() const
{
    return granularity_;
}

std::vector<CurveValidationResult>
PrintFilmStock::validate_interpolation(
    float tolerance) const
{
    std::vector<CurveValidationResult> results;

    results.push_back(
        validate_curve(
            "2383 yellow-forming spectral sensitivity",
            sensitivity_.yellow_forming_log,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 magenta-forming spectral sensitivity",
            sensitivity_.magenta_forming_log,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 cyan-forming spectral sensitivity",
            sensitivity_.cyan_forming_log,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 spectral sensitivity auxiliary A",
            sensitivity_.auxiliary_trace_a_log,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 spectral sensitivity auxiliary B",
            sensitivity_.auxiliary_trace_b_log,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 red sensitometric",
            characteristic_.red_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 green sensitometric",
            characteristic_.green_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 blue sensitometric",
            characteristic_.blue_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 visual-neutral spectral dye density",
            dye_density_.visual_neutral_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 cyan spectral dye density",
            dye_density_.cyan_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 magenta spectral dye density",
            dye_density_.magenta_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 yellow spectral dye density",
            dye_density_.yellow_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 red MTF",
            mtf_.red_response_percent,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 green MTF",
            mtf_.green_response_percent,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 blue MTF",
            mtf_.blue_response_percent,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 red granularity density",
            granularity_.red_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 green granularity density",
            granularity_.green_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 blue granularity density",
            granularity_.blue_density,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 red diffuse RMS granularity",
            granularity_.red_rms,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 green diffuse RMS granularity",
            granularity_.green_rms,
            tolerance));

    results.push_back(
        validate_curve(
            "2383 blue diffuse RMS granularity",
            granularity_.blue_rms,
            tolerance));

    return results;
}

bool
PrintFilmStock::load_sensitivity(
    const std::string& filename)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open 2383 spectral sensitivity CSV: "
            << filename
            << std::endl;
        return false;
    }

    if (!read_header(
            file,
            filename,
            "2383 spectral sensitivity")) {
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.empty()) {
            continue;
        }

        float wavelength = 0.0f;

        if (!parse_float(
                fields[0],
                wavelength)) {
            continue;
        }

        append_if_valid(
            fields,
            1,
            wavelength,
            sensitivity_.yellow_forming_log);

        append_if_valid(
            fields,
            2,
            wavelength,
            sensitivity_.magenta_forming_log);

        append_if_valid(
            fields,
            3,
            wavelength,
            sensitivity_.cyan_forming_log);

        append_if_valid(
            fields,
            4,
            wavelength,
            sensitivity_.auxiliary_trace_a_log);

        append_if_valid(
            fields,
            5,
            wavelength,
            sensitivity_.auxiliary_trace_b_log);
    }

    if (!sensitivity_.yellow_forming_log.valid()
        || !sensitivity_.magenta_forming_log.valid()
        || !sensitivity_.cyan_forming_log.valid()
        || !sensitivity_.auxiliary_trace_a_log.valid()
        || !sensitivity_.auxiliary_trace_b_log.valid()) {

        std::cerr
            << "error: 2383 spectral sensitivity CSV is incomplete"
            << std::endl;
        return false;
    }

    return true;
}

bool
PrintFilmStock::load_characteristic(
    const std::string& filename)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open 2383 sensitometric CSV: "
            << filename
            << std::endl;
        return false;
    }

    if (!read_header(
            file,
            filename,
            "2383 sensitometric")) {
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.empty()) {
            continue;
        }

        float log_exposure = 0.0f;

        if (!parse_float(
                fields[0],
                log_exposure)) {
            continue;
        }

        append_if_valid(
            fields,
            1,
            log_exposure,
            characteristic_.red_density);

        append_if_valid(
            fields,
            2,
            log_exposure,
            characteristic_.green_density);

        append_if_valid(
            fields,
            3,
            log_exposure,
            characteristic_.blue_density);
    }

    if (!characteristic_.red_density.valid()
        || !characteristic_.green_density.valid()
        || !characteristic_.blue_density.valid()) {

        std::cerr
            << "error: 2383 sensitometric CSV is incomplete"
            << std::endl;
        return false;
    }

    return true;
}

bool
PrintFilmStock::load_dye_density(
    const std::string& filename)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open 2383 spectral dye-density CSV: "
            << filename
            << std::endl;
        return false;
    }

    if (!read_header(
            file,
            filename,
            "2383 spectral dye-density")) {
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.empty()) {
            continue;
        }

        float wavelength = 0.0f;

        if (!parse_float(
                fields[0],
                wavelength)) {
            continue;
        }

        append_if_valid(
            fields,
            1,
            wavelength,
            dye_density_.visual_neutral_density);

        append_if_valid(
            fields,
            2,
            wavelength,
            dye_density_.cyan_density);

        append_if_valid(
            fields,
            3,
            wavelength,
            dye_density_.magenta_density);

        append_if_valid(
            fields,
            4,
            wavelength,
            dye_density_.yellow_density);
    }

    if (!dye_density_.visual_neutral_density.valid()
        || !dye_density_.cyan_density.valid()
        || !dye_density_.magenta_density.valid()
        || !dye_density_.yellow_density.valid()) {

        std::cerr
            << "error: 2383 spectral dye-density CSV is incomplete"
            << std::endl;
        return false;
    }

    return true;
}

bool
PrintFilmStock::load_mtf(
    const std::string& filename)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open 2383 MTF CSV: "
            << filename
            << std::endl;
        return false;
    }

    if (!read_header(
            file,
            filename,
            "2383 MTF")) {
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.empty()) {
            continue;
        }

        float frequency = 0.0f;

        if (!parse_float(
                fields[0],
                frequency)) {
            continue;
        }

        append_if_valid(
            fields,
            1,
            frequency,
            mtf_.red_response_percent);

        append_if_valid(
            fields,
            2,
            frequency,
            mtf_.green_response_percent);

        append_if_valid(
            fields,
            3,
            frequency,
            mtf_.blue_response_percent);
    }

    if (!mtf_.red_response_percent.valid()
        || !mtf_.green_response_percent.valid()
        || !mtf_.blue_response_percent.valid()) {

        std::cerr
            << "error: 2383 MTF CSV is incomplete"
            << std::endl;
        return false;
    }

    return true;
}

bool
PrintFilmStock::load_granularity(
    const std::string& filename)
{
    std::ifstream file(filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open 2383 granularity CSV: "
            << filename
            << std::endl;
        return false;
    }

    if (!read_header(
            file,
            filename,
            "2383 granularity")) {
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.empty()) {
            continue;
        }

        float log_exposure = 0.0f;

        if (!parse_float(
                fields[0],
                log_exposure)) {
            continue;
        }

        append_if_valid(
            fields,
            1,
            log_exposure,
            granularity_.red_density);

        append_if_valid(
            fields,
            2,
            log_exposure,
            granularity_.green_density);

        append_if_valid(
            fields,
            3,
            log_exposure,
            granularity_.blue_density);

        append_if_valid(
            fields,
            4,
            log_exposure,
            granularity_.red_rms);

        append_if_valid(
            fields,
            5,
            log_exposure,
            granularity_.green_rms);

        append_if_valid(
            fields,
            6,
            log_exposure,
            granularity_.blue_rms);
    }

    if (!granularity_.red_density.valid()
        || !granularity_.green_density.valid()
        || !granularity_.blue_density.valid()
        || !granularity_.red_rms.valid()
        || !granularity_.green_rms.valid()
        || !granularity_.blue_rms.valid()) {

        std::cerr
            << "error: 2383 granularity CSV is incomplete"
            << std::endl;
        return false;
    }

    return true;
}
