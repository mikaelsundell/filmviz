// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "imageprocessor.h"
#include "spatialresponsemodel.h"
#include "test_common.h"

#include <cmath>
#include <string>
#include <vector>

int main()
{
    FilmPipeline pipeline;
    FilmPipeline::Settings settings;
    settings.resources_directory = FILMVIZ_TEST_RESOURCE_DIR;
    bool passed = test::check(pipeline.initialize(settings), "grain diagnostic pipeline initializes");
    if (!passed) return test::finish(false, "density grain response");
    const auto base = pipeline.process({{0.18f,0.18f,0.18f}});
    const auto unchanged = pipeline.process_density_noise(base, FilmDensity(), FilmDensity());
    passed &= test::check(base.valid && unchanged.valid, "zero density perturbation is valid");
    for (int c=0;c<3;++c)
        passed &= test::near(base.ap0[c],unchanged.ap0[c],0.0,"zero perturbation preserves baseline exactly");

    const auto negative = pipeline.process_density_noise(base, {0.001f,0.001f,0.001f}, FilmDensity());
    const auto print = pipeline.process_density_noise(base, FilmDensity(), {0.001f,0.001f,0.001f});
    passed &= test::check(negative.valid && print.valid, "both density boundaries accept perturbations");
    const auto sum=[](const std::array<float,3>& rgb) { return rgb[0]+rgb[1]+rgb[2]; };
    passed &= test::check(sum(negative.ap0)>sum(base.ap0), "more negative density increases viewed print light");
    passed &= test::check(sum(print.ap0)<sum(base.ap0), "more print density reduces viewed print light");
    passed &= test::check(negative.print_density.red != base.print_density.red,
        "negative noise propagates through print development");

    FilmPipeline::GrainResponse response;
    passed &= test::check(pipeline.grain_response(base,false,response), "downstream Jacobians are finite");
    const auto texture=GranularityModel::texture(1.0f,2048.0f/24.89f);
    const auto off=ImageProcessor::grain_residuals(response,base.ap0,texture,1u,10,12,0.0f,0.0f);
    for(float value:off) passed &= test::near(value,0.0,0.0,"disabled grain leaves zero residual");
    // Sample independent seeds at a fixed lattice phase. Check ensemble DC,
    // not the mean of one correlated image patch. Exercise both the ordinary
    // exponential and the bounded-gain tails, with unequal signed responses.
    for (int stage = 0; stage < 2; ++stage) {
    for (float amplitude : {0.15f, 0.8f, 4.0f}) {
        FilmPipeline::GrainResponse synthetic = {};
        synthetic[6+stage*9] = amplitude;
        synthetic[7+stage*9] = -0.35f * amplitude;
        synthetic[8+stage*9] = 0.6f * amplitude;
        double mean = 0.0;
        constexpr int samples = 32768;
        for (int seed = 1; seed <= samples; ++seed) {
            const auto delta = ImageProcessor::grain_residuals(synthetic, {{1.0f,1.0f,1.0f}},
                texture, static_cast<std::uint32_t>(seed), 10, 12, stage == 0 ? 1.0f : 0.0f,
                stage == 1 ? 1.0f : 0.0f);
            mean += delta[stage*3] / samples;
            passed &= test::check(std::isfinite(delta[stage*3]) && delta[stage*3] > -1.0f,
                "mean-normalized individual stage multiplier is finite and positive");
        }
        passed &= test::near(mean, 0.0, 0.025, "grain preserves ensemble linear-light mean before clipping");
    }
    }
    const std::array<float,3> rgb={{0.2f,0.4f,0.6f}}, residual={{0.01f,-0.02f,0.03f}};
    passed &= test::near(ImageProcessor::grain_visibility(rgb,false,0.0f,0.0f,0.0f),
        1.0, 0.0, "tonal bypass ignores all artistic multipliers");
    passed &= test::near(ImageProcessor::grain_visibility(rgb,true,0.0f,0.0f,0.0f),
        0.0, 0.0, "zero tonal gains suppress grain");
    const auto bypass = ImageProcessor::composite_grain(rgb,residual,1.0f,false);
    for (int c=0;c<3;++c)
        passed &= test::near(bypass[c],rgb[c]+residual[c],1e-6,
            "tonal bypass restores the unattenuated residual");
    passed &= test::near(ImageProcessor::grain_visibility({{0.05f,0.05f,0.05f}}),
        0.80, 1e-6, "visibility trim retains shadow grain at reduced strength");
    passed &= test::near(ImageProcessor::grain_visibility({{0.8f,0.8f,0.8f}}),
        0.35, 1e-6, "visibility trim suppresses bright grain smoothly");
    const auto positive = ImageProcessor::composite_grain(rgb,residual,1.0f);
    const auto negative_delta = std::array<float,3>{{-0.01f,0.02f,-0.03f}};
    const auto negative_composite = ImageProcessor::composite_grain(rgb,negative_delta,1.0f);
    for (int c=0;c<3;++c)
        passed &= test::near(0.5f*(positive[c]+negative_composite[c]),rgb[c],1e-6,
            "noise-free visibility trim preserves symmetric residual mean");
    const auto neutral=ImageProcessor::composite_grain(rgb,residual,0.0f);
    passed &= test::near(neutral[0]/rgb[0],neutral[1]/rgb[1],1e-6,"neutral grain preserves colour ratios");
    passed &= test::near(neutral[1]/rgb[1],neutral[2]/rgb[2],1e-6,"neutral grain preserves all colour ratios");

    SpatialResponseModel mtf;
    const std::string root=FILMVIZ_TEST_RESOURCE_DIR;
    passed &= test::check(mtf.load(root+"/profiles/verita_200d/kodak_verita_200d_modulation_transfer_function_curves.csv",
        root+"/profiles/kodak_2383/kodak_2383_modulation_transfer_function_curves.csv"),"residual MTF loads");
    SpatialResponseModel::Settings spatial;
    spatial.print_amount=1.0f; spatial.clamp_output=false;
    std::vector<float> signed_field(32*32*3,-0.02f);
    passed &= test::check(mtf.apply(signed_field,32,32,spatial),"signed residual filtering succeeds");
    for(float value:signed_field)
        passed &= test::near(value,-0.02,1e-6,"MTF preserves negative residual DC without clipping");
    return test::finish(passed,"density grain response");
}
