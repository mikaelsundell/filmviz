// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmpipeline.h"
#include "ofx/filmvizdirectopenclprocessor.h"

#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main()
{
    cl_uint platform_count=0;
    if(clGetPlatformIDs(0,nullptr,&platform_count)!=CL_SUCCESS||platform_count==0) {
        std::cout << "OpenCL unavailable; direct parity test skipped\n"; return 0;
    }
    std::vector<cl_platform_id> platforms(platform_count);
    clGetPlatformIDs(platform_count,platforms.data(),nullptr);
    cl_device_id device=nullptr;
    for(cl_platform_id platform:platforms) {
        if(clGetDeviceIDs(platform,CL_DEVICE_TYPE_GPU,1,&device,nullptr)==CL_SUCCESS) break;
    }
    if(!device) for(cl_platform_id platform:platforms) {
        if(clGetDeviceIDs(platform,CL_DEVICE_TYPE_DEFAULT,1,&device,nullptr)==CL_SUCCESS) break;
    }
    if(!device) { std::cout << "OpenCL device unavailable; direct parity test skipped\n"; return 0; }
    cl_int status=CL_SUCCESS;
    cl_context context=clCreateContext(nullptr,1,&device,nullptr,nullptr,&status);
    cl_command_queue queue=status==CL_SUCCESS?clCreateCommandQueue(context,device,0,&status):nullptr;
    if(status!=CL_SUCCESS||!context||!queue) { std::cerr << "could not create OpenCL test context\n"; return 1; }

    const std::vector<std::array<float,3>> samples={
        {{0.0f,0.0f,0.0f}}, {{0.18f,0.18f,0.18f}}, {{1.0f,1.0f,1.0f}},
        {{0.18f,0.02f,0.01f}}, {{0.02f,0.18f,0.01f}}, {{0.01f,0.02f,0.18f}},
        {{0.7f,0.2f,0.05f}}, {{0.4f,0.05f,0.35f}}, {{0.02f,0.4f,0.35f}},
        {{2.0f,0.5f,0.1f}}, {{0.1f,1.5f,0.3f}}, {{0.2f,0.1f,2.0f}}};
    std::vector<float> source(samples.size()*4u,1.0f),destination(samples.size()*4u,0.0f);
    for(std::size_t i=0;i<samples.size();++i) for(int c=0;c<3;++c) source[i*4u+c]=samples[i][c];
    cl_mem source_buffer=clCreateBuffer(context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,
        source.size()*sizeof(float),source.data(),&status);
    cl_mem destination_buffer=clCreateBuffer(context,CL_MEM_WRITE_ONLY,
        destination.size()*sizeof(float),nullptr,&status);

    FilmVizOfxRenderSettings settings;
    settings.input_profile=1; settings.output_profile=0;
    settings.exposure_stops=0.35f; settings.negative_flash_percent=1.2f;
    settings.print_flash_percent=0.8f; settings.push_pull_stops=0.4f;
    settings.color_density=0.7f; settings.color_depth=1.3f;
    settings.negative_bleach_bypass=0.15f; settings.print_bleach_bypass=0.1f;
    settings.printer_light_red=26.0f; settings.printer_light_green=24.5f;
    settings.printer_light_blue=25.5f; settings.printer_temperature=3350.0f;
    FilmVizDirectOpenCLProcessor renderer; std::string error;
    FilmVizOfxOpenCLFrame input,output;
    input.x2=output.x2=static_cast<int>(samples.size()); input.y2=output.y2=1;
    input.row_bytes=output.row_bytes=static_cast<std::ptrdiff_t>(samples.size()*4u*sizeof(float));
    input.buffer=source_buffer; output.buffer=destination_buffer;
    bool rendered=renderer.configure(settings,FILMVIZ_TEST_RESOURCE_DIR,queue,error)
        &&renderer.render(settings,queue,input,output,0,0,input.x2,1,0.0,error);
    if(rendered) status=clEnqueueReadBuffer(queue,destination_buffer,CL_TRUE,0,
        destination.size()*sizeof(float),destination.data(),0,nullptr,nullptr);
    if(!rendered||status!=CL_SUCCESS) {
        std::cerr << (error.empty()?"OpenCL readback failed":error) << '\n'; return 1;
    }

    FilmPipeline::Settings cpu_settings;
    cpu_settings.resources_directory=FILMVIZ_TEST_RESOURCE_DIR;
    cpu_settings.exposure_stops=settings.exposure_stops;
    cpu_settings.negative_flash_percent=settings.negative_flash_percent;
    cpu_settings.print_flash_percent=settings.print_flash_percent;
    cpu_settings.push_pull_stops=settings.push_pull_stops;
    cpu_settings.color_density=settings.color_density; cpu_settings.color_depth=settings.color_depth;
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
        for(int c=0;c<3;++c) maximum_error=std::max(maximum_error,
            std::abs(destination[i*4u+c]-expected.ap0[c]));
    }
    const std::vector<float> pointwise=destination;
    settings.halation_enabled=true; settings.halation_strength=0.5f;
    settings.halation_radius=4.0f; settings.halation_threshold=0.4f;
    settings.negative_mtf_amount=1.0f; settings.print_mtf_amount=1.0f;
    rendered=renderer.render(settings,queue,input,output,0,0,input.x2,1,0.0,error);
    if(rendered) status=clEnqueueReadBuffer(queue,destination_buffer,CL_TRUE,0,
        destination.size()*sizeof(float),destination.data(),0,nullptr,nullptr);
    float spatial_change=0.0f;
    for(std::size_t i=0;rendered&&status==CL_SUCCESS&&i<destination.size();++i) {
        if(!std::isfinite(destination[i])) rendered=false;
        spatial_change=std::max(spatial_change,std::abs(destination[i]-pointwise[i]));
    }
    clReleaseMemObject(destination_buffer); clReleaseMemObject(source_buffer);
    clReleaseCommandQueue(queue); clReleaseContext(context);
    std::cout << "Direct OpenCL maximum AP0 error: " << maximum_error << '\n';
    return maximum_error<=0.015f&&rendered&&status==CL_SUCCESS&&spatial_change>1e-5f?0:1;
}
