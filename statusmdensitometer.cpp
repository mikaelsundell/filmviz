// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "statusmdensitometer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

double
StatusMDensitometer::log_product(
    int channel,
    double wavelength_nm)
{
    // ISO 5-3 Status-M transmission spectral products. Table values are
    // log10 products normalized to a peak of 5.000. Between tabulated 10 nm
    // samples we interpolate in log-product space; outside the explicit
    // passband we continue with the endpoint slopes used by the validation
    // work in Calibrate5-7.
    static const std::vector<double> blue_nm = {
        410, 420, 430, 440, 450, 460,
        470, 480, 490, 500, 510
    };

    static const std::vector<double> blue_log = {
        2.103, 4.111, 4.632, 4.871, 5.000, 4.955,
        4.743, 4.343, 3.743, 2.990, 1.852
    };

    static const std::vector<double> green_nm = {
        470, 480, 490, 500, 510, 520, 530, 540,
        550, 560, 570, 580, 590, 600, 610
    };

    static const std::vector<double> green_log = {
        1.152, 2.207, 3.156, 3.804, 4.272,
        4.626, 4.872, 5.000, 4.995, 4.818,
        4.458, 3.915, 3.172, 2.239, 1.070
    };

    static const std::vector<double> red_nm = {
        620, 630, 640, 650, 660, 670, 680, 690,
        700, 710, 720, 730, 740, 750, 760, 770
    };

    static const std::vector<double> red_log = {
        2.109, 4.479, 5.000, 4.899, 4.578, 4.252,
        3.875, 3.491, 3.099, 2.687, 2.269, 1.859,
        1.449, 1.054, 0.654, 0.254
    };

    const std::vector<double>* x = nullptr;
    const std::vector<double>* y = nullptr;
    double left_slope = 0.0;
    double right_slope = 0.0;

    if (channel == 0) {
        x = &red_nm;
        y = &red_log;
        left_slope = 0.260;
        right_slope = -0.040;
    }
    else if (channel == 1) {
        x = &green_nm;
        y = &green_log;
        left_slope = 0.106;
        right_slope = -0.120;
    }
    else {
        x = &blue_nm;
        y = &blue_log;
        left_slope = 0.250;
        right_slope = -0.220;
    }

    if (wavelength_nm <= x->front()) {
        return
            y->front()
            + left_slope
                * (wavelength_nm - x->front());
    }

    if (wavelength_nm >= x->back()) {
        return
            y->back()
            + right_slope
                * (wavelength_nm - x->back());
    }

    const auto upper =
        std::upper_bound(
            x->begin(),
            x->end(),
            wavelength_nm);

    const std::size_t i1 =
        static_cast<std::size_t>(
            upper - x->begin());

    const std::size_t i0 = i1 - 1;

    const double t =
        (wavelength_nm - (*x)[i0])
        / ((*x)[i1] - (*x)[i0]);

    return
        (*y)[i0]
        + t
            * ((*y)[i1] - (*y)[i0]);
}

double
StatusMDensitometer::weight(
    int channel,
    double wavelength_nm)
{
    const double exponent =
        log_product(
            channel,
            wavelength_nm)
        - 5.0;

    if (exponent < -12.0) {
        return 0.0;
    }

    return
        std::pow(
            10.0,
            exponent);
}

StatusMDensitometer::Density
StatusMDensitometer::measure(
    const SampledCurve& spectral_density) const
{
    Density result = {{
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN()
    }};

    if (!spectral_density.valid()) {
        return result;
    }

    for (int channel = 0;
         channel < 3;
         ++channel) {

        double numerator = 0.0;
        double denominator = 0.0;

        for (std::size_t i = 0;
             i < spectral_density.x.size();
             ++i) {

            const double wavelength =
                static_cast<double>(
                    spectral_density.x[i]);

            const double density =
                static_cast<double>(
                    spectral_density.y[i]);

            const double response =
                weight(
                    channel,
                    wavelength);

            if (!std::isfinite(density)
                || !std::isfinite(response)
                || response <= 0.0) {

                continue;
            }

            const double transmittance =
                std::pow(
                    10.0,
                    -density);

            numerator +=
                response
                * transmittance;

            denominator +=
                response;
        }

        if (numerator > 0.0
            && denominator > 0.0) {

            result[channel] =
                -std::log10(
                    numerator
                    / denominator);
        }
    }

    return result;
}
