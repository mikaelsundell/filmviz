// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmviz_metal_preview.h"

#include "filmvizdirectmetalprocessor.h"

#include <OpenImageIO/imagebuf.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

struct FilmVizMetalPreview::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLBuffer> source = nil;
    id<MTLBuffer> destination = nil;

    FilmVizDirectMetalProcessor renderer;
    std::string input_filename;
    std::filesystem::file_time_type input_write_time;
    int max_dimension = 0;
    int width = 0;
    int height = 0;
};

namespace
{

bool
load_input(
    FilmVizMetalPreview::Impl& impl,
    const std::string& filename,
    int max_dimension,
    std::string& error)
{
    std::error_code filesystem_error;
    const auto write_time =
        std::filesystem::last_write_time(filename, filesystem_error);

    if (!filesystem_error
        && filename == impl.input_filename
        && max_dimension == impl.max_dimension
        && write_time == impl.input_write_time
        && impl.source
        && impl.destination) {

        return true;
    }

    OIIO::ImageBuf image(filename);

    if (!image.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        error = "could not read Metal preview image: " + image.geterror();
        return false;
    }

    const OIIO::ImageSpec& spec = image.spec();

    if (spec.width <= 0 || spec.height <= 0 || spec.nchannels < 3) {
        error = "Metal preview image must contain RGB channels";
        return false;
    }

    const int limit = std::max(64, max_dimension);
    const float scale =
        std::min(
            1.0f,
            static_cast<float>(limit)
                / static_cast<float>(std::max(spec.width, spec.height)));

    const int width =
        std::max(1, static_cast<int>(std::lround(spec.width * scale)));
    const int height =
        std::max(1, static_cast<int>(std::lround(spec.height * scale)));

    std::vector<float> source_pixels(
        static_cast<std::size_t>(spec.width)
        * static_cast<std::size_t>(spec.height)
        * static_cast<std::size_t>(spec.nchannels));

    if (!image.get_pixels(
            image.roi(),
            OIIO::TypeDesc::FLOAT,
            source_pixels.data())) {

        error = "could not read Metal preview pixels: " + image.geterror();
        return false;
    }

    std::vector<float> rgba(
        static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height)
        * 4u,
        1.0f);

    for (int y = 0; y < height; ++y) {
        const int source_y =
            std::min(
                spec.height - 1,
                static_cast<int>(
                    static_cast<double>(y) * spec.height / height));

        for (int x = 0; x < width; ++x) {
            const int source_x =
                std::min(
                    spec.width - 1,
                    static_cast<int>(
                        static_cast<double>(x) * spec.width / width));

            const std::size_t source_offset =
                (static_cast<std::size_t>(source_y)
                    * static_cast<std::size_t>(spec.width)
                    + static_cast<std::size_t>(source_x))
                * static_cast<std::size_t>(spec.nchannels);

            const std::size_t destination_offset =
                (static_cast<std::size_t>(y)
                    * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(x))
                * 4u;

            for (int channel = 0; channel < 3; ++channel) {
                rgba[destination_offset + static_cast<std::size_t>(channel)] =
                    source_pixels[
                        source_offset + static_cast<std::size_t>(channel)];
            }
        }
    }

    const NSUInteger bytes =
        static_cast<NSUInteger>(rgba.size() * sizeof(float));

    impl.source =
        [impl.device
            newBufferWithBytes:rgba.data()
            length:bytes
            options:MTLResourceStorageModeShared];
    impl.destination =
        [impl.device
            newBufferWithLength:bytes
            options:MTLResourceStorageModeShared];

    if (!impl.source || !impl.destination) {
        error = "could not allocate Metal preview buffers";
        return false;
    }

    impl.input_filename = filename;
    impl.input_write_time = write_time;
    impl.max_dimension = max_dimension;
    impl.width = width;
    impl.height = height;
    return true;
}

} // namespace

FilmVizMetalPreview::FilmVizMetalPreview()
    : impl_(std::make_unique<Impl>())
{
    impl_->device = MTLCreateSystemDefaultDevice();
    if (impl_->device) {
        impl_->queue = [impl_->device newCommandQueue];
    }
}

FilmVizMetalPreview::~FilmVizMetalPreview() = default;

bool
FilmVizMetalPreview::available()
{
    return MTLCreateSystemDefaultDevice() != nil;
}

void
FilmVizMetalPreview::invalidate_profiles()
{
    impl_->renderer.invalidate_profiles();
}

bool
FilmVizMetalPreview::render(
    const std::string& input_filename,
    const std::string& resources_directory,
    const FilmVizOfxRenderSettings& settings,
    int max_dimension,
    double time,
    FilmVizMetalPreviewResult& result,
    std::string& error)
{
    error.clear();
    result = {};

    if (!impl_->device || !impl_->queue) {
        error = "Metal is unavailable";
        return false;
    }

    if (!load_input(*impl_, input_filename, max_dimension, error)) {
        return false;
    }

    FilmVizOfxMetalFrame source;
    source.x2 = impl_->width;
    source.y2 = impl_->height;
    source.row_bytes =
        static_cast<std::ptrdiff_t>(impl_->width * 4 * sizeof(float));
    source.buffer = (__bridge void*)impl_->source;

    FilmVizOfxMetalFrame destination = source;
    destination.buffer = (__bridge void*)impl_->destination;

    if (!impl_->renderer.configure(
            settings,
            resources_directory,
            (__bridge void*)impl_->queue,
            error)
        || !impl_->renderer.render(
            settings,
            (__bridge void*)impl_->queue,
            source,
            destination,
            0,
            0,
            impl_->width,
            impl_->height,
            time,
            error)) {

        return false;
    }

    id<MTLCommandBuffer> fence = [impl_->queue commandBuffer];
    [fence commit];
    [fence waitUntilCompleted];

    if (fence.status == MTLCommandBufferStatusError) {
        error =
            fence.error
                ? [[fence.error localizedDescription] UTF8String]
                : "Metal preview command failed";
        return false;
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(impl_->width)
        * static_cast<std::size_t>(impl_->height);
    const float* output =
        static_cast<const float*>(impl_->destination.contents);

    result.width = impl_->width;
    result.height = impl_->height;
    result.display_rgb.resize(pixel_count * 3u);
    result.scope_rgb.resize(pixel_count * 3u);

    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        for (std::size_t channel = 0; channel < 3u; ++channel) {
            const float value = output[pixel * 4u + channel];
            result.scope_rgb[pixel * 3u + channel] = value;
            result.display_rgb[pixel * 3u + channel] =
                static_cast<std::uint8_t>(
                    std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
    }

    return true;
}
