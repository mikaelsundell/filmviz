// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "colorimetry.h"
#include "filmpipeline.h"
#include "spectralilluminant.h"
#include "test_common.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ObserverSample
{
    double wavelength = 0.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct SpectrumResult
{
    std::vector<double> values;
    Vec3 forward_ap0;
    double nrmse = 0.0;
    double objective = 0.0;
};

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

bool
load_observer(
    const std::filesystem::path& filename,
    std::vector<ObserverSample>& result)
{
    result.clear();

    std::ifstream file(
        filename);

    if (!file) {
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

        result.push_back(
            sample);
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const ObserverSample& a,
           const ObserverSample& b) {
            return
                a.wavelength
                < b.wavelength;
        });

    return
        !result.empty();
}

template <typename Sample, typename Member>
double
sample_linear(
    const std::vector<Sample>& samples,
    double wavelength,
    Member member,
    double missing = 0.0)
{
    if (samples.empty()
        || wavelength < samples.front().wavelength
        || wavelength > samples.back().wavelength) {

        return missing;
    }

    for (std::size_t i = 1;
         i < samples.size();
         ++i) {

        if (wavelength
            <= samples[i].wavelength) {

            const double x0 =
                samples[i - 1].wavelength;

            const double x1 =
                samples[i].wavelength;

            const double y0 =
                samples[i - 1].*member;

            const double y1 =
                samples[i].*member;

            if (std::abs(
                    x1 - x0)
                <= 1e-20) {

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
    }

    return
        samples.back().*member;
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
sigmoid(
    double value)
{
    if (value >= 0.0) {
        const double e =
            std::exp(
                -value);

        return
            1.0
            / (1.0 + e);
    }

    const double e =
        std::exp(
            value);

    return
        e
        / (1.0 + e);
}

double
logit(
    double value)
{
    constexpr double epsilon =
        1e-6;

    value =
        std::max(
            epsilon,
            std::min(
                1.0 - epsilon,
                value));

    return
        std::log(
            value
            / (1.0 - value));
}

double
square(
    double value)
{
    return
        value
        * value;
}

double
nrmse(
    const Vec3& actual,
    const Vec3& reference)
{
    const double numerator =
        square(actual.x - reference.x)
        + square(actual.y - reference.y)
        + square(actual.z - reference.z);

    const double denominator =
        square(reference.x)
        + square(reference.y)
        + square(reference.z);

    return
        std::sqrt(
            numerator
            / std::max(
                denominator,
                1e-20));
}

class ConstrainedSpectralSolver
{
public:
    ConstrainedSpectralSolver(
        const std::filesystem::path& observer_filename,
        double wavelength_min_nm = 360.0,
        double wavelength_max_nm = 830.0,
        double wavelength_step_nm = 5.0)
        : wavelength_min_nm_(
              wavelength_min_nm)
        , wavelength_max_nm_(
              wavelength_max_nm)
        , wavelength_step_nm_(
              wavelength_step_nm)
    {
        valid_ =
            load_observer(
                observer_filename,
                observer_)
            && d60_.valid();

        if (!valid_) {
            return;
        }

        for (double wavelength =
                 wavelength_min_nm_;
             wavelength <=
                 wavelength_max_nm_
                 + 0.001;
             wavelength +=
                 wavelength_step_nm_) {

            wavelengths_.push_back(
                wavelength);
        }

        build_forward_matrix();
    }

    bool
    valid() const
    {
        return valid_;
    }

    SpectrumResult
    solve(
        const Vec3& target_ap0,
        double smoothness_weight,
        int iterations = 20000) const
    {
        SpectrumResult best;

        best.objective =
            std::numeric_limits<double>::infinity();

        if (!valid_) {
            return best;
        }

        const std::size_t count =
            wavelengths_.size();

        // This problem is a convex quadratic with box constraints:
        //
        //   min ||A s - target||^2 + lambda ||D2 s||^2
        //   subject to 0 <= s_i <= 1
        //
        // Solve it directly in spectral-sample space with projected
        // gradient descent + backtracking. Do not hide the variables behind
        // a sigmoid; the sigmoid made gradients tiny and left saturated-color
        // cases stuck near the initial gray spectrum.
        const double start_value =
            clamp01(
                (
                    target_ap0.x
                    + target_ap0.y
                    + target_ap0.z)
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

        auto evaluate =
            [&](const std::vector<double>& values,
                std::vector<double>* output_gradient,
                Vec3* output_forward) {

                const Vec3 forward =
                    forward_ap0(
                        values);

                if (output_forward) {
                    *output_forward =
                        forward;
                }

                const Vec3 error = {
                    forward.x - target_ap0.x,
                    forward.y - target_ap0.y,
                    forward.z - target_ap0.z
                };

                double objective =
                    square(error.x)
                    + square(error.y)
                    + square(error.z);

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
                                error.x
                                    * forward_matrix_[0][i]
                                + error.y
                                    * forward_matrix_[1][i]
                                + error.z
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
                        smoothness_weight
                        * second_difference
                        * second_difference;

                    if (output_gradient) {
                        const double g =
                            2.0
                            * smoothness_weight
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

        Vec3 current_forward;

        double current_objective =
            evaluate(
                spectrum,
                &gradient,
                &current_forward);

        best.values =
            spectrum;

        best.forward_ap0 =
            current_forward;

        best.objective =
            current_objective;

        best.nrmse =
            nrmse(
                current_forward,
                target_ap0);

        double step =
            1.0;

        for (int iteration = 0;
             iteration < iterations;
             ++iteration) {

            // Projected-gradient norm is the useful stopping test at bounds.
            double projected_gradient_norm2 =
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

                projected_gradient_norm2 +=
                    difference
                    * difference;
            }

            if (projected_gradient_norm2 < 1e-20) {
                break;
            }

            bool accepted =
                false;

            double trial_step =
                step;

            Vec3 candidate_forward;
            double candidate_objective =
                current_objective;

            for (int line_search = 0;
                 line_search < 30;
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

                candidate_objective =
                    evaluate(
                        candidate,
                        nullptr,
                        &candidate_forward);

                if (candidate_objective
                    <= current_objective) {

                    accepted =
                        true;

                    break;
                }

                trial_step *=
                    0.5;
            }

            if (!accepted) {
                break;
            }

            spectrum.swap(
                candidate);

            current_objective =
                evaluate(
                    spectrum,
                    &gradient,
                    &current_forward);

            if (current_objective
                < best.objective) {

                best.values =
                    spectrum;

                best.forward_ap0 =
                    current_forward;

                best.objective =
                    current_objective;

                best.nrmse =
                    nrmse(
                        current_forward,
                        target_ap0);
            }

            // Reuse a successful step and grow cautiously.
            step =
                std::min(
                    100.0,
                    trial_step * 1.2);
        }

        return best;
    }

    Vec3
    forward_ap0(
        const std::vector<double>& spectrum) const
    {
        Vec3 result;

        for (std::size_t i = 0;
             i < spectrum.size();
             ++i) {

            result.x +=
                forward_matrix_[0][i]
                * spectrum[i];

            result.y +=
                forward_matrix_[1][i]
                * spectrum[i];

            result.z +=
                forward_matrix_[2][i]
                * spectrum[i];
        }

        return result;
    }

    const std::vector<double>&
    wavelengths() const
    {
        return wavelengths_;
    }

private:
    void
    build_forward_matrix()
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

            const double illuminant =
                static_cast<double>(
                    d60_.curve().sample(
                        static_cast<float>(wavelength),
                        0.0f));

            const double y_bar =
                sample_linear(
                    observer_,
                    wavelength,
                    &ObserverSample::y);

            normalization +=
                illuminant
                * y_bar
                * wavelength_step_nm_;
        }

        if (normalization <= 1e-20) {
            valid_ =
                false;

            return;
        }

        const double k =
            1.0
            / normalization;

        for (std::size_t i = 0;
             i < count;
             ++i) {

            const double wavelength =
                wavelengths_[i];

            const double illuminant =
                static_cast<double>(
                    d60_.curve().sample(
                        static_cast<float>(wavelength),
                        0.0f));

            const double x_bar =
                sample_linear(
                    observer_,
                    wavelength,
                    &ObserverSample::x);

            const double y_bar =
                sample_linear(
                    observer_,
                    wavelength,
                    &ObserverSample::y);

            const double z_bar =
                sample_linear(
                    observer_,
                    wavelength,
                    &ObserverSample::z);

            const double scale =
                k
                * illuminant
                * wavelength_step_nm_;

            const Colorimetry::XYZ xyz = {
                scale * x_bar,
                scale * y_bar,
                scale * z_bar
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

        // Sanity check: a flat unit reflectance should integrate to
        // approximately AP0 neutral (1,1,1) under D60.
        std::vector<double> white(
            count,
            1.0);

        const Vec3 unit_white =
            forward_ap0(
                white);

        valid_ =
            std::isfinite(unit_white.x)
            && std::isfinite(unit_white.y)
            && std::isfinite(unit_white.z);
    }

    double wavelength_min_nm_ =
        360.0;

    double wavelength_max_nm_ =
        830.0;

    double wavelength_step_nm_ =
        5.0;

    bool valid_ =
        false;

    std::vector<ObserverSample> observer_;
    SpectralIlluminant d60_{
        SpectralIlluminant::Standard::D60
    };
    std::vector<double> wavelengths_;

    std::array<std::vector<double>, 3>
        forward_matrix_;
};

double
sample_at(
    const std::vector<double>& wavelengths,
    const std::vector<double>& values,
    double wavelength)
{
    if (wavelengths.empty()
        || values.empty()) {

        return 0.0;
    }

    std::size_t index =
        0;

    double distance =
        std::abs(
            wavelengths[0]
            - wavelength);

    for (std::size_t i = 1;
         i < wavelengths.size();
         ++i) {

        const double current =
            std::abs(
                wavelengths[i]
                - wavelength);

        if (current < distance) {
            distance =
                current;

            index =
                i;
        }
    }

    return
        values[index];
}

double
band_sum(
    const std::vector<double>& wavelengths,
    const std::vector<double>& values,
    double low,
    double high)
{
    double result =
        0.0;

    for (std::size_t i = 0;
         i < wavelengths.size();
         ++i) {

        if (wavelengths[i] >= low
            && wavelengths[i] < high) {

            result +=
                values[i];
        }
    }

    return result;
}


struct SensitivityRow
{
    double wavelength = 0.0;
    double yellow = -9999.0;
    double magenta = -9999.0;
    double cyan = -9999.0;
};

bool
load_negative_sensitivity(
    const std::filesystem::path& filename,
    std::vector<SensitivityRow>& rows)
{
    rows.clear();

    std::ifstream file(
        filename);

    if (!file) {
        return false;
    }

    std::string line;

    // Header.
    if (!std::getline(
            file,
            line)) {

        return false;
    }

    while (std::getline(
        file,
        line)) {

        if (line.empty()) {
            continue;
        }

        const auto field =
            split_csv(
                line);

        if (field.size() < 4) {
            continue;
        }

        SensitivityRow row;

        if (!parse_double(
                field[0],
                row.wavelength)) {

            continue;
        }

        double value = 0.0;

        if (parse_double(
                field[1],
                value)) {

            row.yellow =
                value;
        }

        if (parse_double(
                field[2],
                value)) {

            row.magenta =
                value;
        }

        if (parse_double(
                field[3],
                value)) {

            row.cyan =
                value;
        }

        rows.push_back(
            row);
    }

    std::sort(
        rows.begin(),
        rows.end(),
        [](const SensitivityRow& a,
           const SensitivityRow& b) {
            return
                a.wavelength
                < b.wavelength;
        });

    return
        !rows.empty();
}

double
sample_log_sensitivity(
    const std::vector<SensitivityRow>& rows,
    double wavelength,
    int channel)
{
    if (rows.size() < 2
        || wavelength < rows.front().wavelength
        || wavelength > rows.back().wavelength) {

        return 0.0;
    }

    const auto value =
        [channel](const SensitivityRow& row) {
            if (channel == 0) {
                return row.yellow;
            }

            if (channel == 1) {
                return row.magenta;
            }

            return row.cyan;
        };

    for (std::size_t i = 1;
         i < rows.size();
         ++i) {

        if (wavelength > rows[i].wavelength) {
            continue;
        }

        const double y0 =
            value(
                rows[i - 1]);

        const double y1 =
            value(
                rows[i]);

        if (y0 < -9000.0
            || y1 < -9000.0) {

            return 0.0;
        }

        const double x0 =
            rows[i - 1].wavelength;

        const double x1 =
            rows[i].wavelength;

        const double t =
            std::abs(x1 - x0) > 1e-20
                ? (wavelength - x0)
                    / (x1 - x0)
                : 0.0;

        const double log_s =
            y0
            + t
                * (y1 - y0);

        return
            std::pow(
                10.0,
                log_s);
    }

    return 0.0;
}

FilmExposure
negative_exposure_from_sampled_spectrum(
    const std::vector<double>& wavelengths,
    const std::vector<double>& spectrum,
    const SpectralIlluminant& d60,
    const std::vector<SensitivityRow>& sensitivity,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm)
{
    double yellow_h = 0.0;
    double magenta_h = 0.0;
    double cyan_h = 0.0;

    for (std::size_t i = 0;
         i < wavelengths.size();
         ++i) {

        const double wavelength =
            wavelengths[i];

        if (wavelength
                < static_cast<double>(
                    wavelength_min_nm)
            || wavelength
                > static_cast<double>(
                    wavelength_max_nm)
                    + 0.001) {

            continue;
        }

        const double illuminant =
            static_cast<double>(
                d60.curve().sample(
                    static_cast<float>(wavelength),
                    0.0f));

        const double power =
            spectrum[i]
            * illuminant;

        yellow_h +=
            power
            * sample_log_sensitivity(
                sensitivity,
                wavelength,
                0)
            * wavelength_step_nm;

        magenta_h +=
            power
            * sample_log_sensitivity(
                sensitivity,
                wavelength,
                1)
            * wavelength_step_nm;

        cyan_h +=
            power
            * sample_log_sensitivity(
                sensitivity,
                wavelength,
                2)
            * wavelength_step_nm;
    }

    FilmExposure result;

    // FilmProcessor record mapping:
    //   red   <- cyan-forming layer
    //   green <- magenta-forming layer
    //   blue  <- yellow-forming layer
    result.red =
        static_cast<float>(
            cyan_h);

    result.green =
        static_cast<float>(
            magenta_h);

    result.blue =
        static_cast<float>(
            yellow_h);

    return result;
}

FilmExposure
neutral_match_exposure(
    const FilmExposure& exposure,
    const FilmExposure& sampled_neutral,
    const FilmExposure& production_neutral)
{
    FilmExposure result =
        exposure;

    result.red *=
        production_neutral.red
        / std::max(
            sampled_neutral.red,
            1e-20f);

    result.green *=
        production_neutral.green
        / std::max(
            sampled_neutral.green,
            1e-20f);

    result.blue *=
        production_neutral.blue
        / std::max(
            sampled_neutral.blue,
            1e-20f);

    return result;
}

double
final_lab_hue(
    const std::array<float, 3>& ap0)
{
    const std::array<double, 3> value = {{
        static_cast<double>(ap0[0]),
        static_cast<double>(ap0[1]),
        static_cast<double>(ap0[2])
    }};

    const Colorimetry::XYZ xyz =
        Colorimetry::ap0_to_xyz_d60(
            value);

    const Colorimetry::Lab lab =
        Colorimetry::xyz_to_lab(
            xyz,
            Colorimetry::aces_d60_white());

    return
        Colorimetry::hue_degrees(
            lab.a,
            lab.b);
}

double
blue_minus_green(
    const std::array<float, 3>& ap0)
{
    return
        static_cast<double>(ap0[2])
        - static_cast<double>(ap0[1]);
}

void
write_result(
    std::ofstream& csv,
    const char* color,
    const char* model,
    double smoothness,
    double input_nrmse,
    const FilmExposure& exposure,
    const FilmPipeline::Result& result)
{
    csv
        << color
        << ','
        << model
        << ','
        << smoothness
        << ','
        << input_nrmse
        << ','
        << exposure.red
        << ','
        << exposure.green
        << ','
        << exposure.blue
        << ','
        << result.negative_status_m_density.red
        << ','
        << result.negative_status_m_density.green
        << ','
        << result.negative_status_m_density.blue
        << ','
        << result.calibrated_negative_density.red
        << ','
        << result.calibrated_negative_density.green
        << ','
        << result.calibrated_negative_density.blue
        << ','
        << result.print_density.red
        << ','
        << result.print_density.green
        << ','
        << result.print_density.blue
        << ','
        << result.ap0[0]
        << ','
        << result.ap0[1]
        << ','
        << result.ap0[2]
        << ','
        << blue_minus_green(
            result.ap0)
        << ','
        << final_lab_hue(
            result.ap0)
        << '\n';
}


} // namespace



int
main()
{
    namespace fs =
        std::filesystem;

    const fs::path resources =
        fs::path(
            FILMVIZ_TEST_RESOURCE_DIR);

    const fs::path observer =
        resources
        / "colorimetry/observers/CIE_xyz_1931_2deg.csv";

    const fs::path sensitivity_file =
        resources
        / "profiles/kodak_50d/kodak_50d_spectral_sensitivity_curves.csv";

    ConstrainedSpectralSolver solver(
        observer);

    SpectralIlluminant d60(
        SpectralIlluminant::Standard::D60);

    std::vector<SensitivityRow> sensitivity;

    FilmPipeline::Settings settings;

    settings.resources_directory =
        FILMVIZ_TEST_RESOURCE_DIR;

    settings.negative_profile =
        "kodak-50d";

    settings.print_profile =
        "kodak-2383";

    settings.exposure_stops =
        0.0f;

    settings.push_pull_stops =
        0.0f;

    settings.negative_bleach_bypass =
        0.0f;

    settings.print_bleach_bypass =
        0.0f;

    settings.printer_light_red =
        25.0f;

    settings.printer_light_green =
        25.0f;

    settings.printer_light_blue =
        25.0f;

    FilmPipeline pipeline;

    bool passed =
        true;

    passed &=
        test::check(
            solver.valid(),
            "constrained spectral solver initializes");

    passed &=
        test::check(
            d60.valid(),
            "D60 illuminant initializes");

    passed &=
        test::check(
            load_negative_sensitivity(
                sensitivity_file,
                sensitivity),
            "Kodak 50D spectral sensitivity loads");

    passed &=
        test::check(
            pipeline.initialize(
                settings),
            std::string("production 50D + 2383 pipeline initializes: ")
                + pipeline.error());

    if (!passed) {
        return
            test::finish(
                false,
                "full pipeline reconstruction compare");
    }

    struct Color
    {
        const char* name;
        Vec3 ap0;
    };

    const std::array<Color, 5> colors = {{
        {
            "neutral",
            {0.18, 0.18, 0.18}
        },
        {
            "orange_red",
            {0.18, 0.08, 0.01}
        },
        {
            "warm_red",
            {0.18, 0.045, 0.018}
        },
        {
            "red",
            {0.18, 0.018, 0.009}
        },
        {
            "deep_red",
            {0.18, 0.0036, 0.0018}
        }
    }};

    const std::array<double, 3> weights = {{
        0.0,
        1e-5,
        1e-4
    }};

    const std::array<float, 3> neutral_ap0 = {{
        settings.middle_gray,
        settings.middle_gray,
        settings.middle_gray
    }};

    FilmExposure production_neutral;

    passed &=
        test::check(
            pipeline.negative_exposure(
                neutral_ap0,
                production_neutral),
            "production neutral negative exposure evaluates");

    const SpectrumResult sampled_neutral_result =
        solver.solve(
            {
                settings.middle_gray,
                settings.middle_gray,
                settings.middle_gray
            },
            0.0);

    const FilmExposure sampled_neutral =
        negative_exposure_from_sampled_spectrum(
            solver.wavelengths(),
            sampled_neutral_result.values,
            d60,
            sensitivity,
            settings.wavelength_min_nm,
            settings.wavelength_max_nm,
            settings.wavelength_step_nm);

    passed &=
        test::check(
            std::isfinite(sampled_neutral.red)
            && std::isfinite(sampled_neutral.green)
            && std::isfinite(sampled_neutral.blue)
            && sampled_neutral.red > 0.0f
            && sampled_neutral.green > 0.0f
            && sampled_neutral.blue > 0.0f,
            "sampled neutral negative exposure evaluates");

    const fs::path output_directory =
        fs::path("tests")
        / "output"
        / "test_full_pipeline_reconstruction_compare";

    std::error_code error;

    fs::create_directories(
        output_directory,
        error);

    passed &=
        test::check(
            !error,
            "comparison output directory exists");

    std::ofstream csv(
        output_directory
        / "full_pipeline_compare.csv");

    passed &=
        test::check(
            static_cast<bool>(csv),
            "full-pipeline comparison CSV opens");

    if (!passed) {
        return
            test::finish(
                false,
                "full pipeline reconstruction compare");
    }

    csv
        << "color,model,smoothness,input_nrmse,"
        << "neg_h_r,neg_h_g,neg_h_b,"
        << "status_r,status_g,status_b,"
        << "cal_r,cal_g,cal_b,"
        << "print_d_r,print_d_g,print_d_b,"
        << "final_ap0_r,final_ap0_g,final_ap0_b,"
        << "final_b_minus_g,final_lab_hue_deg\n";

    csv
        << std::setprecision(10);

    std::cout
        << std::setprecision(10)
        << "\nNeutral exposure anchor\n"
        << "  production H=("
        << production_neutral.red
        << ", "
        << production_neutral.green
        << ", "
        << production_neutral.blue
        << ")\n"
        << "  sampled    H=("
        << sampled_neutral.red
        << ", "
        << sampled_neutral.green
        << ", "
        << sampled_neutral.blue
        << ")\n";

    for (const Color& color :
         colors) {

        const std::array<float, 3> ap0 = {{
            static_cast<float>(
                color.ap0.x),
            static_cast<float>(
                color.ap0.y),
            static_cast<float>(
                color.ap0.z)
        }};

        FilmExposure production_exposure;

        passed &=
            test::check(
                pipeline.negative_exposure(
                    ap0,
                    production_exposure),
                std::string("production exposure evaluates for ")
                    + color.name);

        const FilmPipeline::Result production =
            pipeline.process(
                ap0);

        passed &=
            test::check(
                production.valid,
                std::string("production full pipeline valid for ")
                    + color.name);

        if (!production.valid) {
            continue;
        }

        std::cout
            << "\n"
            << color.name
            << "\n"
            << "  production"
            << " H=("
            << production_exposure.red
            << ", "
            << production_exposure.green
            << ", "
            << production_exposure.blue
            << ") final AP0=("
            << production.ap0[0]
            << ", "
            << production.ap0[1]
            << ", "
            << production.ap0[2]
            << ") B-G="
            << blue_minus_green(
                production.ap0)
            << " hue="
            << final_lab_hue(
                production.ap0)
            << " printD=("
            << production.print_density.red
            << ", "
            << production.print_density.green
            << ", "
            << production.print_density.blue
            << ")\n";

        write_result(
            csv,
            color.name,
            "production_rgb2spec",
            0.0,
            std::numeric_limits<double>::quiet_NaN(),
            production_exposure,
            production);

        for (double weight :
             weights) {

            const SpectrumResult reconstructed =
                solver.solve(
                    color.ap0,
                    weight);

            FilmExposure sampled_exposure =
                negative_exposure_from_sampled_spectrum(
                    solver.wavelengths(),
                    reconstructed.values,
                    d60,
                    sensitivity,
                    settings.wavelength_min_nm,
                    settings.wavelength_max_nm,
                    settings.wavelength_step_nm);

            // Anchor the experimental reconstructor to exactly the same neutral
            // film exposure as the production pipeline. This removes the tiny
            // observer/integration normalization difference and isolates only
            // chromatic/metamer differences.
            sampled_exposure =
                neutral_match_exposure(
                    sampled_exposure,
                    sampled_neutral,
                    production_neutral);

            const FilmPipeline::Result sampled =
                pipeline.process_negative_exposure(
                    sampled_exposure);

            passed &=
                test::check(
                    sampled.valid,
                    std::string("sampled full pipeline valid for ")
                        + color.name
                        + " / lambda "
                        + std::to_string(weight));

            if (!sampled.valid) {
                continue;
            }

            const double hue_delta =
                Colorimetry::signed_hue_delta_degrees(
                    final_lab_hue(
                        production.ap0),
                    final_lab_hue(
                        sampled.ap0));

            std::cout
                << "  sampled lambda="
                << weight
                << " input NRMSE="
                << reconstructed.nrmse
                << " H=("
                << sampled_exposure.red
                << ", "
                << sampled_exposure.green
                << ", "
                << sampled_exposure.blue
                << ") final AP0=("
                << sampled.ap0[0]
                << ", "
                << sampled.ap0[1]
                << ", "
                << sampled.ap0[2]
                << ") B-G="
                << blue_minus_green(
                    sampled.ap0)
                << " hue="
                << final_lab_hue(
                    sampled.ap0)
                << " hueDeltaVsProd="
                << hue_delta
                << " printD=("
                << sampled.print_density.red
                << ", "
                << sampled.print_density.green
                << ", "
                << sampled.print_density.blue
                << ")\n";

            write_result(
                csv,
                color.name,
                "sampled",
                weight,
                reconstructed.nrmse,
                sampled_exposure,
                sampled);
        }
    }

    std::cout
        << "\nCSV: "
        << (
            output_directory
            / "full_pipeline_compare.csv")
        << "\n";

    return
        test::finish(
            passed,
            "full pipeline reconstruction compare");
}
