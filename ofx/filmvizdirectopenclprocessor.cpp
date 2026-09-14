// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizdirectopenclprocessor.h"

#include "filmdirectdata.h"
#include "filmpipeline.h"
#include "filmvizdirectkernelsource.h"
#include "filmvizdirectparams.h"
#include "negativeprofile.h"
#include "printprofile.h"
#include "spatialresponsemodel.h"

#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

void replace_all(std::string& text,const std::string& from,const std::string& to)
{
    std::size_t position=0;
    while((position=text.find(from,position))!=std::string::npos) {
        text.replace(position,from.size(),to);
        position+=to.size();
    }
}

void replace_first(std::string& text,const std::string& from,const std::string& to)
{
    const std::size_t position=text.find(from);
    if(position!=std::string::npos) text.replace(position,from.size(),to);
}

std::string opencl_source()
{
    std::string source=kFilmVizDirectMetalKernelSource;
    replace_all(source,"#include <metal_stdlib>\nusing namespace metal;",
        "#pragma OPENCL EXTENSION cl_khr_fp64 : disable");
    replace_all(source,"device ","__global ");
    replace_all(source,"constant ","__constant ");
    replace_all(source,"thread ","__private ");
    replace_all(source,"__constant DirectParams& p","__constant DirectParams* p");
    replace_all(source,"p.","p->");
    replace_first(source,"struct DirectParams\n{","typedef struct DirectParams\n{");
    replace_first(source,"};","} DirectParams;");
    replace_first(source,"struct SpatialParams\n{","typedef struct SpatialParams\n{");
    replace_first(source,"};","} SpatialParams;");
    replace_all(source,"__constant SpatialParams& p","__constant SpatialParams* p");
    replace_all(source,"__constant SpatialParams& spatial","__constant SpatialParams* spatial");
    replace_all(source,"spatial.","spatial->");
    replace_all(source,"__private float3& x","__private float3* x");
    replace_all(source,"x[r]=M[r][3]/M[r][r]","(*x)[r]=M[r][3]/M[r][r]");
    replace_all(source,"solve3(A0,desired-p->calibration_zero_measured.xyz,delta)",
        "solve3(A0,desired-p->calibration_zero_measured.xyz,&delta)");
    replace_all(source,"solve3(normal,rhs,update)","solve3(normal,rhs,&update)");
    replace_all(source,"solve3(M,rhs,step)","solve3(M,rhs,&step)");
    replace_all(source,"abs(","fabs(");
    for(int i=0;i<16;++i) {
        replace_all(source," [[buffer("+std::to_string(i)+")]]","");
    }
    replace_all(source,", uint2 gid [[thread_position_in_grid]]","");
    replace_all(source,",\n    uint2 gid [[thread_position_in_grid]]","");
    replace_all(source,"kernel void filmviz_direct(","__kernel void filmviz_direct(");
    replace_all(source,"kernel void filmviz_prepare_halation(","__kernel void filmviz_prepare_halation(");
    replace_all(source,"kernel void filmviz_box_blur(","__kernel void filmviz_box_blur(");
    replace_all(source,"kernel void filmviz_combine_halation(","__kernel void filmviz_combine_halation(");
    replace_all(source,"kernel void filmviz_mtf(","__kernel void filmviz_mtf(");
    replace_all(source,"kernel void filmviz_copy_dense(","__kernel void filmviz_copy_dense(");
    replace_all(source,")\n{\n    int x=p->render_x1",
        ")\n{\n    uint2 gid=(uint2)(get_global_id(0),get_global_id(1));\n    int x=p->render_x1");
    replace_all(source,")\n{\n    if(gid.x",
        ")\n{\n    uint2 gid=(uint2)(get_global_id(0),get_global_id(1));\n    if(gid.x");
    replace_all(source,")\n{\n    uint width=uint(p->render_x2",
        ")\n{\n    uint2 gid=(uint2)(get_global_id(0),get_global_id(1));\n    uint width=(uint)(p->render_x2");
    replace_all(source,
        "__global const float4* row = reinterpret_cast<__global const float4*>(bytes + uint(y-y1)*row_bytes);",
        "__global const float4* row = (__global const float4*)(bytes + (uint)(y-y1)*row_bytes);");
    replace_all(source,
        "__global float4* row = reinterpret_cast<__global float4*>(bytes + uint(y-y1)*row_bytes);",
        "__global float4* row = (__global float4*)(bytes + (uint)(y-y1)*row_bytes);");
    replace_all(source,"as_type<float>(h)","as_float(h)");
    replace_all(source,"float2(","(float2)(");
    replace_all(source,"float3(","(float3)(");
    replace_all(source,"float4(","(float4)(");
    replace_all(source,"uint2(","(uint2)(");
    replace_all(source,"uint(","(uint)(");
    replace_all(source,"int(","(int)(");
    replace_all(source,"float(","(float)(");
    return source;
}

std::string opencl_error(const char* operation,cl_int code)
{
    return std::string(operation)+" failed with OpenCL error "+std::to_string(code);
}

struct OpenCLResources
{
    cl_context context=nullptr;
    cl_device_id device=nullptr;
    cl_program program=nullptr;
    std::array<cl_mem,12> buffers={{nullptr}};
    FilmDirectData data;
    SpatialResponseModel spatial_response;
    bool spatial_response_valid=false;

    ~OpenCLResources()
    {
        for(cl_mem buffer:buffers) if(buffer) clReleaseMemObject(buffer);
        if(program) clReleaseProgram(program);
        if(context) clReleaseContext(context);
    }
};

std::mutex gOpenCLCacheMutex;
std::unordered_map<std::string,std::weak_ptr<OpenCLResources>> gOpenCLCache;

std::string profile_key(
    cl_context context,
    cl_device_id device,
    const FilmVizOfxRenderSettings& settings,
    const std::string& resources)
{
    std::ostringstream stream;
    stream << reinterpret_cast<std::uintptr_t>(context) << ':'
        << reinterpret_cast<std::uintptr_t>(device) << ':'
        << resources << ':' << settings.negative_profile << ':' << settings.print_profile;
    return stream.str();
}

cl_mem make_buffer(cl_context context,const std::vector<float>& values,cl_int& status)
{
    if(values.empty()) { status=CL_INVALID_VALUE; return nullptr; }
    return clCreateBuffer(context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,
        values.size()*sizeof(float),const_cast<float*>(values.data()),&status);
}

void populate_params(
    FilmVizDirectParams& p,
    const FilmDirectData& data,
    const FilmVizOfxRenderSettings& settings,
    const FilmVizOfxOpenCLFrame& source,
    const FilmVizOfxOpenCLFrame& destination,
    int render_x1,int render_y1,int render_x2,int render_y2,double time)
{
    p.spectral_count=data.spectral_count; p.rgb2spec_resolution=data.rgb2spec_resolution;
    p.rgb2spec_forward_count=data.rgb2spec_forward_count;
    p.input_profile=settings.input_profile; p.output_profile=settings.output_profile;
    p.source_x1=source.x1; p.source_y1=source.y1; p.source_x2=source.x2; p.source_y2=source.y2;
    p.destination_x1=destination.x1; p.destination_y1=destination.y1;
    p.destination_x2=destination.x2; p.destination_y2=destination.y2;
    p.render_x1=render_x1; p.render_y1=render_y1; p.render_x2=render_x2; p.render_y2=render_y2;
    p.source_row_bytes=static_cast<std::uint32_t>(source.row_bytes);
    p.destination_row_bytes=static_cast<std::uint32_t>(destination.row_bytes);
    p.exposure_stops=settings.exposure_stops; p.negative_flash_percent=settings.negative_flash_percent;
    p.print_flash_percent=settings.print_flash_percent; p.push_pull_stops=settings.push_pull_stops;
    p.color_density=settings.color_density; p.color_depth=settings.color_depth;
    p.negative_bleach_bypass=settings.negative_bleach_bypass; p.print_bleach_bypass=settings.print_bleach_bypass;
    p.printer_light_red=settings.printer_light_red; p.printer_light_green=settings.printer_light_green;
    p.printer_light_blue=settings.printer_light_blue; p.printer_light_master=settings.printer_light_master;
    p.middle_gray=settings.middle_gray; p.printer_temperature=settings.printer_temperature;
    p.wavelength_min_nm=data.wavelength_min_nm; p.wavelength_step_nm=data.wavelength_step_nm;
    p.granularity_count=static_cast<std::uint32_t>(data.negative_granularity_samples.size()/4u);
    p.frame_seed=settings.grain_seed^static_cast<std::uint32_t>(std::llround(time*1000.0));
    p.grain_enabled=settings.grain_enabled?1u:0u; p.negative_grain=settings.negative_grain;
    p.print_grain=settings.print_grain; p.grain_size=settings.grain_size; p.grain_chroma=settings.grain_chroma;
    p.granularity_density_min=data.granularity_density_min; p.granularity_density_max=data.granularity_density_max;
    auto copy3=[](float* destination_values,const std::array<float,3>& source_values) {
        for(int i=0;i<3;++i) destination_values[i]=source_values[i];
    };
    copy3(p.reference_negative_exposure,data.reference_negative_exposure);
    copy3(p.reference_negative_density,data.reference_negative_density);
    copy3(p.minimum_negative_coordinate,data.minimum_negative_coordinate);
    copy3(p.neutral_negative_increment,data.neutral_negative_increment);
    copy3(p.calibration_zero_target,data.calibration_zero_target);
    copy3(p.calibration_zero_measured,data.calibration_zero_measured);
    copy3(p.calibration_minimum_status_m,data.calibration_minimum_status_m);
    copy3(p.print_target_log_exposure,data.print_target_log_exposure);
    for(int i=0;i<3;++i) {
        p.calibration_jacobian_0[i]=data.calibration_zero_jacobian[i];
        p.calibration_jacobian_1[i]=data.calibration_zero_jacobian[3+i];
        p.calibration_jacobian_2[i]=data.calibration_zero_jacobian[6+i];
    }
    const auto& negative=data.negative_characteristic;
    const auto& print=data.print_characteristic;
    p.curve_negative_01[0]=negative[0].offset; p.curve_negative_01[1]=negative[0].count;
    p.curve_negative_01[2]=negative[1].offset; p.curve_negative_01[3]=negative[1].count;
    p.curve_negative_2_print_0[0]=negative[2].offset; p.curve_negative_2_print_0[1]=negative[2].count;
    p.curve_negative_2_print_0[2]=print[0].offset; p.curve_negative_2_print_0[3]=print[0].count;
    p.curve_print_12[0]=print[1].offset; p.curve_print_12[1]=print[1].count;
    p.curve_print_12[2]=print[2].offset; p.curve_print_12[3]=print[2].count;
}

} // namespace

struct FilmVizDirectOpenCLProcessor::Impl
{
    std::shared_ptr<OpenCLResources> resources;
    std::string profile_key;
    std::mutex render_mutex;
};

FilmVizDirectOpenCLProcessor::FilmVizDirectOpenCLProcessor():impl_(new Impl) {}
FilmVizDirectOpenCLProcessor::~FilmVizDirectOpenCLProcessor()=default;

bool FilmVizDirectOpenCLProcessor::configure(
    const FilmVizOfxRenderSettings& settings,
    const std::string& resources_directory,
    void* command_queue,
    std::string& error)
{
    error.clear();
    cl_command_queue queue=reinterpret_cast<cl_command_queue>(command_queue);
    if(!queue) { error="OpenCL command queue is unavailable"; return false; }
    cl_context context=nullptr; cl_device_id device=nullptr;
    cl_int status=clGetCommandQueueInfo(queue,CL_QUEUE_CONTEXT,sizeof(context),&context,nullptr);
    if(status==CL_SUCCESS) status=clGetCommandQueueInfo(queue,CL_QUEUE_DEVICE,sizeof(device),&device,nullptr);
    if(status!=CL_SUCCESS||!context||!device) { error=opencl_error("clGetCommandQueueInfo",status); return false; }
    const std::string key=profile_key(context,device,settings,resources_directory);
    if(impl_->resources&&impl_->profile_key==key) return true;
    {
        std::lock_guard<std::mutex> lock(gOpenCLCacheMutex);
        auto found=gOpenCLCache.find(key);
        if(found!=gOpenCLCache.end()) {
            impl_->resources=found->second.lock();
            if(impl_->resources) { impl_->profile_key=key; return true; }
        }
    }
    FilmPipeline::Settings pipeline_settings;
    pipeline_settings.resources_directory=resources_directory;
    pipeline_settings.negative_profile=settings.negative_profile;
    pipeline_settings.print_profile=settings.print_profile;
    FilmPipeline pipeline;
    auto shared=std::make_shared<OpenCLResources>();
    if(!pipeline.initialize(pipeline_settings)||!pipeline.direct_data(shared->data)) {
        error=pipeline.error().empty()?"could not export FilmViz direct data":pipeline.error(); return false;
    }
    const auto* negative_profile=NegativeProfileCatalog::find(settings.negative_profile);
    const auto* print_profile=PrintProfileCatalog::find(settings.print_profile=="none"
        ?PrintProfileCatalog::default_profile().identifier:settings.print_profile);
    if(negative_profile&&print_profile) {
        const std::filesystem::path resources(resources_directory);
        shared->spatial_response_valid=shared->spatial_response.load(
            (resources/negative_profile->resource_directory/(negative_profile->resource_prefix+"_modulation_transfer_function_curves.csv")).string(),
            (resources/print_profile->resource_directory/print_profile->mtf_filename).string());
    }
    status=clRetainContext(context);
    if(status!=CL_SUCCESS) { error=opencl_error("clRetainContext",status); return false; }
    shared->context=context; shared->device=device;
    const std::string source=opencl_source(); const char* source_pointer=source.c_str();
    const std::size_t source_size=source.size();
    shared->program=clCreateProgramWithSource(context,1,&source_pointer,&source_size,&status);
    if(status==CL_SUCCESS) status=clBuildProgram(shared->program,1,&device,"-cl-std=CL1.2",nullptr,nullptr);
    if(status!=CL_SUCCESS) {
        std::size_t length=0; clGetProgramBuildInfo(shared->program,device,CL_PROGRAM_BUILD_LOG,0,nullptr,&length);
        std::string log(length,'\0');
        if(length) clGetProgramBuildInfo(shared->program,device,CL_PROGRAM_BUILD_LOG,length,log.data(),nullptr);
        error=opencl_error("clBuildProgram",status)+(log.empty()?"":"\n"+log); return false;
    }
    const std::array<const std::vector<float>*,12> values={{
        &shared->data.negative_exposure_samples,&shared->data.negative_density_samples,
        &shared->data.status_m_samples,&shared->data.print_exposure_samples,
        &shared->data.print_density_samples,&shared->data.viewer_ap0_samples,
        &shared->data.characteristic_points,&shared->data.rgb2spec_scale,
        &shared->data.rgb2spec_data,&shared->data.rgb2spec_forward,
        &shared->data.negative_granularity_samples,&shared->data.print_granularity_samples}};
    for(std::size_t i=0;i<values.size();++i) {
        shared->buffers[i]=make_buffer(context,*values[i],status);
        if(status!=CL_SUCCESS) { error=opencl_error("clCreateBuffer",status); return false; }
    }
    impl_->resources=shared; impl_->profile_key=key;
    { std::lock_guard<std::mutex> lock(gOpenCLCacheMutex); gOpenCLCache[key]=shared; }
    return true;
}

bool FilmVizDirectOpenCLProcessor::render(
    const FilmVizOfxRenderSettings& settings,
    void* command_queue,
    const FilmVizOfxOpenCLFrame& source,
    const FilmVizOfxOpenCLFrame& destination,
    int render_x1,int render_y1,int render_x2,int render_y2,double time,
    std::string& error)
{
    error.clear();
    if(!impl_->resources||!command_queue||!source.buffer||!destination.buffer) {
        error="FilmViz direct OpenCL processor is not configured"; return false;
    }
    render_x1=std::max(render_x1,destination.x1); render_y1=std::max(render_y1,destination.y1);
    render_x2=std::min(render_x2,destination.x2); render_y2=std::min(render_y2,destination.y2);
    if(render_x1>=render_x2||render_y1>=render_y2) return true;
    std::lock_guard<std::mutex> lock(impl_->render_mutex);
    cl_int status=CL_SUCCESS;
    cl_command_queue queue=reinterpret_cast<cl_command_queue>(command_queue);
    cl_context context=impl_->resources->context;
    FilmVizDirectParams params;
    populate_params(params,impl_->resources->data,settings,source,destination,
        render_x1,render_y1,render_x2,render_y2,time);
    cl_mem source_buffer=reinterpret_cast<cl_mem>(source.buffer);
    cl_mem destination_buffer=reinterpret_cast<cl_mem>(destination.buffer);
    const bool use_halation=settings.halation_enabled&&settings.halation_strength>0.0f&&settings.halation_radius>0.0f;
    const bool use_mtf=settings.negative_mtf_amount>0.0f||settings.print_mtf_amount>0.0f;
    if(use_mtf&&!impl_->resources->spatial_response_valid) { error="FilmViz measured MTF response is unavailable"; return false; }
    const std::size_t source_width=source.x2-source.x1,source_height=source.y2-source.y1;
    const std::size_t render_width=render_x2-render_x1,render_height=render_y2-render_y1;
    std::vector<cl_mem> temporary_buffers;
    std::vector<cl_kernel> kernels;
    auto make_temporary=[&](std::size_t bytes) {
        cl_mem value=clCreateBuffer(context,CL_MEM_READ_WRITE,bytes,nullptr,&status);
        if(value) temporary_buffers.push_back(value); return value;
    };
    auto make_constant=[&](const void* value,std::size_t bytes) {
        cl_mem result=clCreateBuffer(context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,bytes,const_cast<void*>(value),&status);
        if(result) temporary_buffers.push_back(result); return result;
    };
    auto make_kernel=[&](const char* name) {
        cl_kernel value=clCreateKernel(impl_->resources->program,name,&status);
        if(value) kernels.push_back(value); return value;
    };
    auto enqueue=[&](cl_kernel kernel,std::size_t width,std::size_t height) {
        const std::size_t global[2]={width,height};
        if(status==CL_SUCCESS) status=clEnqueueNDRangeKernel(queue,kernel,2,nullptr,global,nullptr,0,nullptr,nullptr);
    };
    cl_mem prepared=source_buffer,highlight=nullptr,near_a=nullptr,near_b=nullptr,far_a=nullptr,far_b=nullptr;
    FilmVizDirectSpatialParams spatial;
    if(use_halation) {
        const std::size_t bytes=source_width*source_height*sizeof(float)*4u;
        prepared=make_temporary(bytes); highlight=make_temporary(bytes); near_a=make_temporary(bytes);
        near_b=make_temporary(bytes); far_a=make_temporary(bytes); far_b=make_temporary(bytes);
        spatial.width=static_cast<std::uint32_t>(source_width); spatial.height=static_cast<std::uint32_t>(source_height);
        spatial.strength=settings.halation_strength; spatial.threshold=settings.halation_threshold;
        cl_mem params_buffer=make_constant(&params,sizeof(params)); cl_mem spatial_buffer=make_constant(&spatial,sizeof(spatial));
        cl_kernel prepare=make_kernel("filmviz_prepare_halation");
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,0,sizeof(source_buffer),&source_buffer);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,1,sizeof(prepared),&prepared);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,2,sizeof(highlight),&highlight);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,3,sizeof(cl_mem),&impl_->resources->buffers[0]);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,4,sizeof(cl_mem),&impl_->resources->buffers[7]);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,5,sizeof(cl_mem),&impl_->resources->buffers[8]);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,6,sizeof(cl_mem),&impl_->resources->buffers[9]);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,7,sizeof(params_buffer),&params_buffer);
        if(status==CL_SUCCESS) status=clSetKernelArg(prepare,8,sizeof(spatial_buffer),&spatial_buffer);
        enqueue(prepare,source_width,source_height);
        cl_kernel blur=make_kernel("filmviz_box_blur");
        auto blur_three=[&](cl_mem initial,cl_mem a,cl_mem b,int radius) {
            spatial.radius=static_cast<std::uint32_t>(radius);
            for(int pass=0;pass<3&&status==CL_SUCCESS;++pass) {
                cl_mem input=pass==0?initial:a; spatial.horizontal=1;
                cl_mem sb=make_constant(&spatial,sizeof(spatial));
                status=clSetKernelArg(blur,0,sizeof(input),&input); if(status==CL_SUCCESS) status=clSetKernelArg(blur,1,sizeof(b),&b);
                if(status==CL_SUCCESS) status=clSetKernelArg(blur,2,sizeof(sb),&sb); enqueue(blur,source_width,source_height);
                spatial.horizontal=0; sb=make_constant(&spatial,sizeof(spatial));
                if(status==CL_SUCCESS) status=clSetKernelArg(blur,0,sizeof(b),&b); if(status==CL_SUCCESS) status=clSetKernelArg(blur,1,sizeof(a),&a);
                if(status==CL_SUCCESS) status=clSetKernelArg(blur,2,sizeof(sb),&sb); enqueue(blur,source_width,source_height);
            }
        };
        blur_three(highlight,near_a,near_b,filmviz_halation_box_radius(settings.halation_radius));
        blur_three(highlight,far_a,far_b,filmviz_halation_box_radius(settings.halation_radius*2.2f));
        cl_kernel combine=make_kernel("filmviz_combine_halation"); spatial.horizontal=0;
        cl_mem combine_params=make_constant(&spatial,sizeof(spatial));
        if(status==CL_SUCCESS) status=clSetKernelArg(combine,0,sizeof(prepared),&prepared);
        if(status==CL_SUCCESS) status=clSetKernelArg(combine,1,sizeof(highlight),&highlight);
        if(status==CL_SUCCESS) status=clSetKernelArg(combine,2,sizeof(near_a),&near_a);
        if(status==CL_SUCCESS) status=clSetKernelArg(combine,3,sizeof(far_a),&far_a);
        if(status==CL_SUCCESS) status=clSetKernelArg(combine,4,sizeof(combine_params),&combine_params);
        enqueue(combine,source_width,source_height); params.reserved_header[0]=1u;
    }
    cl_mem dense_a=nullptr,dense_b=nullptr,direct_destination=destination_buffer;
    FilmVizDirectParams direct_params=params;
    if(use_mtf) {
        const std::size_t bytes=render_width*render_height*sizeof(float)*4u;
        dense_a=make_temporary(bytes); dense_b=make_temporary(bytes); direct_destination=dense_a;
        direct_params.destination_x1=render_x1; direct_params.destination_y1=render_y1;
        direct_params.destination_x2=render_x2; direct_params.destination_y2=render_y2;
        direct_params.destination_row_bytes=static_cast<std::uint32_t>(render_width*sizeof(float)*4u);
    }
    cl_mem direct_params_buffer=make_constant(&direct_params,sizeof(direct_params));
    cl_kernel direct=make_kernel("filmviz_direct");
    if(status==CL_SUCCESS) status=clSetKernelArg(direct,0,sizeof(source_buffer),&source_buffer);
    if(status==CL_SUCCESS) status=clSetKernelArg(direct,1,sizeof(direct_destination),&direct_destination);
    for(std::size_t i=0;status==CL_SUCCESS&&i<impl_->resources->buffers.size();++i)
        status=clSetKernelArg(direct,static_cast<cl_uint>(i+2),sizeof(cl_mem),&impl_->resources->buffers[i]);
    if(status==CL_SUCCESS) status=clSetKernelArg(direct,14,sizeof(direct_params_buffer),&direct_params_buffer);
    if(status==CL_SUCCESS) status=clSetKernelArg(direct,15,sizeof(prepared),&prepared);
    enqueue(direct,render_width,render_height);
    if(use_mtf&&status==CL_SUCCESS) {
        SpatialResponseModel::Settings s; s.image_width_mm=settings.image_width_mm; s.negative_amount=settings.negative_mtf_amount;
        s.print_amount=settings.print_mtf_amount; s.sampling_width_pixels=static_cast<int>(source_width); s.gamma24_encoded=settings.output_profile==1;
        std::array<std::vector<float>,3> channel_weights;
        if(!impl_->resources->spatial_response.kernels(static_cast<int>(render_width),s,channel_weights)) status=CL_INVALID_VALUE;
        std::vector<float> weights; for(const auto& channel:channel_weights) weights.insert(weights.end(),channel.begin(),channel.end());
        cl_mem weight_buffer=status==CL_SUCCESS?make_constant(weights.data(),weights.size()*sizeof(float)):nullptr;
        spatial.width=static_cast<std::uint32_t>(render_width); spatial.height=static_cast<std::uint32_t>(render_height);
        spatial.radius=channel_weights.empty()?0u:static_cast<std::uint32_t>(channel_weights[0].size()/2u); spatial.gamma24=s.gamma24_encoded?1u:0u;
        cl_kernel mtf=make_kernel("filmviz_mtf");
        auto run_mtf=[&](cl_mem input,cl_mem output,std::uint32_t horizontal) {
            spatial.horizontal=horizontal; cl_mem sb=make_constant(&spatial,sizeof(spatial));
            if(status==CL_SUCCESS) status=clSetKernelArg(mtf,0,sizeof(input),&input); if(status==CL_SUCCESS) status=clSetKernelArg(mtf,1,sizeof(output),&output);
            if(status==CL_SUCCESS) status=clSetKernelArg(mtf,2,sizeof(weight_buffer),&weight_buffer); if(status==CL_SUCCESS) status=clSetKernelArg(mtf,3,sizeof(sb),&sb);
            enqueue(mtf,render_width,render_height);
        };
        run_mtf(dense_a,dense_b,1); run_mtf(dense_b,dense_a,0);
        cl_mem output_params=make_constant(&params,sizeof(params)); cl_kernel copy=make_kernel("filmviz_copy_dense");
        if(status==CL_SUCCESS) status=clSetKernelArg(copy,0,sizeof(dense_a),&dense_a); if(status==CL_SUCCESS) status=clSetKernelArg(copy,1,sizeof(destination_buffer),&destination_buffer);
        if(status==CL_SUCCESS) status=clSetKernelArg(copy,2,sizeof(output_params),&output_params); enqueue(copy,render_width,render_height);
    }
    for(cl_kernel value:kernels) if(value) clReleaseKernel(value);
    for(cl_mem value:temporary_buffers) if(value) clReleaseMemObject(value);
    if(status!=CL_SUCCESS) { error=opencl_error("OpenCL spatial render",status); return false; }
    return true;
}

bool FilmVizDirectOpenCLProcessor::render_cpu_bridge(
    FilmVizOfxProcessor& cpu_processor,
    void* command_queue,
    const FilmVizOfxOpenCLFrame& source,
    const FilmVizOfxOpenCLFrame& destination,
    int render_x1,int render_y1,int render_x2,int render_y2,double time,
    const FilmVizOfxProcessor::Abort& abort,
    std::string& error)
{
    cl_command_queue queue=reinterpret_cast<cl_command_queue>(command_queue);
    cl_mem source_buffer=reinterpret_cast<cl_mem>(source.buffer);
    cl_mem destination_buffer=reinterpret_cast<cl_mem>(destination.buffer);
    std::size_t source_size=0,destination_size=0;
    cl_int status=clGetMemObjectInfo(source_buffer,CL_MEM_SIZE,sizeof(source_size),&source_size,nullptr);
    if(status==CL_SUCCESS) status=clGetMemObjectInfo(destination_buffer,CL_MEM_SIZE,sizeof(destination_size),&destination_size,nullptr);
    std::vector<unsigned char> source_bytes(source_size),destination_bytes(destination_size);
    if(status==CL_SUCCESS) status=clEnqueueReadBuffer(queue,source_buffer,CL_TRUE,0,source_size,source_bytes.data(),0,nullptr,nullptr);
    if(status==CL_SUCCESS) status=clEnqueueReadBuffer(queue,destination_buffer,CL_TRUE,0,destination_size,destination_bytes.data(),0,nullptr,nullptr);
    if(status!=CL_SUCCESS) { error=opencl_error("OpenCL CPU bridge download",status); return false; }
    FilmVizOfxFrame cpu_source;
    cpu_source.x1=source.x1; cpu_source.y1=source.y1; cpu_source.x2=source.x2; cpu_source.y2=source.y2;
    cpu_source.row_bytes=source.row_bytes; cpu_source.data=reinterpret_cast<float*>(source_bytes.data());
    FilmVizOfxFrame cpu_destination;
    cpu_destination.x1=destination.x1; cpu_destination.y1=destination.y1;
    cpu_destination.x2=destination.x2; cpu_destination.y2=destination.y2;
    cpu_destination.row_bytes=destination.row_bytes; cpu_destination.data=reinterpret_cast<float*>(destination_bytes.data());
    if(!cpu_processor.render(cpu_source,cpu_destination,render_x1,render_y1,render_x2,render_y2,time,abort,error)) return false;
    status=clEnqueueWriteBuffer(queue,destination_buffer,CL_TRUE,0,destination_size,destination_bytes.data(),0,nullptr,nullptr);
    if(status!=CL_SUCCESS) { error=opencl_error("OpenCL CPU bridge upload",status); return false; }
    return true;
}
