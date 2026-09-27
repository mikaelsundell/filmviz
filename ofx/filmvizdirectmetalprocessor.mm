// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "outputtransform.h"
#include "inputtransform.h"
#include <exception>

#include "filmvizdirectmetalprocessor.h"

#include "filmvizdirectparams.h"

#include "filmdirectdata.h"
#include "filmpipeline.h"
#include "granularitymodel.h"
#include "negativeprofile.h"
#include "printprofile.h"
#include "spatialresponsemodel.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>

namespace
{

static const char* kDirectMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct DirectParams
{
    uint spectral_count;
    uint rgb2spec_resolution;
    uint rgb2spec_forward_count;
    uint input_profile;
    uint output_profile;
    uint reserved_header0;
    uint reserved_header1;
    uint reserved_header2;
    int source_x1; int source_y1; int source_x2; int source_y2;
    int destination_x1; int destination_y1; int destination_x2; int destination_y2;
    int render_x1; int render_y1; int render_x2; int render_y2;
    uint source_row_bytes; uint destination_row_bytes; uint reserved0; uint reserved1;
    float exposure_stops; float negative_flash_percent; float print_flash_percent; float push_pull_stops;
    float color_density; float color_depth; float negative_bleach_bypass; float print_bleach_bypass;
    float printer_light_red; float printer_light_green; float printer_light_blue; float printer_light_master;
    float middle_gray; float printer_temperature; float wavelength_min_nm; float wavelength_step_nm;
    uint granularity_count; uint frame_seed; uint grain_enabled; uint reserved_grain;
    float negative_grain; float print_grain; float grain_size; float grain_chroma;
    uint grain_tonal_enabled; float grain_shadows; float grain_midtones; float grain_highlights;
    float granularity_density_min; float granularity_density_max; float2 reserved_grain_float;
    float response_response_amount;
    float response_chroma_compression;
    float response_chroma_knee;
    float response_density_center;
    float response_density_width;
    float response_warm_protection;
    float response_warm_hue_center;
    float response_warm_hue_width;
    float response_warm_hue_shift;
    float reserved_response[3];
    float4 reference_negative_exposure;
    float4 reference_negative_density;
    float4 minimum_negative_coordinate;
    float4 neutral_negative_increment;
    float4 calibration_zero_target;
    float4 calibration_zero_measured;
    float4 calibration_minimum_status_m;
    float4 print_target_log_exposure;
    float4 calibration_jacobian_0;
    float4 calibration_jacobian_1;
    float4 calibration_jacobian_2;
    uint4 curve_negative_01;
    uint4 curve_negative_2_print_0;
    uint4 curve_print_12;
};

struct SpatialParams
{
    uint width; uint height; uint radius; uint horizontal;
    uint gamma24; uint reserved0; uint reserved1; uint reserved2;
    float strength; float threshold; float2 reserved_float;
    float4 scatter;
};

inline float4 read_pixel(device const uchar* bytes, uint row_bytes, int x, int y, int x1, int y1)
{
    device const float4* row = reinterpret_cast<device const float4*>(bytes + uint(y-y1)*row_bytes);
    return row[x-x1];
}

inline void write_pixel(device uchar* bytes, uint row_bytes, int x, int y, int x1, int y1, float4 value)
{
    device float4* row = reinterpret_cast<device float4*>(bytes + uint(y-y1)*row_bytes);
    row[x-x1] = value;
}

inline uint find_interval(device const float* scale, uint size, float value)
{
    uint left=0, count=size-2;
    while (count>0) {
        uint step=count>>1, middle=left+step+1;
        if (scale[middle]<=value) { left=middle; count-=step+1; }
        else count=step;
    }
    return min(left,size-2);
}

inline float3 rgb2spec_fetch(device const float* scale, device const float* data, uint res, float3 input)
{
    float3 rgb=clamp(input,0.0f,1.0f);
    if (rgb.x==rgb.y && rgb.y==rgb.z) {
        float v=rgb.x;
        float r=v<=0.0f?-8192.0f:(v>=1.0f?8192.0f:(v-0.5f)/sqrt(v*(1.0f-v)));
        return float3(0.0f,0.0f,r);
    }
    uint dominant=0;
    if (rgb.y>=rgb[dominant]) dominant=1;
    if (rgb.z>=rgb[dominant]) dominant=2;
    float z=rgb[dominant];
    if (z<=0.0f) return float3(data[0],data[1],data[2]);
    float grid=float(res-1)/z;
    float x=rgb[(dominant+1)%3]*grid, y=rgb[(dominant+2)%3]*grid;
    uint xi=min(uint(x),res-2), yi=min(uint(y),res-2), zi=find_interval(scale,res,z);
    uint offset=(((dominant*res+zi)*res+yi)*res+xi)*3;
    uint dx=3, dy=3*res, dz=3*res*res;
    float x1=x-float(xi), y1=y-float(yi);
    float z1=(z-scale[zi])/(scale[zi+1]-scale[zi]);
    float3 result;
    for (uint j=0;j<3;++j) {
        result[j]=((data[offset]* (1-x1)+data[offset+dx]*x1)*(1-y1)+(data[offset+dy]*(1-x1)+data[offset+dy+dx]*x1)*y1)*(1-z1)
            +((data[offset+dz]*(1-x1)+data[offset+dz+dx]*x1)*(1-y1)+(data[offset+dz+dy]*(1-x1)+data[offset+dz+dy+dx]*x1)*y1)*z1;
        ++offset;
    }
    return result;
}

inline float spectrum(float3 coeff, float wavelength)
{
    float v=(coeff.x*wavelength+coeff.y)*wavelength+coeff.z;
    return 0.5f*v*rsqrt(v*v+1.0f)+0.5f;
}

inline float curve_sample(device const float2* points, uint offset, uint count, float x)
{
    if (count==0) return 0.0f;
    if (x<=points[offset].x) return points[offset].y;
    if (x>=points[offset+count-1].x) return points[offset+count-1].y;
    uint lo=0, hi=count-1;
    while (hi-lo>1) { uint mid=(lo+hi)>>1; if (points[offset+mid].x<=x) lo=mid; else hi=mid; }
    float2 a=points[offset+lo], b=points[offset+hi];
    return mix(a.y,b.y,(x-a.x)/(b.x-a.x));
}

inline bool solve3(thread float A[3][3], float3 b, thread float3& x)
{
    float M[3][4];
    for (int r=0;r<3;++r) { for(int c=0;c<3;++c) M[r][c]=A[r][c]; M[r][3]=b[r]; }
    for (int c=0;c<3;++c) {
        int pivot=c;
        for(int r=c+1;r<3;++r) if(abs(M[r][c])>abs(M[pivot][c])) pivot=r;
        if(abs(M[pivot][c])<1e-20f) return false;
        if(pivot!=c) for(int k=0;k<4;++k) { float t=M[c][k]; M[c][k]=M[pivot][k]; M[pivot][k]=t; }
        for(int r=0;r<3;++r) if(r!=c) { float q=M[r][c]/M[c][c]; for(int k=c;k<4;++k) M[r][k]-=q*M[c][k]; }
    }
    for(int r=0;r<3;++r) x[r]=M[r][3]/M[r][r];
    return true;
}

inline float3 rgb_to_lab(float3 rgb, device const float* fwd, uint n)
{
    device const float* matrix=fwd+4*n;
    device const float* white=matrix+9;
    float3 xyz=float3(
        dot(rgb,float3(matrix[0],matrix[1],matrix[2]))/white[0],
        dot(rgb,float3(matrix[3],matrix[4],matrix[5]))/white[1],
        dot(rgb,float3(matrix[6],matrix[7],matrix[8]))/white[2]);
    float d=6.0f/29.0f, d3=d*d*d;
    float3 q;
    for(int i=0;i<3;++i) q[i]=xyz[i]>d3?pow(xyz[i],1.0f/3.0f):xyz[i]/(3.0f*d*d)+4.0f/29.0f;
    return float3(116.0f*q.y-16.0f,500.0f*(q.x-q.y),200.0f*(q.y-q.z));
}

inline float rgb2spec_cost(float3 c,float3 target_lab,device const float* fwd,uint n)
{
    device const float* weights=fwd+n;
    float3 rgb=float3(0.0f);
    for(uint i=0;i<n;++i) {
        float l=fwd[i], v=(c.x*l+c.y)*l+c.z;
        float s=0.5f*v*rsqrt(1.0f+v*v)+0.5f;
        rgb+=float3(weights[i],weights[n+i],weights[2*n+i])*s;
    }
    float3 residual=target_lab-rgb_to_lab(rgb,fwd,n);
    return dot(residual,residual);
}

inline float3 rgb2spec_optimize(float3 wavelength_coeff,float3 target,
    device const float* fwd,uint n)
{
    if(n==0) return wavelength_coeff;
    const float origin=360.0f, scale=1.0f/470.0f;
    float A=wavelength_coeff.x,B=wavelength_coeff.y,C=wavelength_coeff.z;
    float3 c=float3(A/(scale*scale),2.0f*A*origin/scale+B/scale,A*origin*origin+B*origin+C);
    device const float* weights=fwd+n;
    device const float* matrix=fwd+4*n;
    device const float* white=matrix+9;
    float3 reproduced=float3(0.0f);
    float dout[3][3]={{0,0,0},{0,0,0},{0,0,0}};
    for(uint i=0;i<n;++i) {
        float l=fwd[i], P=(c.x*l+c.y)*l+c.z, q=rsqrt(1.0f+P*P);
        float s=0.5f*P*q+0.5f, derivative=0.5f*q*q*q;
        float3 w=float3(weights[i],weights[n+i],weights[2*n+i]); reproduced+=w*s;
        for(int o=0;o<3;++o) { dout[o][0]+=w[o]*derivative*l*l; dout[o][1]+=w[o]*derivative*l; dout[o][2]+=w[o]*derivative; }
    }
    float3 relative=float3(
        dot(reproduced,float3(matrix[0],matrix[1],matrix[2]))/white[0],
        dot(reproduced,float3(matrix[3],matrix[4],matrix[5]))/white[1],
        dot(reproduced,float3(matrix[6],matrix[7],matrix[8]))/white[2]);
    float d=6.0f/29.0f, d3=d*d*d; float3 gradient;
    for(int i=0;i<3;++i) gradient[i]=relative[i]>d3?(1.0f/3.0f)/(pow(relative[i],2.0f/3.0f)*white[i]):1.0f/(3.0f*d*d*white[i]);
    float G[3][3]={{0,116.0f*gradient.y,0},{500.0f*gradient.x,-500.0f*gradient.y,0},{0,200.0f*gradient.y,-200.0f*gradient.z}};
    float lab_rgb[3][3];
    for(int a=0;a<3;++a) for(int b=0;b<3;++b) { lab_rgb[a][b]=0; for(int k=0;k<3;++k) lab_rgb[a][b]+=G[a][k]*matrix[k*3+b]; }
    float J[3][3];
    for(int a=0;a<3;++a) for(int b=0;b<3;++b) { J[a][b]=0; for(int k=0;k<3;++k) J[a][b]-=lab_rgb[a][k]*dout[k][b]; }
    float3 target_lab=rgb_to_lab(target,fwd,n), residual=target_lab-rgb_to_lab(reproduced,fwd,n);
    float base=dot(residual,residual), normal[3][3]; float3 rhs=float3(0.0f);
    for(int a=0;a<3;++a) { for(int b=0;b<3;++b) { normal[a][b]=0; for(int k=0;k<3;++k) normal[a][b]+=J[k][a]*J[k][b]; } for(int k=0;k<3;++k) rhs[a]+=J[k][a]*residual[k]; }
    float lambda=0.0f;
    for(int trial=0;trial<20;++trial) {
        float M[3][3]; for(int a=0;a<3;++a) for(int b=0;b<3;++b) M[a][b]=normal[a][b]+(a==b?lambda:0.0f);
        float3 step;
        if(solve3(M,rhs,step) && rgb2spec_cost(c-step,target_lab,fwd,n)<base) { c-=step; break; }
        lambda=lambda>0.0f?lambda*10.0f:1e-3f;
    }
    return float3(c.x*scale*scale,c.y*scale-2.0f*c.x*origin*scale*scale,c.z-c.y*origin*scale+c.x*(origin*scale)*(origin*scale));
}

inline float3 measure_status(
    float3 coordinate, constant DirectParams& p,
    device const float4* density_data, device const float4* status_data,
    thread float J[3][3])
{
    float numerator[3]={0,0,0}, denominator[3]={0,0,0};
    float derivative[3][3]={{0,0,0},{0,0,0},{0,0,0}};
    float3 active=select(float3(0.0f),float3(1.0f),coordinate>p.minimum_negative_coordinate.xyz);
    float3 amount=max(coordinate-p.minimum_negative_coordinate.xyz,0.0f);
    for(uint i=0;i<p.spectral_count;++i) {
        float4 d=density_data[i];
        float3 basis=float3(d.y,d.z,d.w);
        float optical=max(0.0f,d.x+dot(amount,basis));
        float transmission=pow(10.0f,-optical);
        float3 weights=status_data[i].xyz;
        for(int o=0;o<3;++o) {
            float wt=weights[o]; denominator[o]+=wt; numerator[o]+=wt*transmission;
            for(int c=0;c<3;++c) derivative[o][c]+=wt*transmission*basis[c]*active[c];
        }
    }
    float3 measured;
    for(int o=0;o<3;++o) {
        measured[o]=-log10(max(numerator[o]/max(denominator[o],1e-20f),1e-20f));
        for(int c=0;c<3;++c) J[o][c]=derivative[o][c]/max(numerator[o],1e-20f);
    }
    return measured;
}

inline float3 calibrate_density(float3 target, constant DirectParams& p,
    device const float4* density_data, device const float4* status_data)
{
    float3 desired=max(p.calibration_zero_measured.xyz+target-p.calibration_zero_target.xyz,
                       p.calibration_minimum_status_m.xyz);
    float A0[3][3]={{p.calibration_jacobian_0.x,p.calibration_jacobian_0.y,p.calibration_jacobian_0.z},
                    {p.calibration_jacobian_1.x,p.calibration_jacobian_1.y,p.calibration_jacobian_1.z},
                    {p.calibration_jacobian_2.x,p.calibration_jacobian_2.y,p.calibration_jacobian_2.z}};
    float3 delta;
    if(!solve3(A0,desired-p.calibration_zero_measured.xyz,delta)) return target;
    float3 input=p.calibration_zero_target.xyz+delta;
    for(int iteration=0;iteration<10;++iteration) {
        float J[3][3];
        float3 measured=measure_status(input,p,density_data,status_data,J);
        float3 residual=desired-measured;
        if(max(max(abs(residual.x),abs(residual.y)),abs(residual.z))<2e-5f) break;
        float normal[3][3]; float3 rhs=float3(0.0f);
        for(int r=0;r<3;++r) {
            for(int c=0;c<3;++c) { normal[r][c]=0.0f; for(int o=0;o<3;++o) normal[r][c]+=J[o][r]*J[o][c]; }
            normal[r][r]+=1e-5f;
            for(int o=0;o<3;++o) rhs[r]+=J[o][r]*residual[o];
        }
        float3 update;
        if(!solve3(normal,rhs,update)) break;
        float maximum=max(max(abs(update.x),abs(update.y)),abs(update.z));
        if(maximum>0.5f) update*=0.5f/maximum;
        input+=update;
    }
    return input;
}

inline float smooth_step(float a,float b,float x) { float t=clamp((x-a)/(b-a),0.0f,1.0f); return t*t*(3.0f-2.0f*t); }

inline float3 color_response(float3 coordinate, constant DirectParams& p)
{
    float amount=1.5f*(1.0f+clamp(p.color_density,-4.0f,4.0f)/4.0f);
    if(amount<=0.0f || p.response_response_amount==0.0f) return coordinate;
    float3 n=(coordinate-p.minimum_negative_coordinate.xyz)/p.neutral_negative_increment.xyz;
    float neutral=(n.x+n.y+n.z)/3.0f;
    float3 chroma=n-neutral;
    float magnitude=sqrt(dot(chroma,chroma)/3.0f);
    float density_position=p.response_density_center==1.25f && p.response_density_width==1.0f
        ?neutral:(neutral-p.response_density_center)/p.response_density_width+1.25f;
    float envelope=smooth_step(0.0f,0.20f,density_position)*(1.0f-smooth_step(1.75f,2.50f,density_position));
    float compression=p.response_chroma_compression*amount*envelope;
    float angle=p.response_warm_hue_center*0.017453292519943295f;
    float3 axis=float3(0.40824829f*cos(angle)+0.70710678f*sin(angle),
                      0.40824829f*cos(angle)-0.70710678f*sin(angle),-0.81649658f*cos(angle));
    float length=sqrt(dot(chroma,chroma));
    float direction=length>1e-6f?dot(chroma,axis)/length:0.0f;
    if(p.response_warm_hue_width!=1.0f)
        direction=cos(min(3.14159265f,acos(clamp(direction,-1.0f,1.0f))/p.response_warm_hue_width));
    float warm_hue=smooth_step(0.15f,0.90f,direction);
    float warm_density=smooth_step(0.30f,0.60f,density_position)*(1.0f-smooth_step(1.40f,2.00f,density_position));
    float warm_chroma=smooth_step(0.02f,0.08f,magnitude)*(1.0f-smooth_step(0.35f,0.75f,magnitude));
    float warm=warm_hue*warm_density*warm_chroma;
    compression*=1.0f-p.response_warm_protection*warm;
    float scale=1.0f/(1.0f+compression*magnitude/p.response_chroma_knee);
    float depth=0.08f*p.color_depth*amount*envelope*magnitude/(magnitude+p.response_chroma_knee);
    float3 guided=chroma;
    float guidance=0.15f*warm;
    if(guidance>0.0f && length>1e-6f) {
        float3 mixed=mix(chroma,axis*length,guidance);
        float mixed_length=sqrt(dot(mixed,mixed)); if(mixed_length>1e-6f) guided=mixed*(length/mixed_length);
    }
    float rotation=p.response_warm_hue_shift*0.017453292519943295f*warm;
    if(rotation!=0.0f && length>1e-6f) {
        float3 tangent=float3(guided.y-guided.z,guided.z-guided.x,guided.x-guided.y)*0.577350269f;
        guided=cos(rotation)*guided+sin(rotation)*tangent;
    }
    float3 result=max(p.minimum_negative_coordinate.xyz,
               p.minimum_negative_coordinate.xyz+((neutral-depth)+scale*guided)*p.neutral_negative_increment.xyz);
    return p.response_response_amount==1.0f?result:coordinate+p.response_response_amount*(result-coordinate);
}

inline float blackbody_relative(float wavelength_nm,float temperature)
{
    const float reference=560.0f, c2=1.438776877e7f;
    float ratio=reference/wavelength_nm;
    return pow(ratio,5.0f)*(exp(c2/(reference*temperature))-1.0f)
        /(exp(c2/(wavelength_nm*temperature))-1.0f);
}

inline float3 ap0_to_rec709(float3 ap0)
{
    return float3(
        2.5216862f*ap0.x-1.1341309f*ap0.y-0.3875553f*ap0.z,
       -0.2764799f*ap0.x+1.3727191f*ap0.y-0.0962392f*ap0.z,
       -0.0153781f*ap0.x-0.1529753f*ap0.y+1.1683534f*ap0.z);
}

inline float3 gamut_compress(float3 rgb)
{
    float l=dot(float3(0.2126390059f,0.7151686788f,0.0721923154f),rgb);
    if(!isfinite(l)||l<=1e-6f||l>=1.0f-1e-6f) return rgb;
    float scale=1.0f, low=min(min(rgb.x,rgb.y),rgb.z)/l;
    if(low<0.1f) { float mapped=0.1f*exp((low-0.1f)/0.1f); scale=min(scale,(1.0f-mapped)/(1.0f-low)); }
    float high=(1.0f-max(max(rgb.x,rgb.y),rgb.z))/(1.0f-l);
    if(high<0.1f) { float mapped=0.1f*exp((high-0.1f)/0.1f); scale=min(scale,(1.0f-mapped)/(1.0f-high)); }
    return l+scale*(rgb-l);
}

inline uint mix_bits(uint value)
{
    value^=value>>16; value*=0x7feb352du; value^=value>>15;
    value*=0x846ca68bu; value^=value>>16; return value;
}

inline float normal_sample(uint seed,int x,int y,int stage,int channel)
{
    uint key=seed^mix_bits(uint(x)+0x9e3779b9u)^mix_bits(uint(y)+0x85ebca6bu)
        ^mix_bits(uint(stage)*0xc2b2ae35u+uint(channel));
    float u1=max((float(mix_bits(key)&0x00ffffffu)+0.5f)/16777216.0f,1e-7f);
    float u2=(float(mix_bits(key^0x68bc21ebu)&0x00ffffffu)+0.5f)/16777216.0f;
    return sqrt(-2.0f*log(u1))*cos(6.283185307179586f*u2);
}

inline float hat_integral(float x)
{
    if(x<=-1.0f) return 0.0f;
    if(x<0.0f) return 0.5f*(x+1.0f)*(x+1.0f);
    if(x<1.0f) return 1.0f-0.5f*(1.0f-x)*(1.0f-x);
    return 1.0f;
}
inline float aperture_energy(float width)
{
    if(width>=2.0f) return 1.0f/width-7.0f/(15.0f*width*width);
    if(width>=1.0f) {
        float u=2.0f-width;
        return (width-7.0f/15.0f+u*u*u*u*u/60.0f)/(width*width);
    }
    return 2.0f/3.0f-width*width/6.0f+width*width*width/20.0f;
}
inline float texture_band(uint seed,int x,int y,int stage,int channel,float scale)
{
    float footprint=1.0f/scale;
    if(footprint>8.0f) return normal_sample(seed,x,y,stage,channel)*aperture_energy(footprint);
    float px=(float(x)+0.5f)/scale+0.37f,py=(float(y)+0.5f)/scale+0.61f;
    float lx=px-0.5f*footprint,hx=px+0.5f*footprint;
    float ly=py-0.5f*footprint,hy=py+0.5f*footprint;
    float value=0.0f;
    for(int j=int(floor(ly));j<=int(ceil(hy));++j) {
        float wy=(hat_integral(hy-float(j))-hat_integral(ly-float(j)))/footprint;
        for(int i=int(floor(lx));i<=int(ceil(hx));++i) {
            float wx=(hat_integral(hx-float(i))-hat_integral(lx-float(i)))/footprint;
            value+=wx*wy*normal_sample(seed,i,j,stage,channel);
        }
    }
    return value;
}
inline float texture_field(uint seed,int x,int y,int stage,int channel,constant DirectParams& p)
{
    bool print=stage==1;
    float fine_scale=p.grain_size*(print?0.50f:0.85f);
    float coarse_scale=p.grain_size*(print?1.10f:1.80f);
    float fine=texture_band(seed^0xa511e9b3u,x,y,stage,channel,fine_scale);
    float coarse=texture_band(seed^0x63d83595u,x,y,stage,channel,coarse_scale);
    float fine_norm=1.0f/aperture_energy(p.reserved_grain_float.x/fine_scale);
    float coarse_norm=1.0f/aperture_energy(p.reserved_grain_float.x/coarse_scale);
    return (print?0.948683298f:0.866025404f)*fine_norm*fine
         +(print?0.316227766f:0.5f)*coarse_norm*coarse;
}
inline float spatial_normal(uint seed,int x,int y,int stage,int channel,constant DirectParams& p)
{
    float shared=texture_field(seed,x,y,stage,3,p);
    float independent=texture_field(seed,x,y,stage,channel,p);
    return 0.9f*shared+0.435889894f*independent;
}

inline float3 granularity_sigma(device const float4* samples,uint count,float3 density,float minimum,float maximum)
{
    float3 result;
    for(int c=0;c<3;++c) {
        float position=clamp((density[c]-minimum)/(maximum-minimum),0.0f,1.0f)*float(count-1);
        uint lo=min(uint(floor(position)),count-2), hi=lo+1;
        result[c]=mix(samples[lo][c],samples[hi][c],position-float(lo));
    }
    return result;
}

kernel void filmviz_direct(
    device const uchar* source [[buffer(0)]], device uchar* destination [[buffer(1)]],
    device const float4* negative_exposure_data [[buffer(2)]],
    device const float4* negative_density_data [[buffer(3)]],
    device const float4* status_m_data [[buffer(4)]],
    device const float4* print_exposure_data [[buffer(5)]],
    device const float4* print_density_data [[buffer(6)]],
    device const float4* viewer_data [[buffer(7)]],
    device const float2* curves [[buffer(8)]],
    device const float* rgb_scale [[buffer(9)]],
    device const float* rgb_data [[buffer(10)]],
    device const float* rgb_forward [[buffer(11)]],
    device const float4* negative_granularity [[buffer(12)]],
    device const float4* print_granularity [[buffer(13)]],
    constant DirectParams& p [[buffer(14)]],
    device const float4* prepared_exposure [[buffer(15)]],
    device float4* negative_residual [[buffer(16)]],
    device float4* print_residual [[buffer(17)]], uint2 gid [[thread_position_in_grid]])
{
    int x=p.render_x1+int(gid.x), y=p.render_y1+int(gid.y);
    if(x>=p.render_x2||y>=p.render_y2) return;
    float4 src=read_pixel(source,p.source_row_bytes,x,y,p.source_x1,p.source_y1);
    float3 negative_exposure=float3(0.0f);
    if(p.reserved_header0!=0u) {
        uint width=uint(p.source_x2-p.source_x1);
        negative_exposure=prepared_exposure[uint(y-p.source_y1)*width+uint(x-p.source_x1)].xyz;
    } else {
        float3 ap0=src.xyz;
        float luminance=dot(float3(0.34396645f,0.72816610f,-0.07213255f),ap0);
        float scene_scale=isfinite(luminance)&&luminance>0.18f?luminance/0.18f:1.0f;
        float3 reconstruction=ap0/scene_scale;
        float spectrum_scale=max(1.0f,max(max(reconstruction.x,reconstruction.y),reconstruction.z));
        float3 coeff=rgb2spec_fetch(rgb_scale,rgb_data,p.rgb2spec_resolution,reconstruction/spectrum_scale);
        float3 rgb_target=clamp(reconstruction/spectrum_scale,0.0f,1.0f);
        if(!(rgb_target.x==rgb_target.y && rgb_target.y==rgb_target.z))
            coeff=rgb2spec_optimize(coeff,rgb_target,rgb_forward,p.rgb2spec_forward_count);
        spectrum_scale*=scene_scale;
        for(uint i=0;i<p.spectral_count;++i) {
            float4 s=negative_exposure_data[i];
            negative_exposure+=s.yzw*(spectrum_scale*spectrum(coeff,s.x));
        }
        float3 reference_flash=p.reference_negative_exposure.xyz*(p.middle_gray/0.18f);
        negative_exposure+=reference_flash*(p.negative_flash_percent*0.01f);
    }
    float3 reference=p.reference_negative_exposure.xyz*(p.middle_gray/0.18f);
    float3 log_exposure=-0.515f+(p.reserved_header0!=0u?0.0f:0.301029995664f*p.exposure_stops)+log10(max(negative_exposure,float3(1e-20f))/max(reference,float3(1e-20f)));
    float3 negative_status=float3(
        curve_sample(curves,p.curve_negative_01.x,p.curve_negative_01.y,log_exposure.x),
        curve_sample(curves,p.curve_negative_01.z,p.curve_negative_01.w,log_exposure.y),
        curve_sample(curves,p.curve_negative_2_print_0.x,p.curve_negative_2_print_0.y,log_exposure.z));
    float contrast=exp2(0.2f*p.push_pull_stops);
    negative_status=p.reference_negative_density.xyz+contrast*(negative_status-p.reference_negative_density.xyz);
    bool use_grain=p.grain_enabled!=0u&&(p.negative_grain>0.0f||p.print_grain>0.0f);
    float3 ns=granularity_sigma(negative_granularity,p.granularity_count,negative_status,p.granularity_density_min,p.granularity_density_max);
    float3 negative_noise=float3(0.0f),print_noise=float3(0.0f);
    if(use_grain&&p.negative_grain>0.0f) for(int c=0;c<3;++c)
        negative_noise[c]=p.negative_grain*ns[c]*spatial_normal(p.frame_seed,x,y,0,c,p);
    float3 outputs[3];
    float3 baseline_print_record=float3(0.0f);
    for(int pass=0;pass<(use_grain?3:1);++pass) {
        if((pass==1&&p.negative_grain<=0.0f)||(pass==2&&p.print_grain<=0.0f)) {
            outputs[pass]=outputs[0]; continue;
        }
        float3 print_record;
        if(pass==2) {
            print_record=baseline_print_record+print_noise;
        } else {
            float3 perturbed_status=negative_status+(pass==1?negative_noise:float3(0.0f));
            float3 coordinate=color_response(calibrate_density(perturbed_status,p,negative_density_data,status_m_data),p);
            float3 dye_amount=max(coordinate-p.minimum_negative_coordinate.xyz,0.0f);
            float bypass_scale=0.14f*clamp(p.negative_bleach_bypass,0.0f,1.0f);
            if(bypass_scale>0.0f) for(uint i=0;i<p.spectral_count;++i) {
                float4 d=negative_density_data[i]; float density=max(0.0f,d.x+dot(dye_amount,d.yzw));
                float shape=1.0f-2.0f*float(i)/float(max(p.spectral_count-1,1u));
                if(shape<0.0f) bypass_scale=min(bypass_scale,density/-shape);
            }
            float3 print_exposure=float3(0.0f), print_reference=float3(0.0f);
            for(uint i=0;i<p.spectral_count;++i) {
                float4 d=negative_density_data[i];
                float density=max(0.0f,d.x+dot(dye_amount,d.yzw));
                float shape=1.0f-2.0f*float(i)/float(max(p.spectral_count-1,1u));
                float transmission=pow(10.0f,-max(0.0f,density+bypass_scale*shape));
                float4 pe=print_exposure_data[i];
                if(pe.w<=0.0f) transmission=0.0f;
                float printer=blackbody_relative(p.wavelength_min_nm+float(i)*p.wavelength_step_nm,p.printer_temperature);
                print_exposure+=pe.xyz*(printer*transmission);
                print_reference+=pe.xyz*(printer*pe.w);
            }
            print_exposure+=print_reference*(p.print_flash_percent*0.01f);
            float3 lights=float3(p.printer_light_red,p.printer_light_green,p.printer_light_blue)+p.printer_light_master;
            float3 print_log=log10(max(print_exposure,float3(1e-20f))/max(print_reference,float3(1e-20f)))
                +p.print_target_log_exposure.xyz+(lights-25.0f)*0.025f;
            print_record=float3(
                curve_sample(curves,p.curve_negative_2_print_0.z,p.curve_negative_2_print_0.w,print_log.x),
                curve_sample(curves,p.curve_print_12.x,p.curve_print_12.y,print_log.y),
                curve_sample(curves,p.curve_print_12.z,p.curve_print_12.w,print_log.z));
            if(pass==0) baseline_print_record=print_record;
            if(pass==0&&use_grain&&p.print_grain>0.0f) {
                float3 ps=granularity_sigma(print_granularity,p.granularity_count,print_record,p.granularity_density_min,p.granularity_density_max);
                for(int c=0;c<3;++c) print_noise[c]=p.print_grain*ps[c]*spatial_normal(p.frame_seed,x,y,1,c,p);
            }
        }
        float mean_density=0.0f;
        for(uint i=0;i<p.spectral_count;++i) {
            float density=dot(print_record,print_density_data[i].xyz);
            mean_density+=pass==2?max(0.0f,density):density;
        }
        mean_density/=float(p.spectral_count);
        float chroma_scale=1.0f-0.8f*clamp(p.print_bleach_bypass,0.0f,1.0f);
        float3 viewed=float3(0.0f);
        for(uint i=0;i<p.spectral_count;++i) {
            float density=dot(print_record,print_density_data[i].xyz);
            if(pass==2) density=max(0.0f,density);
            density=mean_density+chroma_scale*(density-mean_density);
            viewed+=viewer_data[i].xyz*pow(10.0f,-density);
        }
        float3 converted=viewed;
        if(p.output_profile==1) converted=pow(max(gamut_compress(ap0_to_rec709(viewed)),0.0f),float3(1.0f/2.4f));
        outputs[pass]=converted;
    }
    if(use_grain) {
        uint width=uint(p.render_x2-p.render_x1);
        uint index=uint(y-p.render_y1)*width+uint(x-p.render_x1);
        float3 base=p.output_profile==1?pow(max(outputs[0],0.0f),float3(2.4f)):outputs[0];
        float3 neg=p.output_profile==1?pow(max(outputs[1],0.0f),float3(2.4f)):outputs[1];
        float3 prt=p.output_profile==1?pow(max(outputs[2],0.0f),float3(2.4f)):outputs[2];
        float3 denominator=max(base,1e-6f);
        float3 ng=clamp(log(max(neg,1e-6f)/denominator),-2.0f,2.0f);
        float3 pg=clamp(log(max(prt,1e-6f)/denominator),-2.0f,2.0f);
        negative_residual[index]=float4(max(base,0.0f)*(exp(ng)-1.0f),0.0f);
        print_residual[index]=float4(max(base,0.0f)*(exp(pg)-1.0f),0.0f);
    }
    write_pixel(destination,p.destination_row_bytes,x,y,p.destination_x1,p.destination_y1,float4(outputs[0],src.w));
}

kernel void filmviz_prepare_halation(
    device const uchar* source [[buffer(0)]],
    device float4* exposure_output [[buffer(1)]],
    device float4* highlight_output [[buffer(2)]],
    device const float4* negative_exposure_data [[buffer(3)]],
    device const float* rgb_scale [[buffer(4)]],
    device const float* rgb_data [[buffer(5)]],
    device const float* rgb_forward [[buffer(6)]],
    constant DirectParams& p [[buffer(7)]],
    constant SpatialParams& spatial [[buffer(8)]],
    uint2 gid [[thread_position_in_grid]])
{
    if(gid.x>=spatial.width||gid.y>=spatial.height) return;
    int x=p.source_x1+int(gid.x), y=p.source_y1+int(gid.y);
    float4 src=read_pixel(source,p.source_row_bytes,x,y,p.source_x1,p.source_y1);
    float3 ap0=src.xyz;
    float exposure_scale=exp2(p.exposure_stops);
    // Match the normal path: reconstruct unexposed AP0, then scale exposure once.
    float luminance=dot(float3(0.34396645f,0.72816610f,-0.07213255f),ap0);
    float scene_scale=isfinite(luminance)&&luminance>0.18f?luminance/0.18f:1.0f;
    float3 reconstruction=ap0/scene_scale;
    float spectrum_scale=max(1.0f,max(max(reconstruction.x,reconstruction.y),reconstruction.z));
    float3 coeff=rgb2spec_fetch(rgb_scale,rgb_data,p.rgb2spec_resolution,reconstruction/spectrum_scale);
    float3 rgb_target=clamp(reconstruction/spectrum_scale,0.0f,1.0f);
    if(!(rgb_target.x==rgb_target.y&&rgb_target.y==rgb_target.z))
        coeff=rgb2spec_optimize(coeff,rgb_target,rgb_forward,p.rgb2spec_forward_count);
    spectrum_scale*=scene_scale;
    float3 exposure=float3(0.0f);
    for(uint i=0;i<p.spectral_count;++i) {
        float4 s=negative_exposure_data[i];
        exposure+=s.yzw*(spectrum_scale*spectrum(coeff,s.x));
    }
    float3 reference=p.reference_negative_exposure.xyz*(p.middle_gray/0.18f);
    exposure=(exposure+reference*(p.negative_flash_percent*0.01f))*exposure_scale;
    // Exposure carries intensity; luminance only selects the source mask.
    float safe_luminance=max(0.0f,luminance*exposure_scale), weight=1.0f;
    if(spatial.threshold>1e-8f) {
        float onset=0.5f*spatial.threshold;
        if(safe_luminance<=onset) weight=0.0f;
        else { float t=clamp((safe_luminance-onset)/max(spatial.threshold-onset,1e-8f),0.0f,1.0f); weight=t*t*(3.0f-2.0f*t); }
    }
    uint index=gid.y*spatial.width+gid.x;
    exposure_output[index]=float4(exposure,src.w);
    highlight_output[index]=float4(max(exposure,0.0f)*weight,src.w);
}

kernel void filmviz_box_blur(
    device const float4* source [[buffer(0)]], device float4* destination [[buffer(1)]],
    constant SpatialParams& p [[buffer(2)]], uint2 gid [[thread_position_in_grid]])
{
    if(gid.x>=p.width||gid.y>=p.height) return;
    float4 value=float4(0.0f); int radius=int(p.radius);
    for(int tap=-radius;tap<=radius;++tap) {
        uint x=p.horizontal!=0u?uint(clamp(int(gid.x)+tap,0,int(p.width)-1)):gid.x;
        uint y=p.horizontal==0u?uint(clamp(int(gid.y)+tap,0,int(p.height)-1)):gid.y;
        value+=source[y*p.width+x];
    }
    destination[gid.y*p.width+gid.x]=value/float(2*radius+1);
}

kernel void filmviz_combine_halation(
    device float4* exposure [[buffer(0)]], device const float4* highlight [[buffer(1)]],
    device const float4* near_blur [[buffer(2)]], device const float4* far_blur [[buffer(3)]],
    constant SpatialParams& p [[buffer(4)]], uint2 gid [[thread_position_in_grid]])
{
    if(gid.x>=p.width||gid.y>=p.height) return;
    uint index=gid.y*p.width+gid.x;
    float3 blurred=0.72f*near_blur[index].xyz+0.28f*far_blur[index].xyz;
    float3 spread=max(float3(0.0f),blurred-0.95f*highlight[index].xyz);
    float4 result=exposure[index]; result.xyz+=p.strength*p.scatter.xyz*spread; exposure[index]=result;
}

kernel void filmviz_mtf(
    device const float4* source [[buffer(0)]], device float4* destination [[buffer(1)]],
    device const float* weights [[buffer(2)]], constant SpatialParams& p [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]])
{
    if(gid.x>=p.width||gid.y>=p.height) return;
    int radius=int(p.radius); float3 value=float3(0.0f);
    for(int tap=-radius;tap<=radius;++tap) {
        uint x=p.horizontal!=0u?uint(clamp(int(gid.x)+tap,0,int(p.width)-1)):gid.x;
        uint y=p.horizontal==0u?uint(clamp(int(gid.y)+tap,0,int(p.height)-1)):gid.y;
        float3 sample=source[y*p.width+x].xyz;
        if(p.gamma24!=0u&&p.horizontal!=0u) sample=pow(max(sample,0.0f),float3(2.4f));
        uint w=uint(tap+radius);
        value+=sample*float3(weights[w],weights[uint(2*radius+1)+w],weights[uint(4*radius+2)+w]);
    }
    if(p.gamma24!=0u&&p.horizontal==0u) value=pow(max(value,0.0f),float3(1.0f/2.4f));
    if(p.horizontal==0u&&p.reserved0==0u) value=clamp(value,0.0f,1.0f);
    destination[gid.y*p.width+gid.x]=float4(value,source[gid.y*p.width+gid.x].w);
}

kernel void filmviz_copy_dense(
    device const float4* source [[buffer(0)]], device uchar* destination [[buffer(1)]],
    constant DirectParams& p [[buffer(2)]],
    device const float4* negative_residual [[buffer(3)]],
    device const float4* print_residual [[buffer(4)]], uint2 gid [[thread_position_in_grid]])
{
    uint width=uint(p.render_x2-p.render_x1),height=uint(p.render_y2-p.render_y1);
    if(gid.x>=width||gid.y>=height) return;
    int x=p.render_x1+int(gid.x),y=p.render_y1+int(gid.y);
    uint index=gid.y*width+gid.x;
    float4 pixel=source[index];
    if(p.grain_enabled!=0u&&(p.negative_grain>0.0f||p.print_grain>0.0f)) {
        float3 base=p.output_profile==1?pow(max(pixel.xyz,0.0f),float3(2.4f)):pixel.xyz;
        float3 relative=(negative_residual[index].xyz+print_residual[index].xyz)/max(base,1e-6f);
        float neutral=dot(float3(0.2126f,0.7152f,0.0722f),relative);
        relative=neutral+p.grain_chroma*(relative-neutral);
        // Match ImageProcessor's empirical, noise-free highlight trim.
        float peak=max(0.0f,max(base.x,max(base.y,base.z)));
        float t=clamp((peak-0.12f)/(0.65f-0.12f),0.0f,1.0f);
        float highlight=t*t*(3.0f-2.0f*t);
        float u=clamp(peak/0.12f,0.0f,1.0f);
        float shadow=1.0f-u*u*(3.0f-2.0f*u);
        float visibility=p.grain_tonal_enabled!=0u
            ?(0.80f-0.45f*highlight)*(shadow*p.grain_shadows
                +(1.0f-shadow-highlight)*p.grain_midtones+highlight*p.grain_highlights):1.0f;
        float3 grained=base*(1.0f+visibility*relative);
        pixel.xyz=p.output_profile==1?pow(max(grained,0.0f),float3(1.0f/2.4f)):grained;
        pixel.xyz=clamp(pixel.xyz,0.0f,1.0f);
    }
    write_pixel(destination,p.destination_row_bytes,x,y,p.destination_x1,p.destination_y1,pixel);
}
)METAL";

id<MTLBuffer> make_buffer(id<MTLDevice> device, const std::vector<float>& values)
{
    return values.empty() ? nil : [device newBufferWithBytes:values.data()
        length:values.size()*sizeof(float) options:MTLResourceStorageModeShared];
}

MTLSize threadgroup_size(id<MTLComputePipelineState> pipeline)
{
    const NSUInteger width=std::max<NSUInteger>(1,std::min<NSUInteger>(pipeline.threadExecutionWidth,16));
    const NSUInteger height=std::max<NSUInteger>(1,std::min<NSUInteger>(pipeline.maxTotalThreadsPerThreadgroup/width,16));
    return MTLSizeMake(width,height,1);
}

bool encode_2d(id<MTLCommandBuffer> command,id<MTLComputePipelineState> pipeline,
    NSUInteger width,NSUInteger height,const std::function<void(id<MTLComputeCommandEncoder>)>& bind)
{
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    if(!encoder) return false;
    [encoder setComputePipelineState:pipeline]; bind(encoder);
    [encoder dispatchThreads:MTLSizeMake(width,height,1) threadsPerThreadgroup:threadgroup_size(pipeline)];
    [encoder endEncoding]; return true;
}

struct DirectResources
{
    id<MTLBuffer> negative_exposure=nil;
    id<MTLBuffer> negative_density=nil;
    id<MTLBuffer> status_m=nil;
    id<MTLBuffer> print_exposure=nil;
    id<MTLBuffer> print_density=nil;
    id<MTLBuffer> viewer=nil;
    id<MTLBuffer> curves=nil;
    id<MTLBuffer> rgb_scale=nil;
    id<MTLBuffer> rgb_data=nil;
    id<MTLBuffer> rgb_forward=nil;
    id<MTLBuffer> negative_granularity=nil;
    id<MTLBuffer> print_granularity=nil;
    FilmDirectData data;
};

std::mutex gDirectCacheMutex;
std::unordered_map<std::string,std::weak_ptr<DirectResources>> gDirectCache;

} // namespace

struct FilmVizDirectMetalProcessor::Impl
{
    std::unique_ptr<InputTransform> input_transform;
    std::string input_key;
    std::unique_ptr<OutputTransform> output_transform;
    std::string output_key;
    id<MTLDevice> device=nil;
    id<MTLLibrary> library=nil;
    id<MTLComputePipelineState> pipeline=nil;
    id<MTLComputePipelineState> prepare_halation=nil;
    id<MTLComputePipelineState> box_blur=nil;
    id<MTLComputePipelineState> combine_halation=nil;
    id<MTLComputePipelineState> mtf=nil;
    id<MTLComputePipelineState> copy_dense=nil;
    id<MTLBuffer> negative_exposure=nil;
    id<MTLBuffer> negative_density=nil;
    id<MTLBuffer> status_m=nil;
    id<MTLBuffer> print_exposure=nil;
    id<MTLBuffer> print_density=nil;
    id<MTLBuffer> viewer=nil;
    id<MTLBuffer> curves=nil;
    id<MTLBuffer> rgb_scale=nil;
    id<MTLBuffer> rgb_data=nil;
    id<MTLBuffer> rgb_forward=nil;
    id<MTLBuffer> negative_granularity=nil;
    id<MTLBuffer> print_granularity=nil;
    FilmDirectData data;
    SpatialResponseModel spatial_response;
    bool spatial_response_valid=false;
    std::string spatial_profile_key;
    std::string profile_key;
    std::shared_ptr<DirectResources> shared_resources;
};

FilmVizDirectMetalProcessor::FilmVizDirectMetalProcessor():impl_(std::make_unique<Impl>()) {}
FilmVizDirectMetalProcessor::~FilmVizDirectMetalProcessor()=default;

void FilmVizDirectMetalProcessor::invalidate_profiles()
{
    impl_->negative_exposure=nil; impl_->negative_density=nil; impl_->status_m=nil;
    impl_->print_exposure=nil; impl_->print_density=nil; impl_->viewer=nil; impl_->curves=nil;
    impl_->rgb_scale=nil; impl_->rgb_data=nil; impl_->rgb_forward=nil;
    impl_->negative_granularity=nil; impl_->print_granularity=nil;
    impl_->shared_resources.reset(); impl_->profile_key.clear();
    impl_->spatial_response_valid=false; impl_->spatial_profile_key.clear();
}

bool FilmVizDirectMetalProcessor::configure(const FilmVizOfxRenderSettings& settings,
    const std::string& resources_directory, void* command_queue, std::string& error)
{
    error.clear();
    if (!std::isfinite(settings.grain_size) || settings.grain_size <= 0.0f
        || !std::isfinite(settings.image_width_mm) || settings.image_width_mm <= 0.0f
        || !std::isfinite(settings.negative_grain) || settings.negative_grain < 0.0f
        || !std::isfinite(settings.print_grain) || settings.print_grain < 0.0f
        || !std::isfinite(settings.grain_chroma) || settings.grain_chroma < 0.0f) {
        error="Invalid grain settings"; return false;
    }
    if (!FilmColorResponse::valid_tuning(settings.color_response)) { error="Invalid color response tuning"; return false; }
    try {
        const std::string output_key = resources_directory + ':' + std::to_string(settings.output_profile);
        if (!impl_->output_transform || impl_->output_key != output_key) {
            impl_->output_transform = std::make_unique<OutputTransform>(
                static_cast<OutputTransform::Encoding>(settings.output_profile), resources_directory);
            impl_->output_key = output_key;
        }
        const std::string input_key = resources_directory + ':' + std::to_string(settings.input_profile);
        if (!impl_->input_transform || impl_->input_key != input_key) {
            impl_->input_transform = std::make_unique<InputTransform>(
                static_cast<InputTransform::Encoding>(settings.input_profile), resources_directory);
            impl_->input_key = input_key;
        }
    } catch (const std::exception& exception) {
        error = std::string("OCIO colour transform: ") + exception.what();
        return false;
    }

    if(!command_queue) { error="Resolve did not provide a Metal command queue"; return false; }
    id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)command_queue;
    id<MTLDevice> device=queue.device;
    if(!device) { error="Resolve Metal command queue has no device"; return false; }
    if(impl_->device!=device||!impl_->pipeline) {
        impl_->device=device; impl_->profile_key.clear();
        NSError* library_error=nil;
        MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
        impl_->library=[device newLibraryWithSource:[NSString stringWithUTF8String:kDirectMetalSource]
            options:options error:&library_error];
        if(!impl_->library) { error=library_error?[[library_error localizedDescription] UTF8String]:"could not compile direct FilmViz Metal library"; return false; }
        NSError* pipeline_error=nil;
        auto make_pipeline=[&](NSString* name)->id<MTLComputePipelineState> {
            id<MTLFunction> function=[impl_->library newFunctionWithName:name];
            return function?[device newComputePipelineStateWithFunction:function error:&pipeline_error]:nil;
        };
        impl_->pipeline=make_pipeline(@"filmviz_direct");
        impl_->prepare_halation=make_pipeline(@"filmviz_prepare_halation");
        impl_->box_blur=make_pipeline(@"filmviz_box_blur");
        impl_->combine_halation=make_pipeline(@"filmviz_combine_halation");
        impl_->mtf=make_pipeline(@"filmviz_mtf");
        impl_->copy_dense=make_pipeline(@"filmviz_copy_dense");
        if(!impl_->pipeline||!impl_->prepare_halation||!impl_->box_blur||!impl_->combine_halation||!impl_->mtf||!impl_->copy_dense) {
            error=pipeline_error?[[pipeline_error localizedDescription] UTF8String]:"could not create FilmViz Metal pipelines"; return false;
        }
    }
    const std::string profile_key=resources_directory+'\n'+settings.negative_profile+'\n'+settings.print_profile;
    if(profile_key!=impl_->spatial_profile_key) {
        const auto* negative_profile=NegativeProfileCatalog::find(settings.negative_profile);
        const auto* print_profile=PrintProfileCatalog::find(settings.print_profile=="none"
            ?PrintProfileCatalog::default_profile().identifier:settings.print_profile);
        impl_->spatial_response_valid=false;
        if(negative_profile&&print_profile) {
            const std::filesystem::path resources(resources_directory);
            impl_->spatial_response_valid=impl_->spatial_response.load(
                (resources/negative_profile->resource_directory/(negative_profile->resource_prefix+"_modulation_transfer_function_curves.csv")).string(),
                (resources/print_profile->resource_directory/print_profile->mtf_filename).string());
        }
        impl_->spatial_profile_key=profile_key;
    }
    const std::string key=std::to_string(reinterpret_cast<std::uintptr_t>((__bridge void*)device))+'\n'+profile_key;
    if(key==impl_->profile_key&&impl_->rgb_data) return true;

    {
        const std::lock_guard<std::mutex> lock(gDirectCacheMutex);
        const auto found=gDirectCache.find(key);
        if(found!=gDirectCache.end()) {
            auto shared=found->second.lock();
            if(shared) {
                impl_->shared_resources=shared; impl_->data=shared->data;
                impl_->negative_exposure=shared->negative_exposure; impl_->negative_density=shared->negative_density;
                impl_->status_m=shared->status_m; impl_->print_exposure=shared->print_exposure;
                impl_->print_density=shared->print_density; impl_->viewer=shared->viewer; impl_->curves=shared->curves;
                impl_->rgb_scale=shared->rgb_scale; impl_->rgb_data=shared->rgb_data; impl_->rgb_forward=shared->rgb_forward;
                impl_->negative_granularity=shared->negative_granularity; impl_->print_granularity=shared->print_granularity;
                impl_->profile_key=key; return true;
            }
        }
    }

    FilmPipeline pipeline;
    FilmPipeline::Settings baseline;
    baseline.resources_directory=resources_directory;
    baseline.negative_profile=settings.negative_profile;
    baseline.print_profile=settings.print_profile;
    baseline.middle_gray=0.18f;
    baseline.printer_temperature_kelvin=3200.0f;
    if(!pipeline.initialize(baseline)||!pipeline.direct_data(impl_->data)) {
        error=pipeline.error().empty()?"could not export FilmViz direct profile data":pipeline.error(); return false;
    }
    impl_->negative_exposure=make_buffer(device,impl_->data.negative_exposure_samples);
    impl_->negative_density=make_buffer(device,impl_->data.negative_density_samples);
    impl_->status_m=make_buffer(device,impl_->data.status_m_samples);
    impl_->print_exposure=make_buffer(device,impl_->data.print_exposure_samples);
    impl_->print_density=make_buffer(device,impl_->data.print_density_samples);
    impl_->viewer=make_buffer(device,impl_->data.viewer_ap0_samples);
    impl_->curves=make_buffer(device,impl_->data.characteristic_points);
    impl_->rgb_scale=make_buffer(device,impl_->data.rgb2spec_scale);
    impl_->rgb_data=make_buffer(device,impl_->data.rgb2spec_data);
    impl_->rgb_forward=make_buffer(device,impl_->data.rgb2spec_forward);
    impl_->negative_granularity=make_buffer(device,impl_->data.negative_granularity_samples);
    impl_->print_granularity=make_buffer(device,impl_->data.print_granularity_samples);
    if(!impl_->negative_exposure||!impl_->negative_density||!impl_->status_m||!impl_->print_exposure
        ||!impl_->print_density||!impl_->viewer||!impl_->curves||!impl_->rgb_scale||!impl_->rgb_data||!impl_->rgb_forward
        ||!impl_->negative_granularity||!impl_->print_granularity) {
        error="could not upload FilmViz direct Metal resources"; return false;
    }
    auto shared=std::make_shared<DirectResources>();
    shared->negative_exposure=impl_->negative_exposure; shared->negative_density=impl_->negative_density;
    shared->status_m=impl_->status_m; shared->print_exposure=impl_->print_exposure;
    shared->print_density=impl_->print_density; shared->viewer=impl_->viewer; shared->curves=impl_->curves;
    shared->rgb_scale=impl_->rgb_scale; shared->rgb_data=impl_->rgb_data; shared->rgb_forward=impl_->rgb_forward;
    shared->negative_granularity=impl_->negative_granularity; shared->print_granularity=impl_->print_granularity;
    shared->data=impl_->data; impl_->shared_resources=shared;
    { const std::lock_guard<std::mutex> lock(gDirectCacheMutex); gDirectCache[key]=shared; }
    impl_->profile_key=key;
    return true;
}

bool FilmVizDirectMetalProcessor::render(const FilmVizOfxRenderSettings& settings, void* command_queue,
    const FilmVizOfxMetalFrame& source, const FilmVizOfxMetalFrame& destination,
    int render_x1,int render_y1,int render_x2,int render_y2,double time,std::string& error)
{
    error.clear();
    if(!command_queue||!source.buffer||!destination.buffer||!impl_->pipeline||!impl_->rgb_data) {
        error="FilmViz direct Metal processor is not configured"; return false;
    }
    render_x1=std::max(render_x1,destination.x1); render_y1=std::max(render_y1,destination.y1);
    render_x2=std::min(render_x2,destination.x2); render_y2=std::min(render_y2,destination.y2);
    if(render_x1>=render_x2||render_y1>=render_y2) return true;
    const std::int64_t source_width_value=static_cast<std::int64_t>(source.x2)-source.x1;
    const std::int64_t source_height_value=static_cast<std::int64_t>(source.y2)-source.y1;
    const std::int64_t render_width_value=static_cast<std::int64_t>(render_x2)-render_x1;
    const std::int64_t render_height_value=static_cast<std::int64_t>(render_y2)-render_y1;
    const auto uint32_max=std::numeric_limits<std::uint32_t>::max();
    const auto int_max=std::numeric_limits<int>::max();
    if(source.row_bytes<=0||destination.row_bytes<=0
        ||static_cast<std::uint64_t>(source.row_bytes)>uint32_max
        ||static_cast<std::uint64_t>(destination.row_bytes)>uint32_max
        ||source_width_value<=0||source_width_value>int_max
        ||source_height_value<=0||source_height_value>uint32_max
        ||render_width_value<=0||render_width_value>int_max
        ||render_height_value<=0||render_height_value>uint32_max) {
        error="FilmViz Metal frame dimensions exceed the GPU parameter layout"; return false;
    }
    const std::uint32_t source_width=static_cast<std::uint32_t>(source_width_value);
    const std::uint32_t source_height=static_cast<std::uint32_t>(source_height_value);
    const std::uint32_t render_width=static_cast<std::uint32_t>(render_width_value);
    const std::uint32_t render_height=static_cast<std::uint32_t>(render_height_value);
    const std::size_t granularity_count=impl_->data.negative_granularity_samples.size()/4u;
    if(granularity_count>uint32_max) {
        error="FilmViz granularity table exceeds the GPU parameter layout"; return false;
    }
    FilmVizDirectParams p;
    p.spectral_count=impl_->data.spectral_count; p.rgb2spec_resolution=impl_->data.rgb2spec_resolution;
    p.rgb2spec_forward_count=impl_->data.rgb2spec_forward_count;
    p.input_profile=1; p.output_profile=settings.output_profile;
    p.source_x1=source.x1; p.source_y1=source.y1; p.source_x2=source.x2; p.source_y2=source.y2;
    p.destination_x1=destination.x1; p.destination_y1=destination.y1; p.destination_x2=destination.x2; p.destination_y2=destination.y2;
    p.render_x1=render_x1; p.render_y1=render_y1; p.render_x2=render_x2; p.render_y2=render_y2;
    p.source_row_bytes=static_cast<std::uint32_t>(source.row_bytes);
    p.destination_row_bytes=static_cast<std::uint32_t>(destination.row_bytes);
    p.exposure_stops=settings.exposure_stops; p.negative_flash_percent=settings.negative_flash_percent;
    p.print_flash_percent=settings.print_flash_percent; p.push_pull_stops=settings.push_pull_stops;
    p.color_density=settings.color_density; p.color_depth=settings.color_depth;
    p.response_response_amount=settings.color_response.response_amount;
    p.response_chroma_compression=settings.color_response.chroma_compression;
    p.response_chroma_knee=settings.color_response.chroma_knee;
    p.response_density_center=settings.color_response.density_center;
    p.response_density_width=settings.color_response.density_width;
    p.response_warm_protection=settings.color_response.warm_protection;
    p.response_warm_hue_center=settings.color_response.warm_hue_center;
    p.response_warm_hue_width=settings.color_response.warm_hue_width;
    p.response_warm_hue_shift=settings.color_response.warm_hue_shift;

    p.negative_bleach_bypass=settings.negative_bleach_bypass; p.print_bleach_bypass=settings.print_bleach_bypass;
    p.printer_light_red=settings.printer_light_red; p.printer_light_green=settings.printer_light_green;
    p.printer_light_blue=settings.printer_light_blue; p.printer_light_master=settings.printer_light_master;
    p.middle_gray=settings.middle_gray; p.printer_temperature=settings.printer_temperature;
    p.wavelength_min_nm=impl_->data.wavelength_min_nm; p.wavelength_step_nm=impl_->data.wavelength_step_nm;
    p.granularity_count=static_cast<std::uint32_t>(granularity_count);
    p.frame_seed=settings.grain_seed^static_cast<std::uint32_t>(std::llround(time*1000.0));
    p.grain_enabled=settings.grain_enabled?1u:0u; p.negative_grain=settings.negative_grain;
    p.print_grain=settings.print_grain; p.grain_size=GranularityModel::grain_size_pixels(settings.grain_size, static_cast<int>(source_width), settings.image_width_mm); p.grain_chroma=settings.grain_chroma;
    const auto grain_texture=GranularityModel::texture(p.grain_size, static_cast<float>(source_width)/settings.image_width_mm);
    p.grain_tonal_enabled=settings.grain_tonal_enabled?1u:0u;
    p.grain_shadows=settings.grain_shadows;
    p.grain_midtones=settings.grain_midtones;
    p.grain_highlights=settings.grain_highlights;
    p.reserved_grain_float[0]=grain_texture.aperture_pixels;
    p.reserved_grain_float[1]=0.0f;
    p.granularity_density_min=impl_->data.granularity_density_min; p.granularity_density_max=impl_->data.granularity_density_max;
    auto copy3=[](float* dst,const std::array<float,3>& src){ for(int i=0;i<3;++i) dst[i]=src[i]; };
    copy3(p.reference_negative_exposure,impl_->data.reference_negative_exposure);
    copy3(p.reference_negative_density,impl_->data.reference_negative_density);
    copy3(p.minimum_negative_coordinate,impl_->data.minimum_negative_coordinate);
    copy3(p.neutral_negative_increment,impl_->data.neutral_negative_increment);
    copy3(p.calibration_zero_target,impl_->data.calibration_zero_target);
    copy3(p.calibration_zero_measured,impl_->data.calibration_zero_measured);
    copy3(p.calibration_minimum_status_m,impl_->data.calibration_minimum_status_m);
    copy3(p.print_target_log_exposure,impl_->data.print_target_log_exposure);
    for(int i=0;i<3;++i) { p.calibration_jacobian_0[i]=impl_->data.calibration_zero_jacobian[i]; p.calibration_jacobian_1[i]=impl_->data.calibration_zero_jacobian[3+i]; p.calibration_jacobian_2[i]=impl_->data.calibration_zero_jacobian[6+i]; }
    const auto& n=impl_->data.negative_characteristic; const auto& q=impl_->data.print_characteristic;
    p.curve_negative_01[0]=n[0].offset; p.curve_negative_01[1]=n[0].count; p.curve_negative_01[2]=n[1].offset; p.curve_negative_01[3]=n[1].count;
    p.curve_negative_2_print_0[0]=n[2].offset; p.curve_negative_2_print_0[1]=n[2].count; p.curve_negative_2_print_0[2]=q[0].offset; p.curve_negative_2_print_0[3]=q[0].count;
    p.curve_print_12[0]=q[1].offset; p.curve_print_12[1]=q[1].count; p.curve_print_12[2]=q[2].offset; p.curve_print_12[3]=q[2].count;
    const bool use_halation=settings.halation_enabled&&settings.halation_strength>0.0f&&settings.halation_radius>0.0f;
    const bool use_mtf=settings.negative_mtf_amount>0.0f||settings.print_mtf_amount>0.0f;
    const bool use_grain=settings.grain_enabled&&(settings.negative_grain>0.0f||settings.print_grain>0.0f);
    if(use_mtf&&!impl_->spatial_response_valid) { error="FilmViz measured MTF response is unavailable"; return false; }
    id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)command_queue;
    id<MTLDevice> device=queue.device;
    // OCIO evaluates the selected config transform before the spectral GPU pass.
    id<MTLBuffer> input_buffer=(__bridge id<MTLBuffer>)source.buffer;
    id<MTLBuffer> converted=[device newBufferWithLength:input_buffer.length options:MTLResourceStorageModeShared];
    if(!converted) { error="could not allocate OCIO input buffer"; return false; }
    id<MTLCommandBuffer> download=[queue commandBuffer];
    id<MTLBlitCommandEncoder> blit=[download blitCommandEncoder];
    [blit copyFromBuffer:input_buffer sourceOffset:0 toBuffer:converted destinationOffset:0 size:input_buffer.length];
    [blit endEncoding]; [download commit]; [download waitUntilCompleted];
    if(download.status==MTLCommandBufferStatusError) { error="could not download OCIO input buffer"; return false; }
    try {
        impl_->input_transform->apply_rgba(static_cast<float*>(converted.contents),
            static_cast<int>(source_width), static_cast<int>(source_height), source.row_bytes);
    } catch(const std::exception& exception) { error=exception.what(); return false; }
    id<MTLCommandBuffer> command=[queue commandBuffer];
    const MTLResourceOptions temporary_options=MTLResourceStorageModePrivate;
    id<MTLBuffer> prepared=converted;
    id<MTLBuffer> highlight=nil,near_a=nil,near_b=nil,far_a=nil,far_b=nil;
    FilmVizDirectSpatialParams spatial;
    if(use_halation) {
        const NSUInteger bytes=static_cast<NSUInteger>(source_width)*source_height*sizeof(float)*4u;
        prepared=[device newBufferWithLength:bytes options:temporary_options];
        highlight=[device newBufferWithLength:bytes options:temporary_options];
        near_a=[device newBufferWithLength:bytes options:temporary_options]; near_b=[device newBufferWithLength:bytes options:temporary_options];
        far_a=[device newBufferWithLength:bytes options:temporary_options]; far_b=[device newBufferWithLength:bytes options:temporary_options];
        if(!prepared||!highlight||!near_a||!near_b||!far_a||!far_b) { error="could not allocate FilmViz Metal halation buffers"; return false; }
        spatial.width=source_width; spatial.height=source_height; spatial.strength=settings.halation_strength; spatial.threshold=settings.halation_threshold;
        if(!encode_2d(command,impl_->prepare_halation,source_width,source_height,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:converted offset:0 atIndex:0]; [e setBuffer:prepared offset:0 atIndex:1]; [e setBuffer:highlight offset:0 atIndex:2];
            [e setBuffer:impl_->negative_exposure offset:0 atIndex:3]; [e setBuffer:impl_->rgb_scale offset:0 atIndex:4]; [e setBuffer:impl_->rgb_data offset:0 atIndex:5]; [e setBuffer:impl_->rgb_forward offset:0 atIndex:6];
            [e setBytes:&p length:sizeof(p) atIndex:7]; [e setBytes:&spatial length:sizeof(spatial) atIndex:8];
        })) { error="could not encode FilmViz Metal halation preparation"; return false; }
        auto blur_three=[&](id<MTLBuffer> initial,id<MTLBuffer> a,id<MTLBuffer> b,int radius) {
            spatial.radius=static_cast<std::uint32_t>(radius);
            for(int pass=0;pass<3;++pass) {
                spatial.horizontal=1; id<MTLBuffer> input=pass==0?initial:a;
                if(!encode_2d(command,impl_->box_blur,source_width,source_height,[&](id<MTLComputeCommandEncoder> e){ [e setBuffer:input offset:0 atIndex:0]; [e setBuffer:b offset:0 atIndex:1]; [e setBytes:&spatial length:sizeof(spatial) atIndex:2]; })) return false;
                spatial.horizontal=0;
                if(!encode_2d(command,impl_->box_blur,source_width,source_height,[&](id<MTLComputeCommandEncoder> e){ [e setBuffer:b offset:0 atIndex:0]; [e setBuffer:a offset:0 atIndex:1]; [e setBytes:&spatial length:sizeof(spatial) atIndex:2]; })) return false;
            }
            return true;
        };
        if(!blur_three(highlight,near_a,near_b,filmviz_halation_box_radius(settings.halation_radius))
            ||!blur_three(highlight,far_a,far_b,filmviz_halation_box_radius(settings.halation_radius*2.2f))) { error="could not encode FilmViz Metal halation blur"; return false; }
        if(!encode_2d(command,impl_->combine_halation,source_width,source_height,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:prepared offset:0 atIndex:0]; [e setBuffer:highlight offset:0 atIndex:1]; [e setBuffer:near_a offset:0 atIndex:2]; [e setBuffer:far_a offset:0 atIndex:3]; [e setBytes:&spatial length:sizeof(spatial) atIndex:4];
        })) { error="could not encode FilmViz Metal halation combine"; return false; }
        p.reserved_header[0]=1u;
    }
    id<MTLBuffer> dense_a=nil,dense_b=nil;
    const NSUInteger residual_bytes=use_grain?static_cast<NSUInteger>(render_width)*render_height*sizeof(float)*4u:sizeof(float)*4u;
    id<MTLBuffer> negative_residual=[device newBufferWithLength:residual_bytes options:temporary_options];
    id<MTLBuffer> print_residual=[device newBufferWithLength:residual_bytes options:temporary_options];
    if(!negative_residual||!print_residual) { error="could not allocate FilmViz grain residual buffers"; return false; }
    FilmVizDirectParams direct_p=p;
    id<MTLBuffer> direct_destination=(__bridge id<MTLBuffer>)destination.buffer;
    if(use_mtf||use_grain) {
        const NSUInteger bytes=static_cast<NSUInteger>(render_width)*render_height*sizeof(float)*4u;
        const std::uint64_t dense_row_bytes=static_cast<std::uint64_t>(render_width)*sizeof(float)*4u;
        if(dense_row_bytes>uint32_max) { error="FilmViz Metal render row exceeds the GPU parameter layout"; return false; }
        dense_a=[device newBufferWithLength:bytes options:temporary_options]; dense_b=[device newBufferWithLength:bytes options:temporary_options];
        if(!dense_a||!dense_b) { error="could not allocate FilmViz Metal MTF buffers"; return false; }
        direct_destination=dense_a; direct_p.destination_x1=render_x1; direct_p.destination_y1=render_y1;
        direct_p.destination_x2=render_x2; direct_p.destination_y2=render_y2;
        direct_p.destination_row_bytes=static_cast<std::uint32_t>(dense_row_bytes);
    }
    if(!encode_2d(command,impl_->pipeline,render_width,render_height,[&](id<MTLComputeCommandEncoder> e){
        [e setBuffer:converted offset:0 atIndex:0]; [e setBuffer:direct_destination offset:0 atIndex:1];
        [e setBuffer:impl_->negative_exposure offset:0 atIndex:2]; [e setBuffer:impl_->negative_density offset:0 atIndex:3]; [e setBuffer:impl_->status_m offset:0 atIndex:4]; [e setBuffer:impl_->print_exposure offset:0 atIndex:5];
        [e setBuffer:impl_->print_density offset:0 atIndex:6]; [e setBuffer:impl_->viewer offset:0 atIndex:7]; [e setBuffer:impl_->curves offset:0 atIndex:8]; [e setBuffer:impl_->rgb_scale offset:0 atIndex:9];
        [e setBuffer:impl_->rgb_data offset:0 atIndex:10]; [e setBuffer:impl_->rgb_forward offset:0 atIndex:11]; [e setBuffer:impl_->negative_granularity offset:0 atIndex:12]; [e setBuffer:impl_->print_granularity offset:0 atIndex:13];
        [e setBytes:&direct_p length:sizeof(direct_p) atIndex:14]; [e setBuffer:prepared offset:0 atIndex:15];
        [e setBuffer:negative_residual offset:0 atIndex:16]; [e setBuffer:print_residual offset:0 atIndex:17];
    })) { error="could not encode FilmViz direct Metal render"; return false; }
    const auto filter=[&](id<MTLBuffer> input,float negative_amount,float print_amount,bool gamma24,bool signed_residual) {
        SpatialResponseModel::Settings settings_mtf;
        settings_mtf.image_width_mm=settings.image_width_mm;
        settings_mtf.negative_amount=negative_amount; settings_mtf.print_amount=print_amount;
        settings_mtf.sampling_width_pixels=static_cast<int>(source_width);
        std::array<std::vector<float>,3> channel_weights;
        if(!impl_->spatial_response.kernels(static_cast<int>(render_width),settings_mtf,channel_weights)) return false;
        std::vector<float> weights;
        for(const auto& channel:channel_weights) weights.insert(weights.end(),channel.begin(),channel.end());
        id<MTLBuffer> weight_buffer=make_buffer(device,weights);
        if(!weight_buffer) return false;
        spatial.width=render_width; spatial.height=render_height;
        spatial.radius=static_cast<std::uint32_t>(channel_weights[0].size()/2u);
        spatial.gamma24=gamma24?1u:0u; spatial.reserved[0]=signed_residual?1u:0u;
        spatial.horizontal=1;
        if(!encode_2d(command,impl_->mtf,render_width,render_height,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:input offset:0 atIndex:0]; [e setBuffer:dense_b offset:0 atIndex:1];
            [e setBuffer:weight_buffer offset:0 atIndex:2]; [e setBytes:&spatial length:sizeof(spatial) atIndex:3];
        })) return false;
        spatial.horizontal=0;
        return encode_2d(command,impl_->mtf,render_width,render_height,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:dense_b offset:0 atIndex:0]; [e setBuffer:input offset:0 atIndex:1];
            [e setBuffer:weight_buffer offset:0 atIndex:2]; [e setBytes:&spatial length:sizeof(spatial) atIndex:3];
        });
    };
    if(use_mtf&&!filter(dense_a,settings.negative_mtf_amount,settings.print_mtf_amount,settings.output_profile==1,use_grain)) {
        error="could not encode FilmViz image MTF"; return false;
    }
    if(use_grain&&settings.print_mtf_amount>0.0f&&!filter(negative_residual,0.0f,settings.print_mtf_amount,false,true)) {
        error="could not encode FilmViz negative grain MTF"; return false;
    }
    if(use_mtf||use_grain) {
        if(!encode_2d(command,impl_->copy_dense,render_width,render_height,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:dense_a offset:0 atIndex:0]; [e setBuffer:(__bridge id<MTLBuffer>)destination.buffer offset:0 atIndex:1];
            [e setBytes:&p length:sizeof(p) atIndex:2];
            [e setBuffer:negative_residual offset:0 atIndex:3]; [e setBuffer:print_residual offset:0 atIndex:4];
        })) { error="could not encode FilmViz grain composite"; return false; }
    }
    if (settings.output_profile > 1) {
        // Convert only the rendered rectangle, preserving alpha and untouched pixels.
        id<MTLBuffer> output=(__bridge id<MTLBuffer>)destination.buffer;
        const NSUInteger row_bytes=static_cast<NSUInteger>(render_width)*4u*sizeof(float);
        id<MTLBuffer> staging=[device newBufferWithLength:row_bytes*render_height options:MTLResourceStorageModeShared];
        if(!staging) { error="could not allocate OCIO output buffer"; return false; }
        id<MTLBlitCommandEncoder> download=[command blitCommandEncoder];
        for(NSUInteger y=0;y<render_height;++y) {
            const NSUInteger offset=(render_y1-destination.y1+y)*destination.row_bytes
                +(render_x1-destination.x1)*4u*sizeof(float);
            [download copyFromBuffer:output sourceOffset:offset toBuffer:staging destinationOffset:y*row_bytes size:row_bytes];
        }
        [download endEncoding]; [command commit]; [command waitUntilCompleted];
        if(command.status==MTLCommandBufferStatusError) { error="could not download OCIO output"; return false; }
        try {
            impl_->output_transform->apply(static_cast<float*>(staging.contents),
                render_width,render_height,4,row_bytes);
        } catch(const std::exception& exception) { error=exception.what(); return false; }
        id<MTLCommandBuffer> upload=[queue commandBuffer];
        id<MTLBlitCommandEncoder> blit=[upload blitCommandEncoder];
        for(NSUInteger y=0;y<render_height;++y) {
            const NSUInteger offset=(render_y1-destination.y1+y)*destination.row_bytes
                +(render_x1-destination.x1)*4u*sizeof(float);
            [blit copyFromBuffer:staging sourceOffset:y*row_bytes toBuffer:output destinationOffset:offset size:row_bytes];
        }
        [blit endEncoding]; [upload commit];
    } else {
        [command commit];
    }
    return true;
}

bool FilmVizDirectMetalProcessor::render_cpu_bridge(
    FilmVizOfxProcessor& cpu_processor, void* command_queue,
    const FilmVizOfxMetalFrame& source, const FilmVizOfxMetalFrame& destination,
    int render_x1,int render_y1,int render_x2,int render_y2,double time,
    const FilmVizOfxProcessor::Abort& abort,std::string& error)
{
    error.clear();
    if(!command_queue||!source.buffer||!destination.buffer) {
        error="invalid FilmViz Metal CPU bridge"; return false;
    }
    id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)command_queue;
    id<MTLBuffer> source_buffer=(__bridge id<MTLBuffer>)source.buffer;
    id<MTLBuffer> destination_buffer=(__bridge id<MTLBuffer>)destination.buffer;
    id<MTLDevice> device=queue.device;
    id<MTLBuffer> source_staging=[device newBufferWithLength:source_buffer.length options:MTLResourceStorageModeShared];
    id<MTLBuffer> destination_staging=[device newBufferWithLength:destination_buffer.length options:MTLResourceStorageModeShared];
    if(!source_staging||!destination_staging) { error="could not allocate FilmViz CPU bridge buffers"; return false; }
    id<MTLCommandBuffer> download=[queue commandBuffer];
    id<MTLBlitCommandEncoder> blit=[download blitCommandEncoder];
    [blit copyFromBuffer:source_buffer sourceOffset:0 toBuffer:source_staging destinationOffset:0 size:source_buffer.length];
    [blit copyFromBuffer:destination_buffer sourceOffset:0 toBuffer:destination_staging destinationOffset:0 size:destination_buffer.length];
    [blit endEncoding]; [download commit]; [download waitUntilCompleted];
    FilmVizOfxFrame cpu_source;
    cpu_source.x1=source.x1; cpu_source.y1=source.y1; cpu_source.x2=source.x2; cpu_source.y2=source.y2;
    cpu_source.row_bytes=source.row_bytes; cpu_source.data=static_cast<float*>(source_staging.contents);
    FilmVizOfxFrame cpu_destination;
    cpu_destination.x1=destination.x1; cpu_destination.y1=destination.y1;
    cpu_destination.x2=destination.x2; cpu_destination.y2=destination.y2;
    cpu_destination.row_bytes=destination.row_bytes; cpu_destination.data=static_cast<float*>(destination_staging.contents);
    if(!cpu_processor.render(cpu_source,cpu_destination,render_x1,render_y1,render_x2,render_y2,time,abort,error)) return false;
    id<MTLCommandBuffer> upload=[queue commandBuffer];
    id<MTLBlitCommandEncoder> upload_blit=[upload blitCommandEncoder];
    [upload_blit copyFromBuffer:destination_staging sourceOffset:0 toBuffer:destination_buffer destinationOffset:0 size:destination_buffer.length];
    [upload_blit endEncoding]; [upload commit];
    return true;
}
