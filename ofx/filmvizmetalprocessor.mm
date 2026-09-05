// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizmetalprocessor.h"
#include "filmvizofxlog.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{

struct alignas(16) MetalParams
{
    std::uint32_t lut_size = 0;
    std::uint32_t input_profile = 0;
    std::uint32_t output_profile = 1;
    std::uint32_t frame_seed = 0;

    std::int32_t source_x1 = 0;
    std::int32_t source_y1 = 0;
    std::int32_t source_x2 = 0;
    std::int32_t source_y2 = 0;

    std::int32_t destination_x1 = 0;
    std::int32_t destination_y1 = 0;
    std::int32_t destination_x2 = 0;
    std::int32_t destination_y2 = 0;

    std::int32_t render_x1 = 0;
    std::int32_t render_y1 = 0;
    std::int32_t render_x2 = 0;
    std::int32_t render_y2 = 0;

    std::uint32_t source_row_bytes = 0;
    std::uint32_t destination_row_bytes = 0;
    std::uint32_t grain_enabled = 0;
    std::uint32_t reserved0 = 0;

    float negative_grain = 0.0f;
    float print_grain = 0.0f;
    float grain_size = 1.0f;
    float grain_chroma = 1.0f;

    float halation_strength = 0.0f;
    float halation_threshold = 0.7f;
    float exposure_stops = 0.0f;
    float reserved2 = 0.0f;

    float halation_log_min[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float halation_log_max[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float record_scatter[4] = {0.22f, 0.06f, 0.015f, 0.0f};
};

static_assert(sizeof(MetalParams) == 160,
    "MetalParams must match the Metal shader constant-buffer layout");

static const char* kMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct MetalParams
{
    uint lut_size;
    uint input_profile;
    uint output_profile;
    uint frame_seed;

    int source_x1;
    int source_y1;
    int source_x2;
    int source_y2;

    int destination_x1;
    int destination_y1;
    int destination_x2;
    int destination_y2;

    int render_x1;
    int render_y1;
    int render_x2;
    int render_y2;

    uint source_row_bytes;
    uint destination_row_bytes;
    uint grain_enabled;
    uint reserved0;

    float negative_grain;
    float print_grain;
    float grain_size;
    float grain_chroma;

    float halation_strength;
    float halation_threshold;
    float exposure_stops;
    float reserved2;

    float4 halation_log_min;
    float4 halation_log_max;
    float4 record_scatter;
};

inline uint lut_index(uint r, uint g, uint b, uint size)
{
    return (b * size + g) * size + r;
}

inline float3 tetrahedral_sample(
    device const float4* lut,
    uint size,
    float3 input)
{
    const float scale = float(size - 1u);
    const float3 p = clamp(input, 0.0f, 1.0f) * scale;
    uint3 lo = uint3(floor(p));
    lo = min(lo, uint3(size - 2u));
    const uint3 hi = lo + 1u;
    const float3 f = p - float3(lo);

    const float3 c000 = lut[lut_index(lo.x, lo.y, lo.z, size)].xyz;
    const float3 c100 = lut[lut_index(hi.x, lo.y, lo.z, size)].xyz;
    const float3 c010 = lut[lut_index(lo.x, hi.y, lo.z, size)].xyz;
    const float3 c001 = lut[lut_index(lo.x, lo.y, hi.z, size)].xyz;
    const float3 c110 = lut[lut_index(hi.x, hi.y, lo.z, size)].xyz;
    const float3 c101 = lut[lut_index(hi.x, lo.y, hi.z, size)].xyz;
    const float3 c011 = lut[lut_index(lo.x, hi.y, hi.z, size)].xyz;
    const float3 c111 = lut[lut_index(hi.x, hi.y, hi.z, size)].xyz;

    if (f.x >= f.y) {
        if (f.y >= f.z) {
            return c000
                + f.x * (c100 - c000)
                + f.y * (c110 - c100)
                + f.z * (c111 - c110);
        }
        if (f.x >= f.z) {
            return c000
                + f.x * (c100 - c000)
                + f.z * (c101 - c100)
                + f.y * (c111 - c101);
        }
        return c000
            + f.z * (c001 - c000)
            + f.x * (c101 - c001)
            + f.y * (c111 - c101);
    }

    if (f.x >= f.z) {
        return c000
            + f.y * (c010 - c000)
            + f.x * (c110 - c010)
            + f.z * (c111 - c110);
    }
    if (f.y >= f.z) {
        return c000
            + f.y * (c010 - c000)
            + f.z * (c011 - c010)
            + f.x * (c111 - c011);
    }
    return c000
        + f.z * (c001 - c000)
        + f.y * (c011 - c001)
        + f.x * (c111 - c011);
}

inline float3 trilinear_sample(
    device const float4* lut,
    uint size,
    float3 input)
{
    const float scale = float(size - 1u);
    const float3 p = clamp(input, 0.0f, 1.0f) * scale;
    uint3 lo = uint3(floor(p));
    lo = min(lo, uint3(size - 2u));
    const uint3 hi = lo + 1u;
    const float3 f = p - float3(lo);

    const float3 c000 = lut[lut_index(lo.x, lo.y, lo.z, size)].xyz;
    const float3 c100 = lut[lut_index(hi.x, lo.y, lo.z, size)].xyz;
    const float3 c010 = lut[lut_index(lo.x, hi.y, lo.z, size)].xyz;
    const float3 c110 = lut[lut_index(hi.x, hi.y, lo.z, size)].xyz;
    const float3 c001 = lut[lut_index(lo.x, lo.y, hi.z, size)].xyz;
    const float3 c101 = lut[lut_index(hi.x, lo.y, hi.z, size)].xyz;
    const float3 c011 = lut[lut_index(lo.x, hi.y, hi.z, size)].xyz;
    const float3 c111 = lut[lut_index(hi.x, hi.y, hi.z, size)].xyz;

    const float3 c00 = mix(c000, c100, f.x);
    const float3 c10 = mix(c010, c110, f.x);
    const float3 c01 = mix(c001, c101, f.x);
    const float3 c11 = mix(c011, c111, f.x);
    return mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z);
}

inline uint mix_bits(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

inline float uniform_open(uint value)
{
    return (float(mix_bits(value) & 0x00ffffffu) + 0.5f)
        / 16777216.0f;
}

inline float normal_sample(uint seed, int x, int y, int stage, int channel)
{
    uint key = seed;
    key ^= mix_bits(uint(x) + 0x9e3779b9u);
    key ^= mix_bits(uint(y) + 0x85ebca6bu);
    key ^= mix_bits(uint(stage) * 0xc2b2ae35u + uint(channel));

    const float u1 = max(uniform_open(key), 1e-7f);
    const float u2 = uniform_open(key ^ 0x68bc21ebu);
    return sqrt(-2.0f * log(u1)) * cos(6.2831853071795864769f * u2);
}

inline float spatial_normal(
    uint seed,
    int x,
    int y,
    int stage,
    int channel,
    float size_pixels)
{
    const float scale = max(1.0f, size_pixels);
    const float px = float(x) / scale;
    const float py = float(y) / scale;
    const int x0 = int(floor(px));
    const int y0 = int(floor(py));
    const float tx = px - float(x0);
    const float ty = py - float(y0);
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);

    const float4 weights = float4(
        (1.0f - sx) * (1.0f - sy),
        sx * (1.0f - sy),
        (1.0f - sx) * sy,
        sx * sy);

    const float4 samples = float4(
        normal_sample(seed, x0, y0, stage, channel),
        normal_sample(seed, x0 + 1, y0, stage, channel),
        normal_sample(seed, x0, y0 + 1, stage, channel),
        normal_sample(seed, x0 + 1, y0 + 1, stage, channel));

    const float value = dot(weights, samples);
    const float variance = dot(weights, weights);
    return variance > 1e-10f ? value / sqrt(variance) : value;
}

inline float3 apply_grain(
    float3 converted,
    float3 lookup_input,
    int x,
    int y,
    constant MetalParams& p,
    device const float4* grain_negative,
    device const float4* grain_print)
{
    if (p.grain_enabled == 0u
        || (p.negative_grain <= 0.0f && p.print_grain <= 0.0f)) {
        return converted;
    }

    const float3 sigma_negative =
        trilinear_sample(grain_negative, p.lut_size, lookup_input);
    const float3 sigma_print =
        trilinear_sample(grain_print, p.lut_size, lookup_input);

    float3 density_noise;
    for (int channel = 0; channel < 3; ++channel) {
        const float negative_noise =
            p.negative_grain
            * sigma_negative[channel]
            * spatial_normal(
                p.frame_seed,
                x,
                y,
                0,
                channel,
                p.grain_size);

        const float print_noise =
            p.print_grain
            * sigma_print[channel]
            * spatial_normal(
                p.frame_seed,
                x,
                y,
                1,
                channel,
                p.grain_size);

        density_noise[channel] = negative_noise - print_noise;
    }

    const float neutral =
        dot(float3(0.2126f, 0.7152f, 0.0722f), density_noise);
    density_noise = neutral + p.grain_chroma * (density_noise - neutral);

    float3 linear =
        p.output_profile == 1u
            ? pow(max(converted, 0.0f), float3(2.4f))
            : converted;

    linear *= pow(float3(10.0f), density_noise);

    const float3 encoded =
        p.output_profile == 1u
            ? pow(max(linear, 0.0f), float3(1.0f / 2.4f))
            : linear;

    return clamp(encoded, 0.0f, 1.0f);
}

inline float3 logc3_to_awg3_linear(float3 encoded)
{
    const float cut = 0.010591f;
    const float a = 5.555556f;
    const float b = 0.052272f;
    const float c = 0.247190f;
    const float d = 0.385537f;
    const float e = 5.367655f;
    const float f = 0.092809f;
    const float encoded_cut = e * cut + f;

    float3 linear;
    for (int channel = 0; channel < 3; ++channel) {
        const float y = encoded[channel];
        const float x =
            y > encoded_cut
                ? (pow(10.0f, (y - d) / c) - b) / a
                : (y - f) / e;
        linear[channel] = max(0.0f, x);
    }

    return linear;
}

inline float3 awg3_linear_to_logc3(float3 linear)
{
    const float cut = 0.010591f;
    const float a = 5.555556f;
    const float b = 0.052272f;
    const float c = 0.247190f;
    const float d = 0.385537f;
    const float e = 5.367655f;
    const float f = 0.092809f;

    float3 encoded;
    for (int channel = 0; channel < 3; ++channel) {
        const float x = max(0.0f, linear[channel]);
        encoded[channel] =
            x > cut
                ? c * log10(a * x + b) + d
                : e * x + f;
    }

    return encoded;
}

inline float3 logc3_to_ap0(float3 encoded)
{
    const float3 linear = logc3_to_awg3_linear(encoded);

    // AWG3 linear -> ACES2065-1/AP0, including D65 -> D60 adaptation.
    return float3(
        0.6803455111f * linear.x + 0.2346762511f * linear.y + 0.0849783114f * linear.z,
        0.0857666276f * linear.x + 1.0154255663f * linear.y - 0.1011922071f * linear.z,
        0.0021230984f * linear.x - 0.0582098498f * linear.y + 1.0560869795f * linear.z);
}

inline float4 read_pixel(
    device const uchar* bytes,
    uint row_bytes,
    int x,
    int y,
    int x1,
    int y1)
{
    device const float4* row =
        reinterpret_cast<device const float4*>(
            bytes + uint(y - y1) * row_bytes);
    return row[x - x1];
}

inline void write_pixel(
    device uchar* bytes,
    uint row_bytes,
    int x,
    int y,
    int x1,
    int y1,
    float4 value)
{
    device float4* row =
        reinterpret_cast<device float4*>(
            bytes + uint(y - y1) * row_bytes);
    row[x - x1] = value;
}

kernel void filmviz_color_grain(
    device const uchar* source [[buffer(0)]],
    device uchar* destination [[buffer(1)]],
    device const float4* exposure_lut [[buffer(2)]],
    device const float4* color_lut [[buffer(3)]],
    device const float4* grain_negative [[buffer(4)]],
    device const float4* grain_print [[buffer(5)]],
    constant MetalParams& p [[buffer(6)]],
    uint2 gid [[thread_position_in_grid]])
{
    const int x = p.render_x1 + int(gid.x);
    const int y = p.render_y1 + int(gid.y);
    if (x >= p.render_x2 || y >= p.render_y2) {
        return;
    }

    const float4 src = read_pixel(
        source,
        p.source_row_bytes,
        x,
        y,
        p.source_x1,
        p.source_y1);

    // Exposure is applied at the same raw-negative-exposure boundary used by
    // FilmPipeline. It therefore remains live without rebuilding either LUT.
    const float exposure_scale = exp2(p.exposure_stops);
    const float3 exposure =
        tetrahedral_sample(exposure_lut, p.lut_size, src.xyz)
        * exposure_scale;

    const float3 log_exposure =
        log10(max(exposure, float3(1e-20f)));
    const float3 lookup = clamp(
        (log_exposure - p.halation_log_min.xyz)
        / (p.halation_log_max.xyz - p.halation_log_min.xyz),
        0.0f,
        1.0f);

    float3 converted = tetrahedral_sample(color_lut, p.lut_size, lookup);
    converted = apply_grain(
        converted,
        lookup,
        x,
        y,
        p,
        grain_negative,
        grain_print);

    write_pixel(
        destination,
        p.destination_row_bytes,
        x,
        y,
        p.destination_x1,
        p.destination_y1,
        float4(converted, src.w));
}

kernel void filmviz_prepare_halation(
    device const uchar* source [[buffer(0)]],
    device const float4* exposure_lut [[buffer(1)]],
    texture2d<float, access::write> exposure_texture [[texture(0)]],
    texture2d<float, access::write> source_texture [[texture(1)]],
    constant MetalParams& p [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]])
{
    const uint width = uint(p.source_x2 - p.source_x1);
    const uint height = uint(p.source_y2 - p.source_y1);
    if (gid.x >= width || gid.y >= height) {
        return;
    }

    const int x = p.source_x1 + int(gid.x);
    const int y = p.source_y1 + int(gid.y);
    const float4 src = read_pixel(
        source,
        p.source_row_bytes,
        x,
        y,
        p.source_x1,
        p.source_y1);

    const float exposure_scale = exp2(p.exposure_stops);
    const float3 exposure =
        tetrahedral_sample(exposure_lut, p.lut_size, src.xyz)
        * exposure_scale;
    const float3 ap0 =
        (p.input_profile == 0u
            ? logc3_to_ap0(src.xyz)
            : src.xyz)
        * exposure_scale;

    const float luminance = max(
        0.0f,
        dot(float3(0.34396645f, 0.72816610f, -0.07213255f), ap0));

    float weight = 0.0f;
    if (p.halation_threshold <= 1e-8f) {
        weight = luminance;
    }
    else {
        const float onset = 0.5f * p.halation_threshold;
        if (luminance > onset) {
            const float t = clamp(
                (luminance - onset)
                / max(p.halation_threshold - onset, 1e-8f),
                0.0f,
                1.0f);
            const float smooth = t * t * (3.0f - 2.0f * t);
            weight = luminance * smooth;
        }
    }

    exposure_texture.write(float4(exposure, src.w), gid);
    source_texture.write(float4(max(exposure, 0.0f) * weight, 0.0f), gid);
}

kernel void filmviz_develop_halation(
    device const uchar* source [[buffer(0)]],
    device uchar* destination [[buffer(1)]],
    device const float4* halation_lut [[buffer(2)]],
    device const float4* grain_negative [[buffer(3)]],
    device const float4* grain_print [[buffer(4)]],
    texture2d<float, access::read> exposure_texture [[texture(0)]],
    texture2d<float, access::read> source_texture [[texture(1)]],
    texture2d<float, access::read> near_texture [[texture(2)]],
    texture2d<float, access::read> far_texture [[texture(3)]],
    constant MetalParams& p [[buffer(5)]],
    uint2 gid [[thread_position_in_grid]])
{
    const int x = p.render_x1 + int(gid.x);
    const int y = p.render_y1 + int(gid.y);
    if (x >= p.render_x2 || y >= p.render_y2) {
        return;
    }

    const int local_x = x - p.source_x1;
    const int local_y = y - p.source_y1;
    if (local_x < 0 || local_y < 0
        || local_x >= p.source_x2 - p.source_x1
        || local_y >= p.source_y2 - p.source_y1) {
        return;
    }

    const uint2 uv = uint2(local_x, local_y);
    const float4 src = read_pixel(
        source,
        p.source_row_bytes,
        x,
        y,
        p.source_x1,
        p.source_y1);

    float3 exposure = exposure_texture.read(uv).xyz;
    const float3 halo_source = source_texture.read(uv).xyz;
    const float3 near_blur = near_texture.read(uv).xyz;
    const float3 far_blur = far_texture.read(uv).xyz;

    const float3 blurred = 0.72f * near_blur + 0.28f * far_blur;
    const float3 spread = max(0.0f, blurred - 0.95f * halo_source);
    exposure += p.halation_strength * p.record_scatter.xyz * spread;

    const float3 log_exposure = log10(max(exposure, float3(1e-20f)));
    const float3 lookup = clamp(
        (log_exposure - p.halation_log_min.xyz)
        / (p.halation_log_max.xyz - p.halation_log_min.xyz),
        0.0f,
        1.0f);

    float3 converted = tetrahedral_sample(halation_lut, p.lut_size, lookup);
    converted = apply_grain(
        converted,
        lookup,
        x,
        y,
        p,
        grain_negative,
        grain_print);

    write_pixel(
        destination,
        p.destination_row_bytes,
        x,
        y,
        p.destination_x1,
        p.destination_y1,
        float4(converted, src.w));
}
)METAL";

id<MTLBuffer>
make_buffer(
    id<MTLDevice> device,
    const std::vector<float>& values)
{
    if (!device || values.empty()) {
        return nil;
    }

    return [device newBufferWithBytes:values.data()
                              length:values.size() * sizeof(float)
                             options:MTLResourceStorageModeShared];
}

MTLSize
threadgroup_size(
    id<MTLComputePipelineState> pipeline)
{
    const NSUInteger width =
        std::max<NSUInteger>(
            1,
            std::min<NSUInteger>(
                pipeline.threadExecutionWidth,
                16));

    const NSUInteger height =
        std::max<NSUInteger>(
            1,
            std::min<NSUInteger>(
                pipeline.maxTotalThreadsPerThreadgroup / width,
                16));

    return MTLSizeMake(width, height, 1);
}

void
encode_compute(
    id<MTLComputeCommandEncoder> encoder,
    id<MTLComputePipelineState> pipeline,
    NSUInteger width,
    NSUInteger height)
{
    [encoder setComputePipelineState:pipeline];
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
        threadsPerThreadgroup:threadgroup_size(pipeline)];
}

struct SharedMetalResources
{
    id<MTLBuffer> color_lut = nil;
    id<MTLBuffer> grain_negative = nil;
    id<MTLBuffer> grain_print = nil;
    id<MTLBuffer> negative_exposure_lut = nil;
    id<MTLBuffer> halation_lut = nil;
    id<MTLBuffer> halation_grain_negative = nil;
    id<MTLBuffer> halation_grain_print = nil;
    FilmVizOfxGpuSnapshot snapshot;
};

struct MetalCacheKey
{
    std::uintptr_t device = 0;
    std::uint64_t transform_hash = 0;
    bool operator==(const MetalCacheKey& other) const
    {
        return
            device == other.device
            && transform_hash == other.transform_hash;
    }
};

struct MetalCacheKeyHash
{
    std::size_t operator()(const MetalCacheKey& key) const
    {
        std::size_t value =
            static_cast<std::size_t>(key.device);
        value ^=
            static_cast<std::size_t>(key.transform_hash)
            + 0x9e3779b9u
            + (value << 6u)
            + (value >> 2u);
        return value;
    }
};

std::mutex gMetalCacheMutex;
std::unordered_map<
    MetalCacheKey,
    std::weak_ptr<SharedMetalResources>,
    MetalCacheKeyHash> gMetalCache;

} // namespace

struct FilmVizMetalProcessor::Impl
{
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = nil;
    id<MTLComputePipelineState> color_pipeline = nil;
    id<MTLComputePipelineState> prepare_halation_pipeline = nil;
    id<MTLComputePipelineState> develop_halation_pipeline = nil;

    id<MTLBuffer> color_lut = nil;
    id<MTLBuffer> grain_negative = nil;
    id<MTLBuffer> grain_print = nil;
    id<MTLBuffer> negative_exposure_lut = nil;
    id<MTLBuffer> halation_lut = nil;
    id<MTLBuffer> halation_grain_negative = nil;
    id<MTLBuffer> halation_grain_print = nil;

    FilmVizOfxGpuSnapshot snapshot;
    std::shared_ptr<SharedMetalResources> shared_resources;
    std::uint64_t uploaded_revision = 0;
};

FilmVizMetalProcessor::FilmVizMetalProcessor()
    : impl_(std::make_unique<Impl>())
{
}

FilmVizMetalProcessor::~FilmVizMetalProcessor() = default;

bool
FilmVizMetalProcessor::configure(
    FilmVizOfxProcessor& cpu_processor,
    void* command_queue,
    std::string& error)
{
    error.clear();

    FilmVizOfxLog::Scope configure_scope(
        "metal_configure",
        "cache=" + cpu_processor.transform_cache_name());

    if (!command_queue) {
        error = "Resolve did not provide a Metal command queue";
        return false;
    }

    id<MTLCommandQueue> queue =
        (__bridge id<MTLCommandQueue>)command_queue;
    id<MTLDevice> device = queue.device;

    if (!device) {
        error = "Resolve Metal command queue has no device";
        return false;
    }

    const std::uint64_t revision =
        cpu_processor.cache_revision();

    if (impl_->device != device
        || !impl_->library
        || !impl_->color_pipeline
        || !impl_->prepare_halation_pipeline
        || !impl_->develop_halation_pipeline) {

        impl_->device = device;
        impl_->uploaded_revision = 0;

        NSError* library_error = nil;
        NSString* source =
            [NSString stringWithUTF8String:kMetalSource];

        MTLCompileOptions* options =
            [[MTLCompileOptions alloc] init];
        options.fastMathEnabled = YES;

        impl_->library =
            [device newLibraryWithSource:source
                                 options:options
                                   error:&library_error];

        if (!impl_->library) {
            error =
                library_error
                    ? [[library_error localizedDescription] UTF8String]
                    : "could not compile FilmViz Metal library";
            return false;
        }

        auto make_pipeline =
            [&](NSString* name,
                id<MTLComputePipelineState>& pipeline) -> bool {
                id<MTLFunction> function =
                    [impl_->library newFunctionWithName:name];

                if (!function) {
                    error =
                        std::string("missing FilmViz Metal kernel: ")
                        + [name UTF8String];
                    return false;
                }

                NSError* pipeline_error = nil;
                pipeline =
                    [device newComputePipelineStateWithFunction:function
                                                          error:&pipeline_error];

                if (!pipeline) {
                    error =
                        pipeline_error
                            ? [[pipeline_error localizedDescription] UTF8String]
                            : "could not create FilmViz Metal pipeline";
                    return false;
                }

                return true;
            };

        if (!make_pipeline(@"filmviz_color_grain", impl_->color_pipeline)
            || !make_pipeline(@"filmviz_prepare_halation", impl_->prepare_halation_pipeline)
            || !make_pipeline(@"filmviz_develop_halation", impl_->develop_halation_pipeline)) {
            return false;
        }
    }

    if (revision == impl_->uploaded_revision
        && impl_->color_lut
        && impl_->negative_exposure_lut
        && impl_->grain_negative
        && impl_->grain_print) {
        configure_scope.finish("result=resident_hit");
        return true;
    }

    FilmVizOfxGpuSnapshot snapshot;
    if (!cpu_processor.gpu_snapshot(snapshot, error)) {
        return false;
    }

    const MetalCacheKey metal_key = {
        reinterpret_cast<std::uintptr_t>((__bridge void*)device),
        snapshot.transform_hash
    };

    {
        const std::lock_guard<std::mutex> lock(gMetalCacheMutex);
        const auto found = gMetalCache.find(metal_key);

        if (found != gMetalCache.end()) {
            std::shared_ptr<SharedMetalResources> shared =
                found->second.lock();

            if (shared) {
                impl_->shared_resources = shared;
                impl_->color_lut = shared->color_lut;
                impl_->grain_negative = shared->grain_negative;
                impl_->grain_print = shared->grain_print;
                impl_->negative_exposure_lut = shared->negative_exposure_lut;
                impl_->halation_lut = shared->halation_lut;
                impl_->halation_grain_negative = shared->halation_grain_negative;
                impl_->halation_grain_print = shared->halation_grain_print;
                impl_->snapshot = shared->snapshot;
                impl_->uploaded_revision = revision;
                configure_scope.finish("result=global_gpu_hit");
                return true;
            }
        }
    }

    impl_->color_lut = make_buffer(device, snapshot.color_lut_rgba);
    impl_->grain_negative = make_buffer(device, snapshot.grain_negative_rgba);
    impl_->grain_print = make_buffer(device, snapshot.grain_print_rgba);
    impl_->negative_exposure_lut =
        make_buffer(device, snapshot.negative_exposure_lut_rgba);
    impl_->halation_lut =
        make_buffer(device, snapshot.halation_lut_rgba);
    impl_->halation_grain_negative =
        make_buffer(device, snapshot.halation_grain_negative_rgba);
    impl_->halation_grain_print =
        make_buffer(device, snapshot.halation_grain_print_rgba);

    if (!impl_->color_lut
        || !impl_->negative_exposure_lut
        || !impl_->grain_negative
        || !impl_->grain_print) {
        error = "could not upload FilmViz Metal LUT resources";
        return false;
    }

    impl_->snapshot = std::move(snapshot);
    impl_->uploaded_revision = revision;

    auto shared =
        std::make_shared<SharedMetalResources>();
    shared->color_lut = impl_->color_lut;
    shared->grain_negative = impl_->grain_negative;
    shared->grain_print = impl_->grain_print;
    shared->negative_exposure_lut = impl_->negative_exposure_lut;
    shared->halation_lut = impl_->halation_lut;
    shared->halation_grain_negative = impl_->halation_grain_negative;
    shared->halation_grain_print = impl_->halation_grain_print;
    shared->snapshot = impl_->snapshot;
    impl_->shared_resources = shared;

    {
        const std::lock_guard<std::mutex> lock(gMetalCacheMutex);
        gMetalCache[metal_key] = shared;
    }

    configure_scope.finish("result=uploaded_shared");
    return true;
}

bool
FilmVizMetalProcessor::render(
    const FilmVizOfxRenderSettings& settings,
    void* command_queue,
    const FilmVizOfxMetalFrame& source,
    const FilmVizOfxMetalFrame& destination,
    int render_x1,
    int render_y1,
    int render_x2,
    int render_y2,
    double time,
    std::string& error)
{
    error.clear();

    FilmVizOfxLog::Scope render_scope(
        "metal_encode",
        std::string("time=") + std::to_string(time)
            + " exposure=" + std::to_string(settings.exposure_stops)
            + " grain=" + (settings.grain_enabled ? "1" : "0")
            + " halation=" + (settings.halation_enabled ? "1" : "0"));

    if (!command_queue
        || !source.buffer
        || !destination.buffer
        || !impl_->device
        || !impl_->color_pipeline
        || !impl_->color_lut
        || !impl_->negative_exposure_lut) {

        error = "FilmViz Metal processor is not configured";
        return false;
    }

    render_x1 = std::max(render_x1, destination.x1);
    render_y1 = std::max(render_y1, destination.y1);
    render_x2 = std::min(render_x2, destination.x2);
    render_y2 = std::min(render_y2, destination.y2);

    if (render_x1 >= render_x2
        || render_y1 >= render_y2) {
        return true;
    }

    id<MTLCommandQueue> queue =
        (__bridge id<MTLCommandQueue>)command_queue;
    id<MTLBuffer> source_buffer =
        (__bridge id<MTLBuffer>)source.buffer;
    id<MTLBuffer> destination_buffer =
        (__bridge id<MTLBuffer>)destination.buffer;

    MetalParams params;
    params.lut_size = static_cast<std::uint32_t>(settings.lut_size);
    params.input_profile = static_cast<std::uint32_t>(settings.input_profile);
    params.output_profile = static_cast<std::uint32_t>(settings.output_profile);
    params.frame_seed =
        settings.grain_seed
        ^ static_cast<std::uint32_t>(
            std::llround(time * 1000.0));

    params.source_x1 = source.x1;
    params.source_y1 = source.y1;
    params.source_x2 = source.x2;
    params.source_y2 = source.y2;
    params.destination_x1 = destination.x1;
    params.destination_y1 = destination.y1;
    params.destination_x2 = destination.x2;
    params.destination_y2 = destination.y2;
    params.render_x1 = render_x1;
    params.render_y1 = render_y1;
    params.render_x2 = render_x2;
    params.render_y2 = render_y2;
    params.source_row_bytes = static_cast<std::uint32_t>(source.row_bytes);
    params.destination_row_bytes = static_cast<std::uint32_t>(destination.row_bytes);
    params.grain_enabled = settings.grain_enabled ? 1u : 0u;
    params.negative_grain = settings.negative_grain;
    params.print_grain = settings.print_grain;
    params.grain_size = settings.grain_size;
    params.grain_chroma = settings.grain_chroma;
    params.halation_strength = settings.halation_strength;
    params.halation_threshold = settings.halation_threshold;
    params.exposure_stops = settings.exposure_stops;

    for (int channel = 0; channel < 3; ++channel) {
        params.halation_log_min[channel] =
            impl_->snapshot.halation_log_min[channel];
        params.halation_log_max[channel] =
            impl_->snapshot.halation_log_max[channel];
    }

    id<MTLCommandBuffer> command_buffer =
        [queue commandBuffer];

    if (!command_buffer) {
        error = "could not create FilmViz Metal command buffer";
        return false;
    }

    const bool use_halation =
        settings.halation_enabled
        && settings.halation_strength > 0.0f
        && settings.halation_radius > 0.0f
        && impl_->snapshot.halation_available
        && impl_->negative_exposure_lut
        && impl_->halation_lut
        && impl_->halation_grain_negative
        && impl_->halation_grain_print;

    if (!use_halation) {
        id<MTLComputeCommandEncoder> encoder =
            [command_buffer computeCommandEncoder];

        [encoder setBuffer:source_buffer offset:0 atIndex:0];
        [encoder setBuffer:destination_buffer offset:0 atIndex:1];
        [encoder setBuffer:impl_->negative_exposure_lut offset:0 atIndex:2];
        [encoder setBuffer:impl_->color_lut offset:0 atIndex:3];
        [encoder setBuffer:impl_->grain_negative offset:0 atIndex:4];
        [encoder setBuffer:impl_->grain_print offset:0 atIndex:5];
        [encoder setBytes:&params length:sizeof(params) atIndex:6];

        encode_compute(
            encoder,
            impl_->color_pipeline,
            static_cast<NSUInteger>(render_x2 - render_x1),
            static_cast<NSUInteger>(render_y2 - render_y1));

        [encoder endEncoding];
        [command_buffer commit];
        return true;
    }

    const NSUInteger width =
        static_cast<NSUInteger>(source.x2 - source.x1);
    const NSUInteger height =
        static_cast<NSUInteger>(source.y2 - source.y1);

    if (width == 0 || height == 0) {
        return true;
    }

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.usage =
        MTLTextureUsageShaderRead
        | MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModePrivate;

    id<MTLTexture> exposure_texture =
        [impl_->device newTextureWithDescriptor:descriptor];
    id<MTLTexture> source_texture =
        [impl_->device newTextureWithDescriptor:descriptor];
    id<MTLTexture> near_texture =
        [impl_->device newTextureWithDescriptor:descriptor];
    id<MTLTexture> far_texture =
        [impl_->device newTextureWithDescriptor:descriptor];

    if (!exposure_texture
        || !source_texture
        || !near_texture
        || !far_texture) {

        error = "could not allocate FilmViz Metal halation textures";
        return false;
    }

    id<MTLComputeCommandEncoder> prepare =
        [command_buffer computeCommandEncoder];
    [prepare setBuffer:source_buffer offset:0 atIndex:0];
    [prepare setBuffer:impl_->negative_exposure_lut offset:0 atIndex:1];
    [prepare setTexture:exposure_texture atIndex:0];
    [prepare setTexture:source_texture atIndex:1];
    [prepare setBytes:&params length:sizeof(params) atIndex:2];
    encode_compute(
        prepare,
        impl_->prepare_halation_pipeline,
        width,
        height);
    [prepare endEncoding];

    const float near_sigma =
        std::max(
            0.5f,
            settings.halation_radius / 3.0f);
    const float far_sigma =
        std::max(
            0.5f,
            settings.halation_radius * 2.2f / 3.0f);

    MPSImageGaussianBlur* near_blur =
        [[MPSImageGaussianBlur alloc]
            initWithDevice:impl_->device
                     sigma:near_sigma];
    MPSImageGaussianBlur* far_blur =
        [[MPSImageGaussianBlur alloc]
            initWithDevice:impl_->device
                     sigma:far_sigma];

    near_blur.edgeMode = MPSImageEdgeModeClamp;
    far_blur.edgeMode = MPSImageEdgeModeClamp;

    [near_blur encodeToCommandBuffer:command_buffer
                       sourceTexture:source_texture
                  destinationTexture:near_texture];
    [far_blur encodeToCommandBuffer:command_buffer
                      sourceTexture:source_texture
                 destinationTexture:far_texture];

    id<MTLComputeCommandEncoder> develop =
        [command_buffer computeCommandEncoder];
    [develop setBuffer:source_buffer offset:0 atIndex:0];
    [develop setBuffer:destination_buffer offset:0 atIndex:1];
    [develop setBuffer:impl_->halation_lut offset:0 atIndex:2];
    [develop setBuffer:impl_->halation_grain_negative offset:0 atIndex:3];
    [develop setBuffer:impl_->halation_grain_print offset:0 atIndex:4];
    [develop setTexture:exposure_texture atIndex:0];
    [develop setTexture:source_texture atIndex:1];
    [develop setTexture:near_texture atIndex:2];
    [develop setTexture:far_texture atIndex:3];
    [develop setBytes:&params length:sizeof(params) atIndex:5];
    encode_compute(
        develop,
        impl_->develop_halation_pipeline,
        static_cast<NSUInteger>(render_x2 - render_x1),
        static_cast<NSUInteger>(render_y2 - render_y1));
    [develop endEncoding];

    [command_buffer commit];
    return true;
}

bool
FilmVizMetalProcessor::copy(
    void* command_queue,
    const FilmVizOfxMetalFrame& source,
    const FilmVizOfxMetalFrame& destination,
    std::string& error)
{
    error.clear();

    if (!command_queue || !source.buffer || !destination.buffer) {
        error = "invalid FilmViz Metal copy";
        return false;
    }

    id<MTLCommandQueue> queue =
        (__bridge id<MTLCommandQueue>)command_queue;
    id<MTLBuffer> source_buffer =
        (__bridge id<MTLBuffer>)source.buffer;
    id<MTLBuffer> destination_buffer =
        (__bridge id<MTLBuffer>)destination.buffer;

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    [blit copyFromBuffer:source_buffer
            sourceOffset:0
                toBuffer:destination_buffer
       destinationOffset:0
                    size:std::min(source_buffer.length, destination_buffer.length)];
    [blit endEncoding];
    [command_buffer commit];
    return true;
}

bool
FilmVizMetalProcessor::render_cpu_bridge(
    FilmVizOfxProcessor& cpu_processor,
    void* command_queue,
    const FilmVizOfxMetalFrame& source,
    const FilmVizOfxMetalFrame& destination,
    int render_x1,
    int render_y1,
    int render_x2,
    int render_y2,
    double time,
    const FilmVizOfxProcessor::Abort& abort,
    std::string& error)
{
    error.clear();

    if (!command_queue
        || !source.buffer
        || !destination.buffer) {
        error = "invalid FilmViz Metal CPU bridge";
        return false;
    }

    id<MTLCommandQueue> queue =
        (__bridge id<MTLCommandQueue>)command_queue;
    id<MTLBuffer> source_buffer =
        (__bridge id<MTLBuffer>)source.buffer;
    id<MTLBuffer> destination_buffer =
        (__bridge id<MTLBuffer>)destination.buffer;
    id<MTLDevice> device = queue.device;

    id<MTLBuffer> source_staging =
        [device newBufferWithLength:source_buffer.length
                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> destination_staging =
        [device newBufferWithLength:destination_buffer.length
                            options:MTLResourceStorageModeShared];

    if (!source_staging || !destination_staging) {
        error = "could not allocate FilmViz CPU bridge buffers";
        return false;
    }

    id<MTLCommandBuffer> download = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [download blitCommandEncoder];
    [blit copyFromBuffer:source_buffer
            sourceOffset:0
                toBuffer:source_staging
       destinationOffset:0
                    size:source_buffer.length];
    [blit copyFromBuffer:destination_buffer
            sourceOffset:0
                toBuffer:destination_staging
       destinationOffset:0
                    size:destination_buffer.length];
    [blit endEncoding];
    [download commit];
    [download waitUntilCompleted];

    FilmVizOfxFrame cpu_source;
    cpu_source.x1 = source.x1;
    cpu_source.y1 = source.y1;
    cpu_source.x2 = source.x2;
    cpu_source.y2 = source.y2;
    cpu_source.row_bytes = source.row_bytes;
    cpu_source.data = static_cast<float*>(source_staging.contents);

    FilmVizOfxFrame cpu_destination;
    cpu_destination.x1 = destination.x1;
    cpu_destination.y1 = destination.y1;
    cpu_destination.x2 = destination.x2;
    cpu_destination.y2 = destination.y2;
    cpu_destination.row_bytes = destination.row_bytes;
    cpu_destination.data = static_cast<float*>(destination_staging.contents);

    if (!cpu_processor.render(
            cpu_source,
            cpu_destination,
            render_x1,
            render_y1,
            render_x2,
            render_y2,
            time,
            abort,
            error)) {
        return false;
    }

    id<MTLCommandBuffer> upload = [queue commandBuffer];
    id<MTLBlitCommandEncoder> upload_blit = [upload blitCommandEncoder];
    [upload_blit copyFromBuffer:destination_staging
                   sourceOffset:0
                       toBuffer:destination_buffer
              destinationOffset:0
                           size:destination_buffer.length];
    [upload_blit endEncoding];
    [upload commit];
    return true;
}
