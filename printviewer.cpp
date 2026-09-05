// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "printviewer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

namespace {

struct Matrix3
{
    double m[3][3];
};

std::array<double, 3>
multiply(
    const Matrix3& matrix,
    const std::array<double, 3>& value)
{
    return {{
        matrix.m[0][0] * value[0]
            + matrix.m[0][1] * value[1]
            + matrix.m[0][2] * value[2],

        matrix.m[1][0] * value[0]
            + matrix.m[1][1] * value[1]
            + matrix.m[1][2] * value[2],

        matrix.m[2][0] * value[0]
            + matrix.m[2][1] * value[1]
            + matrix.m[2][2] * value[2]
    }};
}

Matrix3
multiply(
    const Matrix3& a,
    const Matrix3& b)
{
    Matrix3 result = {{
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0}
    }};

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            for (int k = 0; k < 3; ++k) {
                result.m[row][col] +=
                    a.m[row][k]
                    * b.m[k][col];
            }
        }
    }

    return result;
}

double
determinant(
    const Matrix3& a)
{
    return
        a.m[0][0]
        * (a.m[1][1] * a.m[2][2]
           - a.m[1][2] * a.m[2][1])
        - a.m[0][1]
        * (a.m[1][0] * a.m[2][2]
           - a.m[1][2] * a.m[2][0])
        + a.m[0][2]
        * (a.m[1][0] * a.m[2][1]
           - a.m[1][1] * a.m[2][0]);
}

Matrix3
inverse(
    const Matrix3& a)
{
    const double det =
        determinant(a);

    if (std::abs(det) < 1e-15) {
        Matrix3 identity = {{
            {1.0, 0.0, 0.0},
            {0.0, 1.0, 0.0},
            {0.0, 0.0, 1.0}
        }};
        return identity;
    }

    const double inv_det =
        1.0 / det;

    Matrix3 result;

    result.m[0][0] =
        (a.m[1][1] * a.m[2][2]
         - a.m[1][2] * a.m[2][1])
        * inv_det;

    result.m[0][1] =
        (a.m[0][2] * a.m[2][1]
         - a.m[0][1] * a.m[2][2])
        * inv_det;

    result.m[0][2] =
        (a.m[0][1] * a.m[1][2]
         - a.m[0][2] * a.m[1][1])
        * inv_det;

    result.m[1][0] =
        (a.m[1][2] * a.m[2][0]
         - a.m[1][0] * a.m[2][2])
        * inv_det;

    result.m[1][1] =
        (a.m[0][0] * a.m[2][2]
         - a.m[0][2] * a.m[2][0])
        * inv_det;

    result.m[1][2] =
        (a.m[0][2] * a.m[1][0]
         - a.m[0][0] * a.m[1][2])
        * inv_det;

    result.m[2][0] =
        (a.m[1][0] * a.m[2][1]
         - a.m[1][1] * a.m[2][0])
        * inv_det;

    result.m[2][1] =
        (a.m[0][1] * a.m[2][0]
         - a.m[0][0] * a.m[2][1])
        * inv_det;

    result.m[2][2] =
        (a.m[0][0] * a.m[1][1]
         - a.m[0][1] * a.m[1][0])
        * inv_det;

    return result;
}

Matrix3
bradford_adaptation(
    const std::array<double, 3>& source_white_xyz,
    const std::array<double, 3>& target_white_xyz)
{
    // Same Bradford convention used by InputTransform.
    const Matrix3 M = {{
        { 0.8951,  0.2664, -0.1614},
        {-0.7502,  1.7135,  0.0367},
        { 0.0389, -0.0685,  1.0296}
    }};

    const auto source_lms =
        multiply(M, source_white_xyz);

    const auto target_lms =
        multiply(M, target_white_xyz);

    Matrix3 S = {{
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0}
    }};

    for (int i = 0; i < 3; ++i) {
        S.m[i][i] =
            source_lms[i] != 0.0
                ? target_lms[i] / source_lms[i]
                : 1.0;
    }

    return multiply(
        multiply(
            inverse(M),
            S),
        M);
}

std::array<double, 3>
xy_to_xyz_white(
    double x,
    double y)
{
    if (std::abs(y) < 1e-15) {
        return {{0.0, 1.0, 0.0}};
    }

    return {{
        x / y,
        1.0,
        (1.0 - x - y) / y
    }};
}

const Matrix3&
xyz_d60_to_ap0_matrix()
{
    // Same matrix convention used by InputTransform.
    static const Matrix3 matrix = {{
        { 1.0498110175, 0.0000000000, -0.0000974845},
        {-0.4959030231, 1.3733130458,  0.0982400361},
        { 0.0000000000, 0.0000000000,  0.9912520182}
    }};

    return matrix;
}

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

bool
range_covers(
    const SampledCurve& curve,
    float minimum_nm,
    float maximum_nm)
{
    return
        curve.valid()
        && curve.x.front() <= minimum_nm
        && curve.x.back() >= maximum_nm;
}

} // namespace

bool
PrintViewer::load(
    const std::string& cie_observer_filename,
    const std::string& viewing_illuminant_filename,
    const Settings& settings)
{
    settings_ = settings;
    cie_observer_filename_ = cie_observer_filename;
    viewing_illuminant_filename_ = viewing_illuminant_filename;

    x_bar_ = SampledCurve();
    y_bar_ = SampledCurve();
    z_bar_ = SampledCurve();
    viewing_illuminant_ = SampledCurve();

    normalization_k_ = 0.0f;
    integration_wavelengths_.clear();
    integration_weight_x_.clear();
    integration_weight_y_.clear();
    integration_weight_z_.clear();
    adaptation_to_d60_ = {{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    }};
    viewing_white_xyz_ = XYZ();
    viewing_white_xy_ = xy();
    target_d60_xyz_ = XYZ();
    target_d60_xy_ = xy();
    observer_covers_integration_range_ = false;
    illuminant_covers_integration_range_ = false;
    valid_ = false;

    if (settings_.wavelength_step_nm <= 0.0f
        || settings_.wavelength_max_nm
            < settings_.wavelength_min_nm) {

        std::cerr
            << "error: invalid print viewing wavelength settings"
            << std::endl;
        return false;
    }

    if (!load_cie_observer(
            cie_observer_filename_)) {
        return false;
    }

    if (!load_illuminant(
            viewing_illuminant_filename_)) {
        return false;
    }

    observer_covers_integration_range_ =
        range_covers(
            x_bar_,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm)
        && range_covers(
            y_bar_,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm)
        && range_covers(
            z_bar_,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm);

    illuminant_covers_integration_range_ =
        range_covers(
            viewing_illuminant_,
            settings_.wavelength_min_nm,
            settings_.wavelength_max_nm);

    if (!observer_covers_integration_range_) {
        std::cerr
            << "error: CIE observer does not cover requested integration range "
            << settings_.wavelength_min_nm
            << "-"
            << settings_.wavelength_max_nm
            << " nm"
            << std::endl;
        return false;
    }

    if (!illuminant_covers_integration_range_) {
        std::cerr
            << "error: viewing illuminant does not cover requested integration range "
            << settings_.wavelength_min_nm
            << "-"
            << settings_.wavelength_max_nm
            << " nm"
            << std::endl;
        return false;
    }

    return initialize_colorimetry();
}

bool
PrintViewer::valid() const
{
    return valid_;
}

PrintViewer::Result
PrintViewer::view(
    const SampledCurve& print_transmittance) const
{
    Result result;

    if (!valid_
        || !print_transmittance.valid()
        || print_transmittance.x.front()
            > settings_.wavelength_min_nm
        || print_transmittance.x.back()
            < settings_.wavelength_max_nm) {
        return result;
    }

    result.viewed_xyz =
        integrate_xyz(
            &print_transmittance);

    result.viewed_xy =
        xyz_to_xy(
            result.viewed_xyz);

    result.adapted_xyz_d60 =
        adapt_to_d60(
            result.viewed_xyz);

    result.aces2065_1 =
        xyz_d60_to_ap0(
            result.adapted_xyz_d60);

    return result;
}


float
PrintViewer::view_luminance(
    const SampledCurve& print_transmittance) const
{
    if (!valid_
        || !print_transmittance.valid()
        || print_transmittance.x.front()
            > settings_.wavelength_min_nm
        || print_transmittance.x.back()
            < settings_.wavelength_max_nm) {
        return 0.0f;
    }

    return integrate_y(
        &print_transmittance);
}

PrintViewer::Result
PrintViewer::view_flat_transmittance(
    float transmittance) const
{
    SampledCurve flat;

    if (!valid_) {
        return Result();
    }

    for (float wavelength = settings_.wavelength_min_nm;
         wavelength <= settings_.wavelength_max_nm + 0.001f;
         wavelength += settings_.wavelength_step_nm) {

        flat.x.push_back(wavelength);
        flat.y.push_back(transmittance);
    }

    return view(flat);
}

SampledCurve
PrintViewer::viewed_spectrum(
    const SampledCurve& print_transmittance) const
{
    SampledCurve result;

    if (!valid_
        || !print_transmittance.valid()) {
        return result;
    }

    for (float wavelength = settings_.wavelength_min_nm;
         wavelength <= settings_.wavelength_max_nm + 0.001f;
         wavelength += settings_.wavelength_step_nm) {

        const float illuminant =
            viewing_illuminant_.sample(
                wavelength,
                0.0f);

        const float transmission =
            print_transmittance.sample(
                wavelength,
                0.0f);

        result.x.push_back(wavelength);
        result.y.push_back(
            illuminant
            * std::max(
                0.0f,
                transmission));
    }

    return result;
}

const PrintViewer::Settings&
PrintViewer::settings() const
{
    return settings_;
}

const std::string&
PrintViewer::cie_observer_filename() const
{
    return cie_observer_filename_;
}

const std::string&
PrintViewer::viewing_illuminant_filename() const
{
    return viewing_illuminant_filename_;
}

const SampledCurve&
PrintViewer::x_bar() const
{
    return x_bar_;
}

const SampledCurve&
PrintViewer::y_bar() const
{
    return y_bar_;
}

const SampledCurve&
PrintViewer::z_bar() const
{
    return z_bar_;
}

const SampledCurve&
PrintViewer::viewing_illuminant() const
{
    return viewing_illuminant_;
}

float
PrintViewer::normalization_k() const
{
    return normalization_k_;
}

PrintViewer::XYZ
PrintViewer::viewing_white_xyz() const
{
    return viewing_white_xyz_;
}

PrintViewer::xy
PrintViewer::viewing_white_xy() const
{
    return viewing_white_xy_;
}

PrintViewer::XYZ
PrintViewer::target_d60_xyz() const
{
    return target_d60_xyz_;
}

PrintViewer::xy
PrintViewer::target_d60_xy() const
{
    return target_d60_xy_;
}

bool
PrintViewer::observer_covers_integration_range() const
{
    return observer_covers_integration_range_;
}

bool
PrintViewer::illuminant_covers_integration_range() const
{
    return illuminant_covers_integration_range_;
}

bool
PrintViewer::load_cie_observer(
    const std::string& filename)
{
    std::ifstream file(
        filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open CIE observer CSV: "
            << filename
            << std::endl;
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.size() < 4) {
            continue;
        }

        float wavelength = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        if (!parse_float(fields[0], wavelength)
            || !parse_float(fields[1], x)
            || !parse_float(fields[2], y)
            || !parse_float(fields[3], z)) {
            continue;
        }

        x_bar_.x.push_back(wavelength);
        x_bar_.y.push_back(x);

        y_bar_.x.push_back(wavelength);
        y_bar_.y.push_back(y);

        z_bar_.x.push_back(wavelength);
        z_bar_.y.push_back(z);
    }

    if (!x_bar_.valid()
        || !y_bar_.valid()
        || !z_bar_.valid()) {

        std::cerr
            << "error: CIE observer CSV did not contain usable xbar/ybar/zbar data"
            << std::endl;
        return false;
    }

    return true;
}

bool
PrintViewer::load_illuminant(
    const std::string& filename)
{
    std::ifstream file(
        filename.c_str());

    if (!file) {
        std::cerr
            << "error: could not open viewing illuminant CSV: "
            << filename
            << std::endl;
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const auto fields =
            split_csv_line(line);

        if (fields.size() < 2) {
            continue;
        }

        float wavelength = 0.0f;
        float power = 0.0f;

        if (!parse_float(fields[0], wavelength)
            || !parse_float(fields[1], power)) {
            continue;
        }

        viewing_illuminant_.x.push_back(wavelength);
        viewing_illuminant_.y.push_back(power);
    }

    if (!viewing_illuminant_.valid()) {
        std::cerr
            << "error: viewing illuminant CSV did not contain usable SPD data"
            << std::endl;
        return false;
    }

    return true;
}

bool
PrintViewer::initialize_colorimetry()
{
    double denominator = 0.0;

    for (float wavelength = settings_.wavelength_min_nm;
         wavelength <= settings_.wavelength_max_nm + 0.001f;
         wavelength += settings_.wavelength_step_nm) {

        const double illuminant =
            static_cast<double>(
                viewing_illuminant_.sample(
                    wavelength,
                    0.0f));

        const double ybar =
            static_cast<double>(
                y_bar_.sample(
                    wavelength,
                    0.0f));

        denominator +=
            illuminant
            * ybar
            * static_cast<double>(
                settings_.wavelength_step_nm);
    }

    if (!std::isfinite(denominator)
        || denominator <= 0.0) {

        std::cerr
            << "error: viewing illuminant / CIE observer normalization is non-positive"
            << std::endl;
        return false;
    }

    normalization_k_ =
        static_cast<float>(
            1.0 / denominator);

    // optimized production synthesis: precompute the complete normalized XYZ integration
    // weights on the active wavelength grid. The print dye model emits this
    // exact grid, so the per-pixel viewer can use direct indexed dot products
    // instead of repeatedly interpolating the illuminant and CMFs.
    integration_wavelengths_.clear();
    integration_weight_x_.clear();
    integration_weight_y_.clear();
    integration_weight_z_.clear();

    for (float wavelength = settings_.wavelength_min_nm;
         wavelength <= settings_.wavelength_max_nm + 0.001f;
         wavelength += settings_.wavelength_step_nm) {

        const float illuminant =
            viewing_illuminant_.sample(
                wavelength,
                0.0f);

        const double scale =
            static_cast<double>(illuminant)
            * static_cast<double>(
                settings_.wavelength_step_nm);

        integration_wavelengths_.push_back(
            wavelength);

        integration_weight_x_.push_back(
            scale
            * static_cast<double>(
                x_bar_.sample(
                    wavelength,
                    0.0f)));

        integration_weight_y_.push_back(
            scale
            * static_cast<double>(
                y_bar_.sample(
                    wavelength,
                    0.0f)));

        integration_weight_z_.push_back(
            scale
            * static_cast<double>(
                z_bar_.sample(
                    wavelength,
                    0.0f)));
    }

    viewing_white_xyz_ =
        integrate_xyz(nullptr);

    viewing_white_xy_ =
        xyz_to_xy(
            viewing_white_xyz_);

    const auto d60 =
        xy_to_xyz_white(
            0.32168,
            0.33767);

    target_d60_xyz_.x =
        static_cast<float>(d60[0]);
    target_d60_xyz_.y =
        static_cast<float>(d60[1]);
    target_d60_xyz_.z =
        static_cast<float>(d60[2]);

    target_d60_xy_.x = 0.32168f;
    target_d60_xy_.y = 0.33767f;

    const std::array<double, 3> source_white = {{
        viewing_white_xyz_.x,
        viewing_white_xyz_.y,
        viewing_white_xyz_.z
    }};

    const std::array<double, 3> destination_white = {{
        target_d60_xyz_.x,
        target_d60_xyz_.y,
        target_d60_xyz_.z
    }};

    const Matrix3 cached_adaptation =
        bradford_adaptation(
            source_white,
            destination_white);

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            adaptation_to_d60_[row * 3 + col] =
                cached_adaptation.m[row][col];
        }
    }

    if (!std::isfinite(viewing_white_xyz_.x)
        || !std::isfinite(viewing_white_xyz_.y)
        || !std::isfinite(viewing_white_xyz_.z)
        || std::abs(viewing_white_xyz_.y - 1.0f) > 1e-4f) {

        std::cerr
            << "error: viewing-white normalization validation failed; Y="
            << viewing_white_xyz_.y
            << std::endl;
        return false;
    }

    valid_ = true;

    const Result flat =
        view_flat_transmittance(1.0f);

    const float neutral_span =
        std::max(
            flat.aces2065_1[0],
            std::max(
                flat.aces2065_1[1],
                flat.aces2065_1[2]))
        - std::min(
            flat.aces2065_1[0],
            std::min(
                flat.aces2065_1[1],
                flat.aces2065_1[2]));

    if (!std::isfinite(neutral_span)
        || neutral_span > 5e-4f) {

        std::cerr
            << "error: flat-transmittance D60 adaptation validation failed; AP0 span="
            << neutral_span
            << std::endl;
        valid_ = false;
        return false;
    }

    return true;
}

PrintViewer::XYZ
PrintViewer::integrate_xyz(
    const SampledCurve* transmittance) const
{
    XYZ result;

    if (normalization_k_ <= 0.0f
        || integration_weight_x_.empty()
        || integration_weight_y_.size() != integration_weight_x_.size()
        || integration_weight_z_.size() != integration_weight_x_.size()) {
        return result;
    }

    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;

    if (!transmittance) {
        for (std::size_t i = 0; i < integration_weight_x_.size(); ++i) {
            X += integration_weight_x_[i];
            Y += integration_weight_y_[i];
            Z += integration_weight_z_[i];
        }
    }
    else if (transmittance_matches_integration_grid(*transmittance)) {
        for (std::size_t i = 0; i < integration_weight_x_.size(); ++i) {
            const double T =
                static_cast<double>(
                    std::max(
                        0.0f,
                        transmittance->y[i]));

            X += integration_weight_x_[i] * T;
            Y += integration_weight_y_[i] * T;
            Z += integration_weight_z_[i] * T;
        }
    }
    else {
        for (std::size_t i = 0; i < integration_wavelengths_.size(); ++i) {
            const double T =
                static_cast<double>(
                    std::max(
                        0.0f,
                        transmittance->sample(
                            integration_wavelengths_[i],
                            0.0f)));

            X += integration_weight_x_[i] * T;
            Y += integration_weight_y_[i] * T;
            Z += integration_weight_z_[i] * T;
        }
    }

    result.x =
        normalization_k_
        * static_cast<float>(X);

    result.y =
        normalization_k_
        * static_cast<float>(Y);

    result.z =
        normalization_k_
        * static_cast<float>(Z);

    return result;
}

float
PrintViewer::integrate_y(
    const SampledCurve* transmittance) const
{
    if (normalization_k_ <= 0.0f
        || integration_weight_y_.empty()) {
        return 0.0f;
    }

    double Y = 0.0;

    if (!transmittance) {
        for (double weight : integration_weight_y_) {
            Y += weight;
        }
    }
    else if (transmittance_matches_integration_grid(*transmittance)) {
        for (std::size_t i = 0; i < integration_weight_y_.size(); ++i) {
            Y += integration_weight_y_[i]
                * static_cast<double>(
                    std::max(
                        0.0f,
                        transmittance->y[i]));
        }
    }
    else {
        for (std::size_t i = 0; i < integration_wavelengths_.size(); ++i) {
            Y += integration_weight_y_[i]
                * static_cast<double>(
                    std::max(
                        0.0f,
                        transmittance->sample(
                            integration_wavelengths_[i],
                            0.0f)));
        }
    }

    return normalization_k_
        * static_cast<float>(Y);
}

bool
PrintViewer::transmittance_matches_integration_grid(
    const SampledCurve& transmittance) const
{
    if (!transmittance.valid()
        || transmittance.x.size() != integration_wavelengths_.size()
        || transmittance.y.size() != integration_wavelengths_.size()
        || integration_wavelengths_.empty()) {
        return false;
    }

    // The active dye model is built on this exact grid. Endpoint and one-step
    // checks keep the fast path safe for external diagnostic curves.
    const float eps = 1e-4f;

    if (std::abs(
            transmittance.x.front()
            - integration_wavelengths_.front()) > eps
        || std::abs(
            transmittance.x.back()
            - integration_wavelengths_.back()) > eps) {
        return false;
    }

    if (transmittance.x.size() > 1
        && std::abs(
            (transmittance.x[1] - transmittance.x[0])
            - settings_.wavelength_step_nm) > eps) {
        return false;
    }

    return true;
}

PrintViewer::XYZ
PrintViewer::adapt_to_d60(
    const XYZ& xyz) const
{
    XYZ result;

    result.x = static_cast<float>(
        adaptation_to_d60_[0] * xyz.x
        + adaptation_to_d60_[1] * xyz.y
        + adaptation_to_d60_[2] * xyz.z);

    result.y = static_cast<float>(
        adaptation_to_d60_[3] * xyz.x
        + adaptation_to_d60_[4] * xyz.y
        + adaptation_to_d60_[5] * xyz.z);

    result.z = static_cast<float>(
        adaptation_to_d60_[6] * xyz.x
        + adaptation_to_d60_[7] * xyz.y
        + adaptation_to_d60_[8] * xyz.z);

    return result;
}

PrintViewer::xy
PrintViewer::xyz_to_xy(
    const XYZ& xyz)
{
    xy result;

    const float sum =
        xyz.x + xyz.y + xyz.z;

    if (std::abs(sum) <= 1e-20f) {
        return result;
    }

    result.x = xyz.x / sum;
    result.y = xyz.y / sum;
    return result;
}

std::array<float, 3>
PrintViewer::xyz_d60_to_ap0(
    const XYZ& xyz)
{
    const std::array<double, 3> value = {{
        xyz.x,
        xyz.y,
        xyz.z
    }};

    const auto rgb =
        multiply(
            xyz_d60_to_ap0_matrix(),
            value);

    return {{
        static_cast<float>(rgb[0]),
        static_cast<float>(rgb[1]),
        static_cast<float>(rgb[2])
    }};
}
