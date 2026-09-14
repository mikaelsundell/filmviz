// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizdirectmetalprocessor.h"

#include "filmpipeline.h"
#include "inputtransform.h"

#include <OpenImageIO/imagebuf.h>
#import <Metal/Metal.h>

#include <algorithm>
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

} // namespace

int main(int argc,char** argv)
{
    if(argc<3) {
        std::cerr << "usage: filmviz_metal_compare <input> <output-prefix> [resources]\n";
        return 2;
    }
    const std::string input_filename=argv[1];
    const std::string output_prefix=argv[2];
    const std::string resources=argc>3?argv[3]:"resources";
    OIIO::ImageBuf input(input_filename);
    if(!input.read(0,0,true,OIIO::TypeDesc::FLOAT)||input.spec().nchannels<3) {
        std::cerr << "could not read RGB input: " << input.geterror() << '\n'; return 1;
    }
    const int width=input.spec().width,height=input.spec().height,channels=input.spec().nchannels;
    const std::size_t count=static_cast<std::size_t>(width)*height;
    std::vector<float> source_raw(count*static_cast<std::size_t>(channels));
    input.get_pixels(input.roi(),OIIO::TypeDesc::FLOAT,source_raw.data());
    std::vector<float> source(count*4u,1.0f),cpu(count*3u),metal(count*3u);
    for(std::size_t i=0;i<count;++i) for(int c=0;c<3;++c) source[i*4u+c]=source_raw[i*channels+c];

    FilmVizOfxRenderSettings settings;
    settings.input_profile=1;
    settings.output_profile=0;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue=[device newCommandQueue];
    if(!device||!queue) { std::cerr << "Metal is unavailable\n"; return 1; }
    id<MTLBuffer> source_buffer=[device newBufferWithBytes:source.data() length:source.size()*sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLBuffer> output_buffer=[device newBufferWithLength:source.size()*sizeof(float) options:MTLResourceStorageModeShared];
    FilmVizOfxMetalFrame source_frame,output_frame;
    source_frame.x2=output_frame.x2=width; source_frame.y2=output_frame.y2=height;
    source_frame.row_bytes=output_frame.row_bytes=static_cast<std::ptrdiff_t>(width*4*sizeof(float));
    source_frame.buffer=(__bridge void*)source_buffer; output_frame.buffer=(__bridge void*)output_buffer;
    FilmVizDirectMetalProcessor renderer;
    std::string error;
    if(!renderer.configure(settings,resources,(__bridge void*)queue,error)
        ||!renderer.render(settings,(__bridge void*)queue,source_frame,output_frame,0,0,width,height,0.0,error)) {
        std::cerr << error << '\n'; return 1;
    }
    id<MTLCommandBuffer> fence=[queue commandBuffer]; [fence commit]; [fence waitUntilCompleted];
    const float* gpu=static_cast<const float*>(output_buffer.contents);
    for(std::size_t i=0;i<count;++i) for(int c=0;c<3;++c) metal[i*3u+c]=gpu[i*4u+c];

    FilmPipeline::Settings pipeline_settings;
    pipeline_settings.resources_directory=resources;
    FilmPipeline pipeline;
    if(!pipeline.initialize(pipeline_settings)) { std::cerr << pipeline.error() << '\n'; return 1; }
    for(std::size_t i=0;i<count;++i) {
        const std::array<float,3> ap0={{source[i*4u],source[i*4u+1],source[i*4u+2]}};
        const auto result=pipeline.process(ap0);
        if(!result.valid) { std::cerr << "CPU processing failed at pixel " << i << '\n'; return 1; }
        for(int c=0;c<3;++c) cpu[i*3u+c]=result.ap0[c];
    }

    double squared=0.0; float maximum=0.0f;
    for(std::size_t i=0;i<cpu.size();++i) { float d=metal[i]-cpu[i]; squared+=double(d)*d; maximum=std::max(maximum,std::abs(d)); }
    const double rms=std::sqrt(squared/double(cpu.size()));
    std::vector<float> side(static_cast<std::size_t>(width*2)*height*3u);
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) for(int c=0;c<3;++c) {
        std::size_t input_index=(static_cast<std::size_t>(y)*width+x)*3u+c;
        side[(static_cast<std::size_t>(y)*(width*2)+x)*3u+c]=cpu[input_index];
        side[(static_cast<std::size_t>(y)*(width*2)+width+x)*3u+c]=metal[input_index];
    }
    const std::filesystem::path prefix(output_prefix);
    if(!prefix.parent_path().empty()) std::filesystem::create_directories(prefix.parent_path());
    if(!write_image(output_prefix+"_cpu.exr",width,height,cpu)
        ||!write_image(output_prefix+"_metal.exr",width,height,metal)
        ||!write_image(output_prefix+"_side_by_side.exr",width*2,height,side)) {
        std::cerr << "could not write comparison images\n"; return 1;
    }
    std::cout << "CPU | Metal Direct\nRMS AP0 error: " << rms << "\nMaximum AP0 error: " << maximum << '\n';
    return 0;
}
