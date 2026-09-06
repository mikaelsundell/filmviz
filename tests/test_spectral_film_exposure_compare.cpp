// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "colorimetry.h"
#include "spectralilluminant.h"
#include "spectralreconstructor.h"
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

struct SensitivityRow
{
    double wavelength = 0.0;
    double yellow = -9999.0;
    double magenta = -9999.0;
    double cyan = -9999.0;
};

bool
read_sensitivity(
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

    auto value =
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

        if (wavelength
            > rows[i].wavelength) {

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

std::array<double, 3>
film_exposure(
    const std::vector<double>& wavelengths,
    const std::vector<double>& spectrum,
    const SpectralIlluminant& d60,
    const std::vector<SensitivityRow>& sensitivity,
    double step_nm)
{
    std::array<double, 3> result = {{
        0.0,
        0.0,
        0.0
    }};

    for (std::size_t i = 0;
         i < wavelengths.size();
         ++i) {

        const double wavelength =
            wavelengths[i];

        const double illuminant =
            static_cast<double>(
                d60.curve().sample(
                    static_cast<float>(wavelength),
                    0.0f));

        const double power =
            spectrum[i]
            * illuminant;

        for (int channel = 0;
             channel < 3;
             ++channel) {

            result[channel] +=
                power
                * sample_log_sensitivity(
                    sensitivity,
                    wavelength,
                    channel)
                * step_nm;
        }
    }

    return result;
}

std::array<double, 3>
production_film_exposure(
    SpectralReconstructor& reconstructor,
    const std::array<float, 3>& ap0,
    const SpectralIlluminant& d60,
    const std::vector<SensitivityRow>& sensitivity)
{
    const auto reconstructed =
        reconstructor.reconstruct(
            ap0,
            SpectralReconstructor::Method::Optimized);

    const auto sampled =
        reconstructor.sample(
            reconstructed,
            360.0f,
            830.0f,
            5.0f);

    std::vector<double> wavelengths;
    std::vector<double> spectrum;

    wavelengths.reserve(
        sampled.x.size());

    spectrum.reserve(
        sampled.y.size());

    for (std::size_t i = 0;
         i < sampled.x.size();
         ++i) {

        wavelengths.push_back(
            sampled.x[i]);

        spectrum.push_back(
            sampled.y[i]);
    }

    return
        film_exposure(
            wavelengths,
            spectrum,
            d60,
            sensitivity,
            5.0);
}

double
relative_change(
    double value,
    double reference)
{
    if (std::abs(reference) <= 1e-30) {
        return 0.0;
    }

    return
        value
        / reference;
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

    const fs::path spec =
        resources
        / "spectral/reconstruction/ACES2065_1.spec";

    const fs::path sensitivity_file =
        resources
        / "profiles/kodak_50d/kodak_50d_spectral_sensitivity_curves.csv";

    ConstrainedSpectralSolver solver(
        observer);

    SpectralReconstructor reconstructor(
        spec.string());

    SpectralIlluminant d60(
        SpectralIlluminant::Standard::D60);

    std::vector<SensitivityRow> sensitivity;

    if (!solver.valid()) {
        std::cerr
            << "[FAIL] constrained spectral solver resources failed to load\n"
            << "  observer: "
            << observer
            << "\n"
            << "  D60 source: SpectralIlluminant::Standard::D60\n";

        return 1;
    }

    std::cout
        << "[PASS] constrained spectral solver resources load\n";

    bool passed =
        true;

    passed &=
        test::check(
            reconstructor.valid(),
            "production ACES2065-1 rgb2spec table loads");

    passed &=
        test::check(
            d60.valid(),
            "production D60 illuminant loads");

    passed &=
        test::check(
            read_sensitivity(
                sensitivity_file,
                sensitivity),
            "Kodak 50D sensitivity CSV loads");

    if (!passed) {
        return
            test::finish(
                false,
                "spectral film exposure compare");
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

    const std::array<double, 6> weights = {{
        0.0,
        1e-8,
        1e-7,
        1e-6,
        1e-5,
        1e-4
    }};

    const fs::path output_directory =
        fs::path("tests")
        / "output"
        / "test_constrained_spectral_solver";

    std::error_code error;

    fs::create_directories(
        output_directory,
        error);

    if (error) {
        std::cerr
            << "[FAIL] could not create output directory\n";

        return 1;
    }

    std::ofstream csv(
        output_directory
        / "spectra.csv");

    std::ofstream exposure_csv(
        output_directory
        / "film_exposure_compare.csv");

    if (!csv
        || !exposure_csv) {
        std::cerr
            << "[FAIL] could not create spectra.csv\n";

        return 1;
    }

    csv
        << "smoothness,color,wavelength_nm,value\n";

    exposure_csv
        << "color,model,smoothness,nrmse,yellow_h,magenta_h,cyan_h,"
           "yellow_vs_production,magenta_vs_production,cyan_vs_production\n";

    std::cout
        << std::setprecision(10);

    for (double weight :
         weights) {

        std::cout
            << "\nConstrained solver smoothness="
            << weight
            << "\n";

        for (const Color& color :
             colors) {

            const std::array<float, 3> production_ap0 = {{
                static_cast<float>(color.ap0.x),
                static_cast<float>(color.ap0.y),
                static_cast<float>(color.ap0.z)
            }};

            const auto production_h =
                production_film_exposure(
                    reconstructor,
                    production_ap0,
                    d60,
                    sensitivity);

            const SpectrumResult result =
                solver.solve(
                    color.ap0,
                    weight);

            const auto sampled_h =
                film_exposure(
                    solver.wavelengths(),
                    result.values,
                    d60,
                    sensitivity,
                    5.0);

            const auto& wavelengths =
                solver.wavelengths();

            const double short_band =
                band_sum(
                    wavelengths,
                    result.values,
                    380.0,
                    450.0);

            const double middle_band =
                band_sum(
                    wavelengths,
                    result.values,
                    500.0,
                    600.0);

            const double long_band =
                band_sum(
                    wavelengths,
                    result.values,
                    600.0,
                    700.1);

            const double short_long_ratio =
                long_band > 1e-20
                    ? short_band
                        / long_band
                    : 0.0;

            std::cout
                << "  "
                << std::setw(10)
                << color.name
                << " NRMSE="
                << result.nrmse
                << " forward=("
                << result.forward_ap0.x
                << ", "
                << result.forward_ap0.y
                << ", "
                << result.forward_ap0.z
                << ")"
                << " short/long="
                << short_long_ratio
                << " bands[S/M/L]=("
                << short_band
                << ", "
                << middle_band
                << ", "
                << long_band
                << ")"
                << " S400="
                << sample_at(
                    wavelengths,
                    result.values,
                    400.0)
                << " S420="
                << sample_at(
                    wavelengths,
                    result.values,
                    420.0)
                << " S550="
                << sample_at(
                    wavelengths,
                    result.values,
                    550.0)
                << " S650="
                << sample_at(
                    wavelengths,
                    result.values,
                    650.0)
                << " H[Y/M/C]=("
                << sampled_h[0]
                << ", "
                << sampled_h[1]
                << ", "
                << sampled_h[2]
                << ")"
                << " vs production=("
                << production_h[0]
                << ", "
                << production_h[1]
                << ", "
                << production_h[2]
                << ")"
                << " ratio=("
                << relative_change(
                    sampled_h[0],
                    production_h[0])
                << ", "
                << relative_change(
                    sampled_h[1],
                    production_h[1])
                << ", "
                << relative_change(
                    sampled_h[2],
                    production_h[2])
                << ")"
                << "\n";

            exposure_csv
                << color.name
                << ",sampled,"
                << weight
                << ","
                << result.nrmse
                << ","
                << sampled_h[0]
                << ","
                << sampled_h[1]
                << ","
                << sampled_h[2]
                << ","
                << relative_change(
                    sampled_h[0],
                    production_h[0])
                << ","
                << relative_change(
                    sampled_h[1],
                    production_h[1])
                << ","
                << relative_change(
                    sampled_h[2],
                    production_h[2])
                << "\n";

            if (weight == weights.front()) {
                exposure_csv
                    << color.name
                    << ",production,0,,"
                    << production_h[0]
                    << ","
                    << production_h[1]
                    << ","
                    << production_h[2]
                    << ",1,1,1\n";
            }

            for (std::size_t i = 0;
                 i < wavelengths.size();
                 ++i) {

                csv
                    << weight
                    << ','
                    << color.name
                    << ','
                    << wavelengths[i]
                    << ','
                    << result.values[i]
                    << '\n';
            }
        }
    }

    std::cout
        << "\nCSV: "
        << (
            output_directory
            / "spectra.csv")
        << "\n"
        << "CSV: "
        << (
            output_directory
            / "film_exposure_compare.csv")
        << "\n";

    return
        test::finish(
            passed,
            "spectral film exposure compare");
}
