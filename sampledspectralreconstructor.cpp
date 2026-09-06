// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "sampledspectralreconstructor.h"

#include "colorimetry.h"
#include "spectralilluminant.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace {

bool
parse_double(
    const std::string& text,
    double& value)
{
    if (text.empty()) {
        return false;
    }

    const char* begin =
        text.c_str();

    char* end =
        nullptr;

    errno =
        0;

    const double parsed =
        std::strtod(
            begin,
            &end);

    if (begin == end
        || errno == ERANGE
        || !std::isfinite(parsed)) {

        return false;
    }

    while (*end != '\0') {
        if (*end != ' '
            && *end != '\t'
            && *end != '\r'
            && *end != '\n') {

            return false;
        }

        ++end;
    }

    value =
        parsed;

    return true;
}

std::vector<std::string>
split_csv(
    const std::string& line)
{
    std::vector<std::string> result;

    std::stringstream stream(
        line);

    std::string field;

    while (std::getline(
        stream,
        field,
        ',')) {

        result.push_back(
            field);
    }

    return result;
}

double
clamp01(
    double value)
{
    return
        std::max(
            0.0,
            std::min(
                1.0,
                value));
}

double
sample_observer(
    const std::vector<SampledSpectralReconstructor::ObserverSample>& samples,
    double wavelength,
    int channel)
{
    if (samples.empty()
        || wavelength < samples.front().wavelength
        || wavelength > samples.back().wavelength) {

        return 0.0;
    }

    const auto component =
        [channel](const SampledSpectralReconstructor::ObserverSample& sample) {
            if (channel == 0) {
                return sample.x;
            }

            if (channel == 1) {
                return sample.y;
            }

            return sample.z;
        };

    for (std::size_t i = 1;
         i < samples.size();
         ++i) {

        if (wavelength > samples[i].wavelength) {
            continue;
        }

        const double x0 =
            samples[i - 1].wavelength;

        const double x1 =
            samples[i].wavelength;

        const double y0 =
            component(
                samples[i - 1]);

        const double y1 =
            component(
                samples[i]);

        if (std::abs(x1 - x0) <= 1e-20) {
            return y0;
        }

        const double t =
            (wavelength - x0)
            / (x1 - x0);

        return
            y0
            + t
                * (y1 - y0);
    }

    return
        component(
            samples.back());
}

} // namespace

bool
SampledSpectralReconstructor::initialize(
    const std::string& observer_filename,
    const SpectralIlluminant& illuminant)
{
    return
        initialize(
            observer_filename,
            illuminant,
            Settings{});
}

bool
SampledSpectralReconstructor::initialize(
    const std::string& observer_filename,
    const SpectralIlluminant& illuminant,
    const Settings& settings)
{
    settings_ =
        settings;

    error_.clear();
    valid_ =
        false;

    observer_.clear();
    wavelengths_.clear();

    for (auto& row :
         forward_matrix_) {

        row.clear();
    }

    if (settings_.wavelength_step_nm <= 0.0f
        || settings_.wavelength_max_nm
            <= settings_.wavelength_min_nm
        || settings_.max_iterations <= 0
        || settings_.smoothness < 0.0
        || settings_.projected_gradient_tolerance < 0.0) {

        error_ =
            "invalid sampled spectral reconstruction settings";

        return false;
    }

    if (!illuminant.valid()) {
        error_ =
            "invalid sampled spectral reconstruction illuminant";

        return false;
    }

    if (!load_observer(
            observer_filename)) {

        return false;
    }

    for (float wavelength =
             settings_.wavelength_min_nm;
         wavelength <=
             settings_.wavelength_max_nm
             + 0.001f;
         wavelength +=
             settings_.wavelength_step_nm) {

        wavelengths_.push_back(
            wavelength);
    }

    if (!build_forward_matrix(
            illuminant)) {

        return false;
    }

    valid_ =
        true;

    return true;
}

bool
SampledSpectralReconstructor::valid() const
{
    return valid_;
}

SampledCurve
SampledSpectralReconstructor::reconstruct(
    const std::array<float, 3>& ap0_linear) const
{
    SampledCurve result;

    if (!valid_) {
        return result;
    }

    std::array<double, 3> target = {{
        std::max(
            0.0,
            static_cast<double>(
                ap0_linear[0])),
        std::max(
            0.0,
            static_cast<double>(
                ap0_linear[1])),
        std::max(
            0.0,
            static_cast<double>(
                ap0_linear[2]))
    }};

    const double maximum =
        std::max(
            target[0],
            std::max(
                target[1],
                target[2]));

    const double scale =
        std::max(
            1.0,
            maximum);

    for (double& value :
         target) {

        value /=
            scale;
    }

    const std::size_t count =
        wavelengths_.size();

    const double start_value =
        clamp01(
            (
                target[0]
                + target[1]
                + target[2])
            / 3.0);

    std::vector<double> spectrum(
        count,
        start_value);

    std::vector<double> gradient(
        count,
        0.0);

    std::vector<double> candidate(
        count,
        0.0);

    const auto evaluate =
        [&](const std::vector<double>& values,
            std::vector<double>* output_gradient) {

            const std::array<double, 3> forward =
                forward_ap0(
                    values);

            const std::array<double, 3> error = {{
                forward[0] - target[0],
                forward[1] - target[1],
                forward[2] - target[2]
            }};

            double objective =
                error[0] * error[0]
                + error[1] * error[1]
                + error[2] * error[2];

            if (output_gradient) {
                std::fill(
                    output_gradient->begin(),
                    output_gradient->end(),
                    0.0);

                for (std::size_t i = 0;
                     i < count;
                     ++i) {

                    (*output_gradient)[i] +=
                        2.0
                        * (
                            error[0]
                                * forward_matrix_[0][i]
                            + error[1]
                                * forward_matrix_[1][i]
                            + error[2]
                                * forward_matrix_[2][i]);
                }
            }

            for (std::size_t i = 1;
                 i + 1 < count;
                 ++i) {

                const double second_difference =
                    values[i - 1]
                    - 2.0 * values[i]
                    + values[i + 1];

                objective +=
                    settings_.smoothness
                    * second_difference
                    * second_difference;

                if (output_gradient) {
                    const double g =
                        2.0
                        * settings_.smoothness
                        * second_difference;

                    (*output_gradient)[i - 1] +=
                        g;

                    (*output_gradient)[i] -=
                        2.0 * g;

                    (*output_gradient)[i + 1] +=
                        g;
                }
            }

            return objective;
        };

    double objective =
        evaluate(
            spectrum,
            &gradient);

    double step =
        1.0;

    for (int iteration = 0;
         iteration < settings_.max_iterations;
         ++iteration) {

        double projected_norm_squared =
            0.0;

        for (std::size_t i = 0;
             i < count;
             ++i) {

            const double projected =
                clamp01(
                    spectrum[i]
                    - gradient[i]);

            const double difference =
                spectrum[i]
                - projected;

            projected_norm_squared +=
                difference
                * difference;
        }

        if (projected_norm_squared
            <= settings_.projected_gradient_tolerance
                * settings_.projected_gradient_tolerance) {

            break;
        }

        bool accepted =
            false;

        double trial_step =
            step;

        for (int line_search = 0;
             line_search < 24;
             ++line_search) {

            for (std::size_t i = 0;
                 i < count;
                 ++i) {

                candidate[i] =
                    clamp01(
                        spectrum[i]
                        - trial_step
                            * gradient[i]);
            }

            const double candidate_objective =
                evaluate(
                    candidate,
                    nullptr);

            if (candidate_objective
                <= objective) {

                spectrum.swap(
                    candidate);

                objective =
                    evaluate(
                        spectrum,
                        &gradient);

                accepted =
                    true;

                step =
                    std::min(
                        100.0,
                        trial_step
                            * 1.2);

                break;
            }

            trial_step *=
                0.5;
        }

        if (!accepted) {
            break;
        }
    }

    result.x.reserve(
        count);

    result.y.reserve(
        count);

    for (std::size_t i = 0;
         i < count;
         ++i) {

        result.x.push_back(
            static_cast<float>(
                wavelengths_[i]));

        result.y.push_back(
            static_cast<float>(
                scale
                * spectrum[i]));
    }

    return
        result.valid()
            ? result
            : SampledCurve();
}

const SampledSpectralReconstructor::Settings&
SampledSpectralReconstructor::settings() const
{
    return settings_;
}

const std::string&
SampledSpectralReconstructor::error() const
{
    return error_;
}

bool
SampledSpectralReconstructor::load_observer(
    const std::string& filename)
{
    std::ifstream file(
        filename.c_str());

    if (!file) {
        error_ =
            "could not open CIE observer CSV: "
            + filename;

        return false;
    }

    std::string line;

    while (std::getline(
        file,
        line)) {

        const auto field =
            split_csv(
                line);

        if (field.size() < 4) {
            continue;
        }

        ObserverSample sample;

        if (!parse_double(
                field[0],
                sample.wavelength)
            || !parse_double(
                field[1],
                sample.x)
            || !parse_double(
                field[2],
                sample.y)
            || !parse_double(
                field[3],
                sample.z)) {

            continue;
        }

        observer_.push_back(
            sample);
    }

    std::sort(
        observer_.begin(),
        observer_.end(),
        [](const ObserverSample& a,
           const ObserverSample& b) {
            return
                a.wavelength
                < b.wavelength;
        });

    if (observer_.empty()) {
        error_ =
            "CIE observer CSV did not contain usable samples";

        return false;
    }

    return true;
}

bool
SampledSpectralReconstructor::build_forward_matrix(
    const SpectralIlluminant& illuminant)
{
    const std::size_t count =
        wavelengths_.size();

    for (auto& row :
         forward_matrix_) {

        row.assign(
            count,
            0.0);
    }

    double normalization =
        0.0;

    for (double wavelength :
         wavelengths_) {

        const double power =
            illuminant.curve().sample(
                static_cast<float>(
                    wavelength),
                0.0f);

        const double y_bar =
            sample_observer(
                observer_,
                wavelength,
                1);

        normalization +=
            power
            * y_bar
            * settings_.wavelength_step_nm;
    }

    if (!std::isfinite(normalization)
        || normalization <= 1e-20) {

        error_ =
            "could not normalize sampled D60/CIE forward model";

        return false;
    }

    const double k =
        1.0
        / normalization;

    for (std::size_t i = 0;
         i < count;
         ++i) {

        const double wavelength =
            wavelengths_[i];

        const double power =
            illuminant.curve().sample(
                static_cast<float>(
                    wavelength),
                0.0f);

        const double scale =
            k
            * power
            * settings_.wavelength_step_nm;

        const Colorimetry::XYZ xyz = {
            scale
                * sample_observer(
                    observer_,
                    wavelength,
                    0),
            scale
                * sample_observer(
                    observer_,
                    wavelength,
                    1),
            scale
                * sample_observer(
                    observer_,
                    wavelength,
                    2)
        };

        const auto ap0 =
            Colorimetry::xyz_d60_to_ap0(
                xyz);

        forward_matrix_[0][i] =
            ap0[0];

        forward_matrix_[1][i] =
            ap0[1];

        forward_matrix_[2][i] =
            ap0[2];
    }

    return true;
}

std::array<double, 3>
SampledSpectralReconstructor::forward_ap0(
    const std::vector<double>& spectrum) const
{
    std::array<double, 3> result = {{
        0.0,
        0.0,
        0.0
    }};

    for (std::size_t i = 0;
         i < spectrum.size();
         ++i) {

        result[0] +=
            forward_matrix_[0][i]
            * spectrum[i];

        result[1] +=
            forward_matrix_[1][i]
            * spectrum[i];

        result[2] +=
            forward_matrix_[2][i]
            * spectrum[i];
    }

    return result;
}
