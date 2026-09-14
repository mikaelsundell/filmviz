// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizdirectopenclprocessor.h"

#include "filmpipeline.h"

#include <OpenImageIO/imagebuf.h>

#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

bool write_image(const std::string& filename,int width,int height,const std::vector<float>& pixels)
{
    OIIO::ImageSpec spec(width,height,3,OIIO::TypeDesc::FLOAT);
    spec.channelnames={"R","G","B"};
    OIIO::ImageBuf image(spec);
    image.set_pixels(OIIO::ROI::All(),OIIO::TypeDesc::FLOAT,pixels.data());
    return image.write(filename);
}

cl_device_id find_device()
{
    cl_uint count=0;
    if(clGetPlatformIDs(0,nullptr,&count)!=CL_SUCCESS||count==0) return nullptr;
    std::vector<cl_platform_id> platforms(count);
    clGetPlatformIDs(count,platforms.data(),nullptr);
    cl_device_id device=nullptr;
    for(cl_platform_id platform:platforms)
        if(clGetDeviceIDs(platform,CL_DEVICE_TYPE_GPU,1,&device,nullptr)==CL_SUCCESS) return device;
    for(cl_platform_id platform:platforms)
        if(clGetDeviceIDs(platform,CL_DEVICE_TYPE_DEFAULT,1,&device,nullptr)==CL_SUCCESS) return device;
    return nullptr;
}

} // namespace

int main(int argc,char** argv)
{
    if(argc<3) {
        std::cerr << "usage: filmviz_opencl_compare <input> <output-prefix> [resources] [--spatial]\n"; return 2;
    }
    const std::string input_filename=argv[1],output_prefix=argv[2];
    const std::string resources=argc>3?argv[3]:"resources";
    const bool spatial=argc>4&&std::string(argv[4])=="--spatial";
    OIIO::ImageBuf input(input_filename);
    if(!input.read(0,0,true,OIIO::TypeDesc::FLOAT)||input.spec().nchannels<3) {
        std::cerr << "could not read RGB input: " << input.geterror() << '\n'; return 1;
    }
    const int width=input.spec().width,height=input.spec().height,channels=input.spec().nchannels;
    const std::size_t count=static_cast<std::size_t>(width)*height;
    std::vector<float> source_raw(count*static_cast<std::size_t>(channels));
    input.get_pixels(input.roi(),OIIO::TypeDesc::FLOAT,source_raw.data());
    std::vector<float> source(count*4u,1.0f),cpu(count*3u),opencl(count*3u),gpu_rgba(count*4u);
    for(std::size_t i=0;i<count;++i) for(int c=0;c<3;++c) source[i*4u+c]=source_raw[i*channels+c];

    cl_device_id device=find_device(); cl_int status=CL_SUCCESS;
    cl_context context=device?clCreateContext(nullptr,1,&device,nullptr,nullptr,&status):nullptr;
    cl_command_queue queue=status==CL_SUCCESS&&context?clCreateCommandQueue(context,device,0,&status):nullptr;
    if(status!=CL_SUCCESS||!queue) { std::cerr << "OpenCL GPU is unavailable\n"; return 1; }
    cl_mem source_buffer=clCreateBuffer(context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,
        source.size()*sizeof(float),source.data(),&status);
    cl_mem output_buffer=clCreateBuffer(context,CL_MEM_WRITE_ONLY,
        gpu_rgba.size()*sizeof(float),nullptr,&status);

    FilmVizOfxRenderSettings settings; settings.input_profile=1; settings.output_profile=0;
    if(spatial) {
        settings.halation_enabled=true; settings.halation_strength=0.5f;
        settings.halation_radius=12.0f; settings.halation_threshold=0.7f;
        settings.negative_mtf_amount=1.0f; settings.print_mtf_amount=1.0f;
    }
    FilmVizOfxOpenCLFrame source_frame,output_frame;
    source_frame.x2=output_frame.x2=width; source_frame.y2=output_frame.y2=height;
    source_frame.row_bytes=output_frame.row_bytes=static_cast<std::ptrdiff_t>(width*4*sizeof(float));
    source_frame.buffer=source_buffer; output_frame.buffer=output_buffer;
    FilmVizDirectOpenCLProcessor renderer; std::string error;
    if(!renderer.configure(settings,resources,queue,error)
        ||!renderer.render(settings,queue,source_frame,output_frame,0,0,width,height,0.0,error)) {
        std::cerr << error << '\n'; return 1;
    }
    status=clEnqueueReadBuffer(queue,output_buffer,CL_TRUE,0,
        gpu_rgba.size()*sizeof(float),gpu_rgba.data(),0,nullptr,nullptr);
    if(status!=CL_SUCCESS) { std::cerr << "OpenCL readback failed\n"; return 1; }
    for(std::size_t i=0;i<count;++i) for(int c=0;c<3;++c) opencl[i*3u+c]=gpu_rgba[i*4u+c];

    if(spatial) {
        FilmVizOfxProcessor reference; std::vector<float> cpu_rgba(count*4u,0.0f);
        FilmVizOfxFrame cpu_source,cpu_output; cpu_source.x2=cpu_output.x2=width; cpu_source.y2=cpu_output.y2=height;
        cpu_source.row_bytes=cpu_output.row_bytes=static_cast<std::ptrdiff_t>(width*4*sizeof(float));
        cpu_source.data=source.data(); cpu_output.data=cpu_rgba.data();
        if(!reference.configure(settings,resources,error)||!reference.render(cpu_source,cpu_output,0,0,width,height,0.0,{},error)) { std::cerr << error << '\n'; return 1; }
        for(std::size_t i=0;i<count;++i) for(int c=0;c<3;++c) cpu[i*3u+c]=cpu_rgba[i*4u+c];
    } else {
        FilmPipeline::Settings pipeline_settings; pipeline_settings.resources_directory=resources;
        FilmPipeline pipeline;
        if(!pipeline.initialize(pipeline_settings)) { std::cerr << pipeline.error() << '\n'; return 1; }
        for(std::size_t i=0;i<count;++i) {
            const auto result=pipeline.process({{source[i*4u],source[i*4u+1],source[i*4u+2]}});
            if(!result.valid) { std::cerr << "CPU processing failed at pixel " << i << '\n'; return 1; }
            for(int c=0;c<3;++c) cpu[i*3u+c]=result.ap0[c];
        }
    }
    double squared=0.0; float maximum=0.0f;
    for(std::size_t i=0;i<cpu.size();++i) {
        const float difference=opencl[i]-cpu[i];
        squared+=static_cast<double>(difference)*difference;
        maximum=std::max(maximum,std::abs(difference));
    }
    std::vector<float> side(static_cast<std::size_t>(width*2)*height*3u);
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) for(int c=0;c<3;++c) {
        const std::size_t input_index=(static_cast<std::size_t>(y)*width+x)*3u+c;
        side[(static_cast<std::size_t>(y)*(width*2)+x)*3u+c]=cpu[input_index];
        side[(static_cast<std::size_t>(y)*(width*2)+width+x)*3u+c]=opencl[input_index];
    }
    const std::filesystem::path prefix(output_prefix);
    if(!prefix.parent_path().empty()) std::filesystem::create_directories(prefix.parent_path());
    const bool written=write_image(output_prefix+"_cpu.exr",width,height,cpu)
        &&write_image(output_prefix+"_opencl.exr",width,height,opencl)
        &&write_image(output_prefix+"_side_by_side.exr",width*2,height,side);
    clReleaseMemObject(output_buffer); clReleaseMemObject(source_buffer);
    clReleaseCommandQueue(queue); clReleaseContext(context);
    if(!written) { std::cerr << "could not write comparison images\n"; return 1; }
    std::cout << "CPU | OpenCL Direct" << (spatial?" (MTF + halation)":"") << "\nRMS AP0 error: " << std::sqrt(squared/cpu.size())
        << "\nMaximum AP0 error: " << maximum << '\n';
    return 0;
}
