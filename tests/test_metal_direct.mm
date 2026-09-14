// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "ofx/filmvizdirectmetalprocessor.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main()
{
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    if(!device) {
        std::cout << "Metal unavailable; direct parity test skipped\n";
        return 0;
    }
    id<MTLCommandQueue> queue=[device newCommandQueue];
    const std::vector<std::array<float,3>> samples={
        {{0.0f,0.0f,0.0f}}, {{0.18f,0.18f,0.18f}}, {{1.0f,1.0f,1.0f}},
        {{0.18f,0.02f,0.01f}}, {{0.02f,0.18f,0.01f}}, {{0.01f,0.02f,0.18f}},
        {{0.7f,0.2f,0.05f}}, {{0.4f,0.05f,0.35f}}, {{0.02f,0.4f,0.35f}},
        {{2.0f,0.5f,0.1f}}, {{0.1f,1.5f,0.3f}}, {{0.2f,0.1f,2.0f}}
    };
    std::vector<float> source(samples.size()*4u,1.0f), destination(samples.size()*4u,0.0f);
    for(std::size_t i=0;i<samples.size();++i) for(int c=0;c<3;++c) source[i*4u+c]=samples[i][c];
    id<MTLBuffer> source_buffer=[device newBufferWithBytes:source.data() length:source.size()*sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLBuffer> destination_buffer=[device newBufferWithLength:destination.size()*sizeof(float) options:MTLResourceStorageModeShared];

    FilmVizOfxRenderSettings settings;
    settings.input_profile=1;
    settings.output_profile=0;
    settings.exposure_stops=0.35f;
    settings.negative_flash_percent=1.2f;
    settings.print_flash_percent=0.8f;
    settings.push_pull_stops=0.4f;
    settings.color_density=0.7f;
    settings.color_depth=1.3f;
    settings.negative_bleach_bypass=0.15f;
    settings.print_bleach_bypass=0.1f;
    settings.printer_light_red=26.0f;
    settings.printer_light_green=24.5f;
    settings.printer_light_blue=25.5f;
    settings.printer_temperature=3350.0f;

    FilmVizDirectMetalProcessor renderer;
    std::string error;
    if(!renderer.configure(settings,FILMVIZ_TEST_RESOURCE_DIR,(__bridge void*)queue,error)) {
        std::cerr << error << '\n'; return 1;
    }
    FilmVizOfxMetalFrame input,output;
    input.x2=output.x2=static_cast<int>(samples.size()); input.y2=output.y2=1;
    input.row_bytes=output.row_bytes=static_cast<std::ptrdiff_t>(samples.size()*4u*sizeof(float));
    input.buffer=(__bridge void*)source_buffer; output.buffer=(__bridge void*)destination_buffer;
    if(!renderer.render(settings,(__bridge void*)queue,input,output,0,0,input.x2,1,0.0,error)) {
        std::cerr << error << '\n'; return 1;
    }
    id<MTLCommandBuffer> fence=[queue commandBuffer]; [fence commit]; [fence waitUntilCompleted];
    std::copy_n(static_cast<const float*>(destination_buffer.contents),destination.size(),destination.data());

    FilmPipeline::Settings cpu_settings;
    cpu_settings.resources_directory=FILMVIZ_TEST_RESOURCE_DIR;
    cpu_settings.exposure_stops=settings.exposure_stops;
    cpu_settings.negative_flash_percent=settings.negative_flash_percent;
    cpu_settings.print_flash_percent=settings.print_flash_percent;
    cpu_settings.push_pull_stops=settings.push_pull_stops;
    cpu_settings.color_density=settings.color_density;
    cpu_settings.color_depth=settings.color_depth;
    cpu_settings.negative_bleach_bypass=settings.negative_bleach_bypass;
    cpu_settings.print_bleach_bypass=settings.print_bleach_bypass;
    cpu_settings.printer_light_red=settings.printer_light_red;
    cpu_settings.printer_light_green=settings.printer_light_green;
    cpu_settings.printer_light_blue=settings.printer_light_blue;
    cpu_settings.printer_temperature_kelvin=settings.printer_temperature;
    FilmPipeline reference;
    if(!reference.initialize(cpu_settings)) { std::cerr << reference.error() << '\n'; return 1; }
    float maximum_error=0.0f;
    for(std::size_t i=0;i<samples.size();++i) {
        const auto expected=reference.process(samples[i]);
        if(!expected.valid) return 1;
        float sample_error=0.0f;
        for(int c=0;c<3;++c) {
            const float difference=std::abs(destination[i*4u+c]-expected.ap0[c]);
            sample_error=std::max(sample_error,difference);
            maximum_error=std::max(maximum_error,difference);
        }
        if(sample_error>0.01f) {
            std::cout << i << " input=" << samples[i][0] << ',' << samples[i][1] << ',' << samples[i][2]
                << " cpu=" << expected.ap0[0] << ',' << expected.ap0[1] << ',' << expected.ap0[2]
                << " metal=" << destination[i*4u] << ',' << destination[i*4u+1] << ',' << destination[i*4u+2]
                << " error=" << sample_error << '\n';
        }
    }
    const std::vector<float> pointwise=destination;
    settings.halation_enabled=true; settings.halation_strength=0.5f;
    settings.halation_radius=4.0f; settings.halation_threshold=0.4f;
    settings.negative_mtf_amount=1.0f; settings.print_mtf_amount=1.0f;
    if(!renderer.render(settings,(__bridge void*)queue,input,output,0,0,input.x2,1,0.0,error)) {
        std::cerr << error << '\n'; return 1;
    }
    fence=[queue commandBuffer]; [fence commit]; [fence waitUntilCompleted];
    std::copy_n(static_cast<const float*>(destination_buffer.contents),destination.size(),destination.data());
    float spatial_change=0.0f;
    for(std::size_t i=0;i<destination.size();++i) {
        if(!std::isfinite(destination[i])) return 1;
        spatial_change=std::max(spatial_change,std::abs(destination[i]-pointwise[i]));
    }
    std::cout << "Direct Metal maximum AP0 error: " << maximum_error << '\n';
    return maximum_error<=0.015f&&spatial_change>1e-5f ? 0 : 1;
}
