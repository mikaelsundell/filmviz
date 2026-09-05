// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "spectralreconstructor.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr float kRgb2SpecLambdaMinNm = 360.0f;
constexpr float kRgb2SpecLambdaMaxNm = 830.0f;

} // namespace

SpectralReconstructor::SpectralReconstructor(
    const std::string& filename)
{
    load(filename);
}

SpectralReconstructor::~SpectralReconstructor()
{
    reset();
}

SpectralReconstructor::SpectralReconstructor(
    SpectralReconstructor&& other) noexcept
    : model_(other.model_)
    , filename_(std::move(other.filename_))
{
    other.model_ = nullptr;
}

SpectralReconstructor&
SpectralReconstructor::operator=(
    SpectralReconstructor&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    reset();

    model_ = other.model_;
    filename_ = std::move(other.filename_);

    other.model_ = nullptr;

    return *this;
}

bool
SpectralReconstructor::load(
    const std::string& filename)
{
    reset();

    model_ =
        rgb2spec_load(
            filename.c_str());

    if (!model_) {
        return false;
    }

    filename_ = filename;
    return true;
}

void
SpectralReconstructor::reset()
{
    if (model_) {
        rgb2spec_free(model_);
        model_ = nullptr;
    }

    filename_.clear();
}

bool
SpectralReconstructor::valid() const
{
    return model_ != nullptr;
}

const std::string&
SpectralReconstructor::filename() const
{
    return filename_;
}

SpectralReconstructor::Spectrum
SpectralReconstructor::reconstruct(
    const std::array<float, 3>& linear_rgb,
    Method method) const
{
    Spectrum result;

    if (!model_) {
        return result;
    }

    // FilmViz scene-linear scaling semantics:
    //   1. clamp negative RGB to zero;
    //   2. scale values above one into rgb2spec's [0,1] lookup domain;
    //   3. reconstruct the normalized spectrum;
    //   4. restore scene-linear scale during evaluation.
    float rgb[3] = {
        std::max(0.0f, linear_rgb[0]),
        std::max(0.0f, linear_rgb[1]),
        std::max(0.0f, linear_rgb[2])
    };

    const float max_component =
        std::max(rgb[0], std::max(rgb[1], rgb[2]));

    result.scale =
        std::max(1.0f, max_component);

    rgb[0] /= result.scale;
    rgb[1] /= result.scale;
    rgb[2] /= result.scale;

    if (method == Method::Optimized) {
        rgb2spec_fetch_opt(
            model_,
            rgb,
            result.coeff);
    }
    else {
        rgb2spec_fetch(
            model_,
            rgb,
            result.coeff);
    }

    return result;
}

float
SpectralReconstructor::evaluate(
    const Spectrum& spectrum,
    float wavelength_nm) const
{
    if (!model_) {
        return 0.0f;
    }

    return
        spectrum.scale
        * rgb2spec_eval_precise(
            const_cast<float*>(spectrum.coeff),
            wavelength_nm);
}

SampledCurve
SpectralReconstructor::sample(
    const Spectrum& spectrum,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm) const
{
    SampledCurve result;

    if (!model_
        || wavelength_step_nm <= 0.0f
        || wavelength_max_nm < wavelength_min_nm) {
        return result;
    }

    for (float wavelength = wavelength_min_nm;
         wavelength <= wavelength_max_nm + 0.001f;
         wavelength += wavelength_step_nm) {

        result.x.push_back(wavelength);
        result.y.push_back(
            evaluate(
                spectrum,
                wavelength));
    }

    return result;
}

SampledCurve
SpectralReconstructor::reconstruct_sampled(
    const std::array<float, 3>& linear_rgb,
    float wavelength_min_nm,
    float wavelength_max_nm,
    float wavelength_step_nm,
    Method method) const
{
    return sample(
        reconstruct(
            linear_rgb,
            method),
        wavelength_min_nm,
        wavelength_max_nm,
        wavelength_step_nm);
}

bool
SpectralReconstructor::has_forward_model() const
{
    return
        model_
        && model_->fwd
        && model_->nfine > 0;
}

std::array<float, 3>
SpectralReconstructor::forward_rgb(
    const Spectrum& spectrum) const
{
    std::array<float, 3> result = {{0.0f, 0.0f, 0.0f}};

    if (!has_forward_model()) {
        return result;
    }

    const int n =
        static_cast<int>(model_->nfine);

    const float* lambda_scaled =
        model_->fwd;

    const float* weights =
        model_->fwd + n;

    for (int i = 0; i < n; ++i) {
        const float wavelength_nm =
            kRgb2SpecLambdaMinNm
            + lambda_scaled[i]
                * (kRgb2SpecLambdaMaxNm - kRgb2SpecLambdaMinNm);

        const float value =
            evaluate(
                spectrum,
                wavelength_nm);

        for (int channel = 0; channel < 3; ++channel) {
            result[channel] +=
                weights[channel * n + i]
                * value;
        }
    }

    return result;
}
