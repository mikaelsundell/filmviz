// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "granularitymodel.h"
#include "imageprocessor.h"
#include "test_common.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

int
main()
{
    bool passed = true;
    passed &= test::near(GranularityModel::grain_size_pixels(1.0f, 2048, 24.89f),
        1.0, 1e-6, "Super 35 reference grain size");
    passed &= test::near(GranularityModel::grain_size_pixels(1.0f, 4096, 24.89f),
        2.0, 1e-6, "grain footprint doubles at double resolution");
    passed &= test::near(GranularityModel::grain_size_pixels(1.0f, 1024, 24.89f),
        0.5, 1e-6, "preview footprint follows working resolution");
    passed &= test::near(GranularityModel::grain_size_pixels(1.0f, 2048, 10.26f),
        24.89 / 10.26, 1e-6, "16mm enlarges grain at equal output resolution");
    passed &= test::near(GranularityModel::grain_size_pixels(1.5f, 2048, 52.48f),
        1.5 * 24.89 / 52.48, 1e-6, "65mm mapping retains artistic scale multiplier");
    GranularityModel model;
    const std::string resources = FILMVIZ_TEST_RESOURCE_DIR;

    passed &= test::check(
        model.load(
            resources
                + "/profiles/verita_200d/kodak_verita_200d_diffuse_rms_granularity_curves.csv",
            resources
                + "/profiles/kodak_2383/kodak_2383_diffuse_rms_granularity_curves.csv"),
        "measured Verita and 2383 granularity curves load");

    const FilmDensity negative_density = {
        1.587837f,
        1.886565f,
        2.320899f
    };

    const FilmDensity negative_sigma =
        model.negative_sigma(
            negative_density);

    passed &= test::near(
        negative_sigma.red,
        0.007731,
        1e-6,
        "Verita red RMS lookup");

    passed &= test::near(
        negative_sigma.green,
        0.006458,
        1e-6,
        "Verita green RMS lookup");

    passed &= test::near(
        negative_sigma.blue,
        0.016148,
        1e-6,
        "Verita blue RMS lookup");

    const FilmDensity print_density = {
        0.171354f,
        0.229659f,
        0.279110f
    };

    const FilmDensity print_sigma =
        model.print_sigma(
            print_density);

    passed &= test::near(
        print_sigma.red,
        0.003765,
        1e-6,
        "2383 red RMS lookup");

    passed &= test::near(
        print_sigma.green,
        0.004553,
        1e-6,
        "2383 green RMS lookup");

    passed &= test::near(
        print_sigma.blue,
        0.023226,
        1e-6,
        "2383 blue RMS lookup");

    const float repeated_a =
        GranularityModel::normal_sample(
            42u,
            17,
            29,
            0,
            1);

    const float repeated_b =
        GranularityModel::normal_sample(
            42u,
            17,
            29,
            0,
            1);

    passed &= test::near(
        repeated_a,
        repeated_b,
        0.0,
        "grain is deterministic for a fixed seed and coordinate");

    constexpr int sample_count = 100000;
    double sum = 0.0;
    double sum_squared = 0.0;

    for (int i = 0; i < sample_count; ++i) {
        const double value =
            GranularityModel::normal_sample(
                91u,
                i,
                i * 7,
                1,
                2);

        sum += value;
        sum_squared += value * value;
    }

    const double mean = sum / sample_count;
    const double variance =
        sum_squared / sample_count
        - mean * mean;

    passed &= test::check(
        std::abs(mean) < 0.015,
        "grain generator has approximately zero mean");

    passed &= test::check(
        std::abs(std::sqrt(variance) - 1.0) < 0.02,
        "grain generator has approximately unit RMS");

    const std::array<float, 3> channel_noise = {{
        0.012f,
        -0.007f,
        0.031f
    }};
    const std::array<float, 3> neutral =
        ImageProcessor::mix_grain_chroma(
            channel_noise,
            0.0f);
    const std::array<float, 3> measured =
        ImageProcessor::mix_grain_chroma(
            channel_noise,
            1.0f);
    const std::array<float, 3> reduced =
        ImageProcessor::mix_grain_chroma(
            channel_noise,
            0.35f);

    passed &= test::near(
        neutral[0],
        neutral[1],
        1e-8,
        "zero chroma makes red and green grain identical");

    passed &= test::near(
        neutral[1],
        neutral[2],
        1e-8,
        "zero chroma makes green and blue grain identical");

    for (int channel = 0; channel < 3; ++channel) {
        passed &= test::near(
            measured[channel],
            channel_noise[channel],
            1e-8,
            "unit chroma preserves measured channel grain");
    }

    const auto luma =
        [](const std::array<float, 3>& value) {
            return
                0.2126 * value[0]
                + 0.7152 * value[1]
                + 0.0722 * value[2];
        };

    passed &= test::near(
        luma(reduced),
        luma(channel_noise),
        1e-8,
        "grain chroma reduction preserves weighted luminance noise");

    // Integrated texture diagnostic: reference-aperture RMS, channel covariance,
    // subpixel area averaging and deterministic independent stages.
    const float ppm = 2048.0f / 24.89f;
    const auto texture = GranularityModel::texture(1.0f, ppm);
    double sum_r=0.0, sum_g=0.0, sum_rr=0.0, sum_gg=0.0, sum_rg=0.0;
    for (int y=0; y<256; ++y) {
        for (int x=0; x<256; ++x) {
            const float r=GranularityModel::spatial_sample(31u,x,y,0,0,texture);
            const float g=GranularityModel::spatial_sample(31u,x,y,0,1,texture);
            sum_r+=r; sum_g+=g; sum_rr+=r*r; sum_gg+=g*g; sum_rg+=r*g;
        }
    }
    const double count=256.0*256.0;
    const double mean_r=sum_r/count, mean_g=sum_g/count;
    const double var_r=sum_rr/count-mean_r*mean_r, var_g=sum_gg/count-mean_g*mean_g;
    const double correlation=(sum_rg/count-mean_r*mean_g)/std::sqrt(var_r*var_g);
    const double fine_rms=texture.fine_normalization*GranularityModel::aperture_energy(1.0f,0.85f);
    const double coarse_rms=texture.coarse_normalization*GranularityModel::aperture_energy(1.0f,1.80f);
    const double expected_variance=0.75*fine_rms*fine_rms+0.25*coarse_rms*coarse_rms;
    passed &= test::check(std::abs(mean_r)<0.1 && std::abs(mean_g)<0.1,"integrated texture is approximately zero mean");
    passed &= test::near(var_r/expected_variance,1.0,0.12,"pixel RMS follows aperture normalization");
    passed &= test::near(correlation,0.81,0.04,"channels share structure without becoming identical");
    const float fine=GranularityModel::spatial_sample(31u,17,23,0,0,texture);
    passed &= test::near(fine,GranularityModel::spatial_sample(31u,17,23,0,0,texture),0.0,"texture is deterministic");
    passed &= test::check(fine!=GranularityModel::spatial_sample(31u,17,23,1,0,texture),"negative and print textures remain distinct");
    passed &= test::check(GranularityModel::aperture_energy(2.0f,0.85f)<GranularityModel::aperture_energy(1.0f,0.85f),
        "larger pixel footprint averages grain down");
    passed &= test::near(GranularityModel::aperture_energy(1.0f,1.0f),0.55,1e-6,"unit integrated triangular basis energy");
    passed &= test::near(GranularityModel::aperture_energy(2.0f,1.0f),23.0/60.0,1e-6,"two-cell aperture energy");
    for (int stage = 0; stage < 2; ++stage) {
        const bool print = stage == 1;
        const double fine_norm = print ? texture.print_fine_normalization : texture.fine_normalization;
        const double coarse_norm = print ? texture.print_coarse_normalization : texture.coarse_normalization;
        const double fine_aperture = fine_norm * GranularityModel::aperture_energy(
            texture.aperture_pixels, print ? 0.50f : 0.85f);
        const double coarse_aperture = coarse_norm * GranularityModel::aperture_energy(
            texture.aperture_pixels, print ? 1.10f : 1.80f);
        passed &= test::near((print ? 0.90 : 0.75)*fine_aperture*fine_aperture
                            +(print ? 0.10 : 0.25)*coarse_aperture*coarse_aperture,
                            1.0, 1e-6, "both stage shapes preserve reference-aperture variance");
    }
    const auto high=GranularityModel::texture(4.0f,ppm*4.0f);
    const auto low=GranularityModel::texture(2.0f,ppm*2.0f);
    for (int stage=0;stage<2;++stage)
    for (int y=0;y<8;++y) for(int x=0;x<8;++x) {
        float average=0.0f;
        for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx)
            average+=0.25f*GranularityModel::spatial_sample(71u,2*x+dx,2*y+dy,stage,0,high);
        passed &= test::near(average,GranularityModel::spatial_sample(71u,x,y,stage,0,low),5e-5,
            "pixel integration commutes with a two-by-two area downsample");
    }
    // Independent numerical quadrature checks the analytic aperture normalization.
    const auto primitive=[](double t) {
        if(t<=-1.0) return 0.0;
        if(t<0.0) return 0.5*(t+1.0)*(t+1.0);
        if(t<1.0) return 1.0-0.5*(1.0-t)*(1.0-t);
        return 1.0;
    };
    for (double width : {0.2,0.7,1.0,1.5,2.0,4.0,8.0}) {
        double energy=0.0;
        for(int phase=0;phase<512;++phase) {
            const double center=(phase+0.5)/512.0;
            for(int node=-6;node<=6;++node) {
                const double weight=(primitive(center+width/2-node)-primitive(center-width/2-node))/width;
                energy+=weight*weight/512.0;
            }
        }
        passed &= test::near(GranularityModel::aperture_energy(static_cast<float>(width),1.0f),energy,2e-6,
            "analytic aperture energy agrees with integrated basis quadrature");
    }

    return
        test::finish(
            passed,
            "measured two-stage granularity model");
}
