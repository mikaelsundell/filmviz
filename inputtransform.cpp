// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#include "inputtransform.h"
#include "ocioconfig.h"
#include <algorithm>

namespace OCIO = OCIO_NAMESPACE;
using FilmVizOCIO::catalog;
struct InputTransform::Impl { OCIO::ConstCPUProcessorRcPtr processor; };
InputTransform::InputTransform(Encoding encoding, const std::string& resources) : encoding_(encoding)
{
    const auto& data = catalog(resources);
    const auto& name = data.names.at(static_cast<std::size_t>(encoding));
    auto impl = std::make_shared<Impl>();
    impl->processor = data.config->getProcessor(name.c_str(), "ACES2065-1")->getDefaultCPUProcessor();
    impl_ = std::move(impl);
}
std::array<float, 3> InputTransform::to_ap0(const std::array<float, 3>& rgb) const
{
    auto result = rgb;
    impl_->processor->applyRGB(result.data());
    return result;
}
void InputTransform::apply_rgba(float* pixels, int width, int height, std::ptrdiff_t row_bytes) const
{
    OCIO::PackedImageDesc image(pixels, width, height, 4, OCIO::BIT_DEPTH_F32,
                               sizeof(float), 4 * sizeof(float), row_bytes);
    impl_->processor->apply(image);
}
InputTransform::Encoding InputTransform::encoding() const { return encoding_; }
const std::vector<std::string>& InputTransform::profiles(const std::string& resources) { return catalog(resources).names; }
bool InputTransform::parse_encoding(const std::string& name, Encoding& encoding, const std::string& resources)
{
    std::string resolved = name;
    if (name == "awg3-logc3-ei800" || name == "arri-awg3-logc3-ei800") resolved = "ARRI LogC3 (EI800)";
    if (name == "ap0-linear" || name == "aces2065-1-linear") resolved = "ACES2065-1";
    const auto& data = catalog(resources);
    const auto space = data.config->getColorSpace(resolved.c_str());
    if (!space) return false;
    const auto found = std::find(data.names.begin(), data.names.end(), space->getName());
    if (found == data.names.end()) return false;
    encoding = static_cast<Encoding>(std::distance(data.names.begin(), found));
    return true;
}
const char* InputTransform::name(Encoding encoding) { return profiles().at(static_cast<std::size_t>(encoding)).c_str(); }
