// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#include "outputtransform.h"
#include "ocioconfig.h"
#include "colortransform.h"
#include "displaygamutcompressor.h"
#include <algorithm>
#include <map>
#include <mutex>
namespace OCIO = OCIO_NAMESPACE;
struct OutputTransform::Impl { OCIO::ConstCPUProcessorRcPtr processor; };
const std::vector<std::string>& OutputTransform::profiles(const std::string& resources)
{
    const auto& config = FilmVizOCIO::catalog(resources);
    static std::mutex mutex;
    static std::map<const FilmVizOCIO::Catalog*, std::vector<std::string>> lists;
    const std::lock_guard<std::mutex> lock(mutex);
    auto found = lists.find(&config);
    if (found != lists.end()) return found->second;
    std::vector<std::string> names = {"ap0-linear", "rec709-gamma24"};
    names.insert(names.end(), config.names.begin(), config.names.end());
    return lists.emplace(&config, std::move(names)).first->second;
}
bool OutputTransform::parse_encoding(const std::string& name, Encoding& encoding, const std::string& resources)
{
    const auto& names = profiles(resources);
    auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) {
        const auto space = FilmVizOCIO::catalog(resources).config->getColorSpace(name.c_str());
        if (!space) return false;
        found = std::find(names.begin(), names.end(), space->getName());
    }
    if (found == names.end()) return false;
    encoding = static_cast<Encoding>(std::distance(names.begin(), found));
    return true;
}
OutputTransform::OutputTransform(Encoding encoding, const std::string& resources) : encoding_(encoding)
{
    auto impl = std::make_shared<Impl>();
    if (encoding != Encoding::AP0Linear && encoding != Encoding::Rec709Gamma24) {
        const auto& name = profiles(resources).at(static_cast<std::size_t>(encoding));
        impl->processor = FilmVizOCIO::catalog(resources).config->getProcessor(
            "ACES2065-1", name.c_str())->getDefaultCPUProcessor();
    }
    impl_ = std::move(impl);
}
std::array<float, 3> OutputTransform::from_ap0(const std::array<float, 3>& rgb) const
{
    if (encoding_ == Encoding::Rec709Gamma24) {
        static const ColorTransform preview(ColorTransform::ColorSpace::ACES2065_1,
            ColorTransform::TransferFunction::Linear, ColorTransform::ColorSpace::Rec709,
            ColorTransform::TransferFunction::Gamma24);
        return preview.encode_transfer(DisplayGamutCompressor::compress_rec709(preview.transform_linear(rgb)));
    }
    auto result = rgb;
    if (impl_->processor) impl_->processor->applyRGB(result.data());
    return result;
}
void OutputTransform::apply(float* pixels, int width, int height, int channels, std::ptrdiff_t row_bytes) const
{
    if (impl_->processor) {
        OCIO::PackedImageDesc image(pixels, width, height, channels, OCIO::BIT_DEPTH_F32,
            sizeof(float), channels * sizeof(float), row_bytes);
        impl_->processor->apply(image);
    } else if (encoding_ == Encoding::Rec709Gamma24) {
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<float*>(reinterpret_cast<char*>(pixels) + y * row_bytes);
            for (int x = 0; x < width; ++x) {
                float* p = row + x * channels;
                const auto rgb = from_ap0({{p[0], p[1], p[2]}});
                std::copy(rgb.begin(), rgb.end(), p);
            }
        }
    }
}
