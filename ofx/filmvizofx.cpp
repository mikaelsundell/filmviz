// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

// FilmViz OpenFX front end with CPU and Metal render backends.


#include "ofxCore.h"
#include "filmvizofxprocessor.h"
#include "filmvizofxlog.h"
#include "filmcolorresponse.h"
#include "filmformat.h"
#include "negativeprofile.h"
#include "printprofile.h"

#if FILMVIZ_HAS_METAL
#include "filmvizmetalprocessor.h"
#endif

#include "ofxImageEffect.h"
#include "ofxGPURender.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace
{

constexpr const char* kPluginIdentifier = "com.github.mikaelsundell.filmviz";
constexpr const char* kPluginLabel = "FilmViz";
constexpr const char* kPluginGrouping = "FilmViz";

constexpr const char* kParamBackend = "backend";
constexpr const char* kParamInputProfile = "inputProfile";
constexpr const char* kParamNegativeProfile = "negativeProfile";
constexpr const char* kParamPrintProfile = "printProfile";
constexpr const char* kParamOutputProfile = "outputProfile";
constexpr const char* kParamExposure = "exposure";
constexpr const char* kParamNegativeFlash = "negativeFlash";
constexpr const char* kParamPrintFlash = "printFlash";
constexpr const char* kParamPushPull = "pushPull";
constexpr const char* kParamColorDensity = "colorDensity";
constexpr const char* kParamWarmToneSeparation = "warmToneSeparation";
constexpr const char* kParamNegativeBleachBypass = "negativeBleachBypass";
constexpr const char* kParamPrintBleachBypass = "printBleachBypass";
constexpr const char* kParamPrinterLightRed = "printerLightRed";
constexpr const char* kParamPrinterLightGreen = "printerLightGreen";
constexpr const char* kParamPrinterLightBlue = "printerLightBlue";
constexpr const char* kParamPrinterLightMaster = "printerLightMaster";
constexpr const char* kParamMiddleGray = "middleGray";
constexpr const char* kParamFilmFormat = "filmFormat";
constexpr const char* kParamImageWidthMm = "imageWidthMm";
constexpr const char* kParamNegativeMtf = "negativeMtf";
constexpr const char* kParamPrintMtf = "printMtf";
constexpr const char* kParamEnableGrain = "enableGrain";
constexpr const char* kParamNegativeGrain = "negativeGrain";
constexpr const char* kParamPrintGrain = "printGrain";
constexpr const char* kParamGrainSize = "grainSize";
constexpr const char* kParamGrainChroma = "grainChroma";
constexpr const char* kParamGrainSeed = "grainSeed";
constexpr const char* kParamEnableHalation = "enableHalation";
constexpr const char* kParamHalationStrength = "halationStrength";
constexpr const char* kParamHalationRadius = "halationRadius";
constexpr const char* kParamHalationThreshold = "halationThreshold";
constexpr const char* kParamWorkerThreads = "workerThreads";

constexpr const char* kGroupSetup = "groupSetup";
constexpr const char* kGroupNegative = "groupNegative";
constexpr const char* kGroupPrint = "groupPrint";
constexpr const char* kGroupSpatial = "groupSpatial";
constexpr const char* kGroupGrain = "groupGrain";
constexpr const char* kGroupHalation = "groupHalation";
constexpr const char* kGroupAdvanced = "groupAdvanced";

constexpr int kInteractiveLutSize = 9;

float
quantize_interactive(
    float value,
    float step)
{
    return
        step > 0.0f
            ? std::round(value / step) * step
            : value;
}

FilmVizOfxRenderSettings
interactive_transform_settings(
    const FilmVizOfxRenderSettings& settings)
{
    FilmVizOfxRenderSettings preview = settings;

    // Coarse steps encourage reuse while Resolve emits the dense sequence of
    // values produced by slider drags. The exact values and production LUT
    // size are restored for the non-interactive render.
    preview.push_pull_stops =
        quantize_interactive(preview.push_pull_stops, 0.10f);
    preview.color_density =
        quantize_interactive(preview.color_density, 0.05f);
    preview.warm_tone_separation =
        quantize_interactive(preview.warm_tone_separation, 0.05f);
    preview.negative_flash_percent =
        quantize_interactive(preview.negative_flash_percent, 0.10f);
    preview.print_flash_percent =
        quantize_interactive(preview.print_flash_percent, 0.10f);
    preview.negative_bleach_bypass =
        quantize_interactive(preview.negative_bleach_bypass, 0.05f);
    preview.print_bleach_bypass =
        quantize_interactive(preview.print_bleach_bypass, 0.05f);
    preview.printer_light_red =
        quantize_interactive(preview.printer_light_red, 0.50f);
    preview.printer_light_green =
        quantize_interactive(preview.printer_light_green, 0.50f);
    preview.printer_light_blue =
        quantize_interactive(preview.printer_light_blue, 0.50f);
    preview.printer_light_master =
        quantize_interactive(preview.printer_light_master, 0.50f);
    preview.middle_gray =
        std::max(
            0.01f,
            quantize_interactive(preview.middle_gray, 0.01f));

    return preview;
}

OfxHost* gHost = nullptr;
const OfxImageEffectSuiteV1* gEffectSuite = nullptr;
const OfxPropertySuiteV1* gPropertySuite = nullptr;
const OfxParameterSuiteV1* gParameterSuite = nullptr;

struct InstanceData
{
    OfxImageClipHandle source_clip = nullptr;
    OfxImageClipHandle output_clip = nullptr;

    OfxParamHandle backend = nullptr;
    OfxParamHandle input = nullptr;
    OfxParamHandle negative = nullptr;
    OfxParamHandle print = nullptr;
    OfxParamHandle output = nullptr;
    OfxParamHandle exposure = nullptr;
    OfxParamHandle negative_flash = nullptr;
    OfxParamHandle print_flash = nullptr;
    OfxParamHandle push_pull = nullptr;
    OfxParamHandle color_density = nullptr;
    OfxParamHandle warm_tone_separation = nullptr;
    OfxParamHandle negative_bypass = nullptr;
    OfxParamHandle print_bypass = nullptr;
    OfxParamHandle printer_r = nullptr;
    OfxParamHandle printer_g = nullptr;
    OfxParamHandle printer_b = nullptr;
    OfxParamHandle printer_master = nullptr;
    OfxParamHandle middle_gray = nullptr;
    OfxParamHandle film_format = nullptr;
    OfxParamHandle image_width_mm = nullptr;
    OfxParamHandle negative_mtf = nullptr;
    OfxParamHandle print_mtf = nullptr;
    OfxParamHandle threads = nullptr;

    OfxParamHandle grain_enabled = nullptr;
    OfxParamHandle negative_grain = nullptr;
    OfxParamHandle print_grain = nullptr;
    OfxParamHandle grain_size = nullptr;
    OfxParamHandle grain_chroma = nullptr;
    OfxParamHandle grain_seed = nullptr;

    OfxParamHandle halation_enabled = nullptr;
    OfxParamHandle halation_strength = nullptr;
    OfxParamHandle halation_radius = nullptr;
    OfxParamHandle halation_threshold = nullptr;

    std::string resources_directory;
    FilmVizOfxProcessor processor;
#if FILMVIZ_HAS_METAL
    FilmVizMetalProcessor metal_processor;
#endif
};

std::string
bundle_resources_directory()
{
    if (const char* override_path = std::getenv("FILMVIZ_RESOURCES")) {
        if (*override_path) {
            return override_path;
        }
    }

    std::filesystem::path binary;

#if defined(_WIN32)
    HMODULE module = nullptr;

    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&bundle_resources_directory),
            &module)) {

        char path[MAX_PATH] = {};
        const DWORD length =
            GetModuleFileNameA(
                module,
                path,
                MAX_PATH);

        if (length > 0) {
            binary = std::filesystem::path(path);
        }
    }
#else
    Dl_info info;

    if (dladdr(
            reinterpret_cast<const void*>(&bundle_resources_directory),
            &info)
        && info.dli_fname) {

        binary = std::filesystem::path(info.dli_fname);
    }
#endif

    if (!binary.empty()) {
        const std::filesystem::path candidate =
            binary.parent_path()
            .parent_path()
            / "Resources"
            / "filmviz";

        if (std::filesystem::exists(candidate)) {
            return candidate.string();
        }
    }

    return "resources";
}

InstanceData*
instance_data(
    OfxImageEffectHandle effect)
{
    OfxPropertySetHandle properties = nullptr;
    void* pointer = nullptr;

    if (gEffectSuite->getPropertySet(
            effect,
            &properties) != kOfxStatOK
        || !properties
        || gPropertySuite->propGetPointer(
            properties,
            kOfxPropInstanceData,
            0,
            &pointer) != kOfxStatOK) {

        return nullptr;
    }

    return static_cast<InstanceData*>(pointer);
}

bool
fetch_param(
    OfxParamSetHandle parameter_set,
    const char* name,
    OfxParamHandle& handle)
{
    return
        gParameterSuite->paramGetHandle(
            parameter_set,
            name,
            &handle,
            nullptr) == kOfxStatOK
        && handle;
}

bool
read_settings(
    InstanceData& instance,
    double time,
    FilmVizOfxRenderSettings& settings,
    int& backend)
{
    int backend_value = 0;
    int input = 0;
    int negative = 0;
    int print = 0;
    int output = 1;
    int film_format = 5;
    int threads = 0;

    int grain_enabled = 0;
    int grain_seed = 1;
    int halation_enabled = 0;

    double exposure = 0.0;
    double negative_flash = 0.0;
    double print_flash = 0.0;
    double push_pull = 0.0;
    double color_density = 0.0;
    double warm_tone_separation =
        FilmColorResponse::standard_warm_tone_separation;
    double negative_bypass = 0.0;
    double print_bypass = 0.0;
    double printer_r = 25.0;
    double printer_g = 25.0;
    double printer_b = 25.0;
    double printer_master = 0.0;
    double middle_gray = 0.18;
    double image_width_mm = 24.89;
    double negative_mtf = 0.0;
    double print_mtf = 0.0;

    double negative_grain = 0.0;
    double print_grain = 0.0;
    double grain_size = 1.0;
    double grain_chroma = 1.0;

    double halation_strength = 0.0;
    double halation_radius = 12.0;
    double halation_threshold = 0.7;

    const OfxStatus status[] = {
        gParameterSuite->paramGetValueAtTime(instance.backend, time, &backend_value),
        gParameterSuite->paramGetValueAtTime(instance.input, time, &input),
        gParameterSuite->paramGetValueAtTime(instance.negative, time, &negative),
        gParameterSuite->paramGetValueAtTime(instance.print, time, &print),
        gParameterSuite->paramGetValueAtTime(instance.output, time, &output),
        gParameterSuite->paramGetValueAtTime(instance.exposure, time, &exposure),
        gParameterSuite->paramGetValueAtTime(instance.negative_flash, time, &negative_flash),
        gParameterSuite->paramGetValueAtTime(instance.print_flash, time, &print_flash),
        gParameterSuite->paramGetValueAtTime(instance.push_pull, time, &push_pull),
        gParameterSuite->paramGetValueAtTime(instance.color_density, time, &color_density),
        gParameterSuite->paramGetValueAtTime(instance.warm_tone_separation, time, &warm_tone_separation),
        gParameterSuite->paramGetValueAtTime(instance.negative_bypass, time, &negative_bypass),
        gParameterSuite->paramGetValueAtTime(instance.print_bypass, time, &print_bypass),
        gParameterSuite->paramGetValueAtTime(instance.printer_r, time, &printer_r),
        gParameterSuite->paramGetValueAtTime(instance.printer_g, time, &printer_g),
        gParameterSuite->paramGetValueAtTime(instance.printer_b, time, &printer_b),
        gParameterSuite->paramGetValueAtTime(instance.printer_master, time, &printer_master),
        gParameterSuite->paramGetValueAtTime(instance.middle_gray, time, &middle_gray),
        gParameterSuite->paramGetValueAtTime(instance.film_format, time, &film_format),
        gParameterSuite->paramGetValueAtTime(instance.image_width_mm, time, &image_width_mm),
        gParameterSuite->paramGetValueAtTime(instance.negative_mtf, time, &negative_mtf),
        gParameterSuite->paramGetValueAtTime(instance.print_mtf, time, &print_mtf),
        gParameterSuite->paramGetValueAtTime(instance.threads, time, &threads),
        gParameterSuite->paramGetValueAtTime(instance.grain_enabled, time, &grain_enabled),
        gParameterSuite->paramGetValueAtTime(instance.negative_grain, time, &negative_grain),
        gParameterSuite->paramGetValueAtTime(instance.print_grain, time, &print_grain),
        gParameterSuite->paramGetValueAtTime(instance.grain_size, time, &grain_size),
        gParameterSuite->paramGetValueAtTime(instance.grain_chroma, time, &grain_chroma),
        gParameterSuite->paramGetValueAtTime(instance.grain_seed, time, &grain_seed),
        gParameterSuite->paramGetValueAtTime(instance.halation_enabled, time, &halation_enabled),
        gParameterSuite->paramGetValueAtTime(instance.halation_strength, time, &halation_strength),
        gParameterSuite->paramGetValueAtTime(instance.halation_radius, time, &halation_radius),
        gParameterSuite->paramGetValueAtTime(instance.halation_threshold, time, &halation_threshold)
    };

    for (OfxStatus value : status) {
        if (value != kOfxStatOK) {
            return false;
        }
    }

#if FILMVIZ_HAS_METAL
    backend = backend_value;
#else
    backend = backend_value == 1 ? 2 : 0;
#endif

    const auto& negative_profiles =
        NegativeProfileCatalog::profiles();
    const auto& print_profiles =
        PrintProfileCatalog::profiles();
    const auto& film_formats =
        FilmFormatCatalog::formats();

    if (negative < 0
        || negative >= static_cast<int>(negative_profiles.size())
        || print < 0
        || print >= static_cast<int>(print_profiles.size())
        || film_format < 0
        || film_format >= static_cast<int>(film_formats.size())) {
        return false;
    }

    settings.input_profile = input;
    settings.negative_profile =
        negative_profiles[static_cast<std::size_t>(negative)].identifier;
    settings.print_profile =
        print_profiles[static_cast<std::size_t>(print)].identifier;
    settings.output_profile = output;
    settings.threads = threads;
    settings.exposure_stops = static_cast<float>(exposure);
    settings.negative_flash_percent = static_cast<float>(negative_flash);
    settings.print_flash_percent = static_cast<float>(print_flash);
    settings.push_pull_stops = static_cast<float>(push_pull);
    settings.color_density = static_cast<float>(color_density);
    settings.warm_tone_separation =
        static_cast<float>(warm_tone_separation);
    settings.negative_bleach_bypass = static_cast<float>(negative_bypass);
    settings.print_bleach_bypass = static_cast<float>(print_bypass);
    settings.printer_light_red = static_cast<float>(printer_r);
    settings.printer_light_green = static_cast<float>(printer_g);
    settings.printer_light_blue = static_cast<float>(printer_b);
    settings.printer_light_master = static_cast<float>(printer_master);
    settings.middle_gray = static_cast<float>(middle_gray);
    settings.film_format =
        film_formats[static_cast<std::size_t>(film_format)].identifier;
    settings.image_width_mm =
        settings.film_format == "custom"
            ? static_cast<float>(image_width_mm)
            : film_formats[
                static_cast<std::size_t>(film_format)].image_width_mm;
    settings.negative_mtf_amount = static_cast<float>(negative_mtf);
    settings.print_mtf_amount = static_cast<float>(print_mtf);

    settings.grain_enabled = grain_enabled != 0;
    settings.negative_grain = static_cast<float>(negative_grain);
    settings.print_grain = static_cast<float>(print_grain);
    settings.grain_size = static_cast<float>(grain_size);
    settings.grain_chroma = static_cast<float>(grain_chroma);
    settings.grain_seed =
        static_cast<std::uint32_t>(
            std::max(
                0,
                grain_seed));

    settings.halation_enabled = halation_enabled != 0;
    settings.halation_strength = static_cast<float>(halation_strength);
    settings.halation_radius = static_cast<float>(halation_radius);
    settings.halation_threshold = static_cast<float>(halation_threshold);

    return true;
}

OfxStatus
create_instance(
    OfxImageEffectHandle effect)
{
    auto instance =
        std::make_unique<InstanceData>();

    instance->resources_directory =
        bundle_resources_directory();

    if (gEffectSuite->clipGetHandle(
            effect,
            kOfxImageEffectSimpleSourceClipName,
            &instance->source_clip,
            nullptr) != kOfxStatOK
        || !instance->source_clip
        || gEffectSuite->clipGetHandle(
            effect,
            kOfxImageEffectOutputClipName,
            &instance->output_clip,
            nullptr) != kOfxStatOK
        || !instance->output_clip) {

        return kOfxStatFailed;
    }

    OfxParamSetHandle parameter_set = nullptr;

    if (gEffectSuite->getParamSet(
            effect,
            &parameter_set) != kOfxStatOK
        || !parameter_set) {

        return kOfxStatFailed;
    }

    const bool ok =
        fetch_param(parameter_set, kParamBackend, instance->backend)
        && fetch_param(parameter_set, kParamInputProfile, instance->input)
        && fetch_param(parameter_set, kParamNegativeProfile, instance->negative)
        && fetch_param(parameter_set, kParamPrintProfile, instance->print)
        && fetch_param(parameter_set, kParamOutputProfile, instance->output)
        && fetch_param(parameter_set, kParamExposure, instance->exposure)
        && fetch_param(parameter_set, kParamNegativeFlash, instance->negative_flash)
        && fetch_param(parameter_set, kParamPrintFlash, instance->print_flash)
        && fetch_param(parameter_set, kParamPushPull, instance->push_pull)
        && fetch_param(parameter_set, kParamColorDensity, instance->color_density)
        && fetch_param(parameter_set, kParamWarmToneSeparation, instance->warm_tone_separation)
        && fetch_param(parameter_set, kParamNegativeBleachBypass, instance->negative_bypass)
        && fetch_param(parameter_set, kParamPrintBleachBypass, instance->print_bypass)
        && fetch_param(parameter_set, kParamPrinterLightRed, instance->printer_r)
        && fetch_param(parameter_set, kParamPrinterLightGreen, instance->printer_g)
        && fetch_param(parameter_set, kParamPrinterLightBlue, instance->printer_b)
        && fetch_param(parameter_set, kParamPrinterLightMaster, instance->printer_master)
        && fetch_param(parameter_set, kParamMiddleGray, instance->middle_gray)
        && fetch_param(parameter_set, kParamFilmFormat, instance->film_format)
        && fetch_param(parameter_set, kParamImageWidthMm, instance->image_width_mm)
        && fetch_param(parameter_set, kParamNegativeMtf, instance->negative_mtf)
        && fetch_param(parameter_set, kParamPrintMtf, instance->print_mtf)
        && fetch_param(parameter_set, kParamWorkerThreads, instance->threads)
        && fetch_param(parameter_set, kParamEnableGrain, instance->grain_enabled)
        && fetch_param(parameter_set, kParamNegativeGrain, instance->negative_grain)
        && fetch_param(parameter_set, kParamPrintGrain, instance->print_grain)
        && fetch_param(parameter_set, kParamGrainSize, instance->grain_size)
        && fetch_param(parameter_set, kParamGrainChroma, instance->grain_chroma)
        && fetch_param(parameter_set, kParamGrainSeed, instance->grain_seed)
        && fetch_param(parameter_set, kParamEnableHalation, instance->halation_enabled)
        && fetch_param(parameter_set, kParamHalationStrength, instance->halation_strength)
        && fetch_param(parameter_set, kParamHalationRadius, instance->halation_radius)
        && fetch_param(parameter_set, kParamHalationThreshold, instance->halation_threshold);

    if (!ok) {
        return kOfxStatFailed;
    }

    OfxPropertySetHandle properties = nullptr;

    if (gEffectSuite->getPropertySet(
            effect,
            &properties) != kOfxStatOK
        || !properties) {

        return kOfxStatFailed;
    }

    InstanceData* instance_pointer =
        instance.get();

    gPropertySuite->propSetPointer(
        properties,
        kOfxPropInstanceData,
        0,
        instance.release());

    {
        std::ostringstream stream;
        stream
            << "node=" << instance_pointer
            << " resources=" << instance_pointer->resources_directory;
        FilmVizOfxLog::write("node_create", stream.str());
    }

    return kOfxStatOK;
}

OfxStatus
destroy_instance(
    OfxImageEffectHandle effect)
{
    OfxPropertySetHandle properties = nullptr;
    void* pointer = nullptr;

    if (gEffectSuite->getPropertySet(
            effect,
            &properties) != kOfxStatOK
        || !properties) {

        return kOfxStatFailed;
    }

    gPropertySuite->propGetPointer(
        properties,
        kOfxPropInstanceData,
        0,
        &pointer);

    {
        std::ostringstream stream;
        stream << "node=" << pointer;
        FilmVizOfxLog::write("node_destroy", stream.str());
    }

    delete static_cast<InstanceData*>(pointer);

    gPropertySuite->propSetPointer(
        properties,
        kOfxPropInstanceData,
        0,
        nullptr);

    return kOfxStatOK;
}

bool
fetch_suites()
{
    if (!gHost || !gHost->fetchSuite) {
        return false;
    }

    gEffectSuite =
        reinterpret_cast<const OfxImageEffectSuiteV1*>(
            gHost->fetchSuite(
                gHost->host,
                kOfxImageEffectSuite,
                1));

    gPropertySuite =
        reinterpret_cast<const OfxPropertySuiteV1*>(
            gHost->fetchSuite(
                gHost->host,
                kOfxPropertySuite,
                1));

    gParameterSuite =
        reinterpret_cast<const OfxParameterSuiteV1*>(
            gHost->fetchSuite(
                gHost->host,
                kOfxParameterSuite,
                1));

    return
        gEffectSuite
        && gPropertySuite
        && gParameterSuite;
}

OfxStatus
describe(
    OfxImageEffectHandle effect)
{
    if (!fetch_suites()) {
        return kOfxStatErrMissingHostFeature;
    }

    OfxPropertySetHandle properties = nullptr;

    if (gEffectSuite->getPropertySet(
            effect,
            &properties) != kOfxStatOK
        || !properties) {

        return kOfxStatFailed;
    }

    gPropertySuite->propSetString(
        properties,
        kOfxPropLabel,
        0,
        kPluginLabel);

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPluginPropGrouping,
        0,
        kPluginGrouping);

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropSupportedContexts,
        0,
        kOfxImageEffectContextFilter);

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropSupportedContexts,
        1,
        kOfxImageEffectContextGeneral);

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropSupportedPixelDepths,
        0,
        kOfxBitDepthFloat);

#if FILMVIZ_HAS_METAL
#ifdef kOfxImageEffectPropMetalRenderSupported
    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropMetalRenderSupported,
        0,
        "true");
#endif
#endif

#ifdef kOfxImageEffectPropCPURenderSupported
    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropCPURenderSupported,
        0,
        "true");
#endif

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPluginRenderThreadSafety,
        0,
        kOfxImageEffectRenderFullySafe);

    gPropertySuite->propSetInt(
        properties,
        kOfxImageEffectPluginPropFieldRenderTwiceAlways,
        0,
        0);

    gPropertySuite->propSetInt(
        properties,
        kOfxImageEffectPropSupportsMultipleClipDepths,
        0,
        0);

    gPropertySuite->propSetInt(
        properties,
        kOfxImageEffectPropSupportsTiles,
        0,
        0);

    return kOfxStatOK;
}

bool
define_clip(
    OfxImageEffectHandle effect,
    const char* name,
    bool source)
{
    OfxPropertySetHandle properties = nullptr;

    if (gEffectSuite->clipDefine(
            effect,
            name,
            &properties) != kOfxStatOK
        || !properties) {

        return false;
    }

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropSupportedComponents,
        0,
        kOfxImageComponentRGBA);

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropSupportedComponents,
        1,
        kOfxImageComponentRGB);

    if (source) {
        gPropertySuite->propSetInt(
            properties,
            kOfxImageClipPropOptional,
            0,
            0);
    }

    return true;
}

bool
define_group_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    bool open)
{
    OfxPropertySetHandle properties = nullptr;

    if (gParameterSuite->paramDefine(
            parameter_set,
            kOfxParamTypeGroup,
            name,
            &properties) != kOfxStatOK
        || !properties) {

        return false;
    }

    gPropertySuite->propSetString(properties, kOfxPropLabel, 0, label);
    gPropertySuite->propSetInt(
        properties,
        kOfxParamPropGroupOpen,
        0,
        open ? 1 : 0);
    return true;
}

bool
set_parameter_parent(
    OfxPropertySetHandle properties,
    const char* parent)
{
    return
        !parent
        || gPropertySuite->propSetString(
               properties,
               kOfxParamPropParent,
               0,
               parent) == kOfxStatOK;
}

bool
define_boolean_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    int default_value,
    const char* parent = nullptr)
{
    OfxPropertySetHandle properties = nullptr;

    if (gParameterSuite->paramDefine(
            parameter_set,
            kOfxParamTypeBoolean,
            name,
            &properties) != kOfxStatOK
        || !properties) {

        return false;
    }

    gPropertySuite->propSetString(
        properties,
        kOfxPropLabel,
        0,
        label);

    gPropertySuite->propSetInt(
        properties,
        kOfxParamPropDefault,
        0,
        default_value);

    return set_parameter_parent(properties, parent);
}

bool
define_choice_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    const char* const* options,
    int option_count,
    int default_value,
    const char* parent = nullptr)
{
    OfxPropertySetHandle properties = nullptr;

    if (gParameterSuite->paramDefine(
            parameter_set,
            kOfxParamTypeChoice,
            name,
            &properties) != kOfxStatOK
        || !properties) {

        return false;
    }

    gPropertySuite->propSetString(
        properties,
        kOfxPropLabel,
        0,
        label);

    gPropertySuite->propSetInt(
        properties,
        kOfxParamPropDefault,
        0,
        default_value);

    for (int i = 0; i < option_count; ++i) {
        gPropertySuite->propSetString(
            properties,
            kOfxParamPropChoiceOption,
            i,
            options[i]);
    }

    return set_parameter_parent(properties, parent);
}

bool
define_integer_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    int default_value,
    int minimum,
    int maximum,
    const char* parent = nullptr)
{
    OfxPropertySetHandle properties = nullptr;

    if (gParameterSuite->paramDefine(
            parameter_set,
            kOfxParamTypeInteger,
            name,
            &properties) != kOfxStatOK
        || !properties) {

        return false;
    }

    gPropertySuite->propSetString(properties, kOfxPropLabel, 0, label);
    gPropertySuite->propSetInt(properties, kOfxParamPropDefault, 0, default_value);
    gPropertySuite->propSetInt(properties, kOfxParamPropMin, 0, minimum);
    gPropertySuite->propSetInt(properties, kOfxParamPropMax, 0, maximum);
    gPropertySuite->propSetInt(properties, kOfxParamPropDisplayMin, 0, minimum);
    gPropertySuite->propSetInt(properties, kOfxParamPropDisplayMax, 0, maximum);

    return set_parameter_parent(properties, parent);
}

bool
define_double_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    double default_value,
    double minimum,
    double maximum,
    const char* parent = nullptr)
{
    OfxPropertySetHandle properties = nullptr;

    if (gParameterSuite->paramDefine(
            parameter_set,
            kOfxParamTypeDouble,
            name,
            &properties) != kOfxStatOK
        || !properties) {

        return false;
    }

    gPropertySuite->propSetString(
        properties,
        kOfxPropLabel,
        0,
        label);

    gPropertySuite->propSetDouble(
        properties,
        kOfxParamPropDefault,
        0,
        default_value);

    gPropertySuite->propSetDouble(
        properties,
        kOfxParamPropMin,
        0,
        minimum);

    gPropertySuite->propSetDouble(
        properties,
        kOfxParamPropMax,
        0,
        maximum);

    gPropertySuite->propSetDouble(
        properties,
        kOfxParamPropDisplayMin,
        0,
        minimum);

    gPropertySuite->propSetDouble(
        properties,
        kOfxParamPropDisplayMax,
        0,
        maximum);

    gPropertySuite->propSetDouble(
        properties,
        kOfxParamPropIncrement,
        0,
        0.01);

    return set_parameter_parent(properties, parent);
}

OfxStatus
describe_in_context(
    OfxImageEffectHandle effect)
{
    if (!gEffectSuite || !gPropertySuite) {
        if (!fetch_suites()) {
            return kOfxStatErrMissingHostFeature;
        }
    }

    if (!define_clip(
            effect,
            kOfxImageEffectSimpleSourceClipName,
            true)
        || !define_clip(
            effect,
            kOfxImageEffectOutputClipName,
            false)) {

        return kOfxStatFailed;
    }

    OfxParamSetHandle parameter_set = nullptr;

    if (gEffectSuite->getParamSet(
            effect,
            &parameter_set) != kOfxStatOK
        || !parameter_set) {

        return kOfxStatFailed;
    }

#if FILMVIZ_HAS_METAL
    static const char* backends[] = {
        "Auto",
        "Metal",
        "CPU"
    };
    constexpr int backend_count = 3;
#else
    static const char* backends[] = {
        "Auto",
        "CPU"
    };
    constexpr int backend_count = 2;
#endif

    static const char* input_profiles[] = {
        "ARRI Wide Gamut 3 / LogC3 EI800",
        "ACES2065-1 / AP0"
    };

    const auto& supported_negatives =
        NegativeProfileCatalog::profiles();

    std::vector<const char*> negative_options;
    negative_options.reserve(supported_negatives.size());

    for (const auto& profile : supported_negatives) {
        negative_options.push_back(profile.display_name.c_str());
    }

    const auto& supported_prints =
        PrintProfileCatalog::profiles();

    std::vector<const char*> print_options;
    print_options.reserve(supported_prints.size());

    for (const auto& profile : supported_prints) {
        print_options.push_back(profile.display_name.c_str());
    }

    const auto& supported_formats =
        FilmFormatCatalog::formats();
    std::vector<const char*> format_options;
    format_options.reserve(supported_formats.size());
    int format_default = 0;

    for (std::size_t index = 0;
         index < supported_formats.size();
         ++index) {
        const auto& format = supported_formats[index];
        format_options.push_back(format.display_name.c_str());
        if (format.identifier
            == FilmFormatCatalog::default_format().identifier) {
            format_default = static_cast<int>(index);
        }
    }

    static const char* output_profiles[] = {
        "ACES2065-1 / AP0",
        "Rec.709 / Gamma 2.4"
    };

    if (!define_group_parameter(parameter_set, kGroupSetup, "Setup", true)
        || !define_group_parameter(parameter_set, kGroupNegative, "Negative", true)
        || !define_group_parameter(parameter_set, kGroupPrint, "Print", true)
        || !define_group_parameter(parameter_set, kGroupSpatial, "Spatial Response", false)
        || !define_group_parameter(parameter_set, kGroupGrain, "Grain", false)
        || !define_group_parameter(parameter_set, kGroupHalation, "Halation", false)
        || !define_group_parameter(parameter_set, kGroupAdvanced, "Advanced", false)
        || !define_choice_parameter(parameter_set, kParamBackend, "Processing", backends, backend_count, 0, kGroupSetup)
        || !define_choice_parameter(parameter_set, kParamInputProfile, "Input", input_profiles, 2, 0, kGroupSetup)
        || !define_choice_parameter(parameter_set, kParamNegativeProfile, "Stock", negative_options.data(), static_cast<int>(negative_options.size()), 0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamExposure, "Exposure", 0.0, -8.0, 8.0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamNegativeFlash, "Flash (%)", 0.0, 0.0, 25.0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamPushPull, "Push / Pull", 0.0, -3.0, 3.0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamColorDensity, "Color Density", 0.0, FilmColorResponse::minimum_trim, FilmColorResponse::maximum_trim, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamWarmToneSeparation, "Warm Separation", FilmColorResponse::standard_warm_tone_separation, FilmColorResponse::minimum_warm_tone_separation, FilmColorResponse::maximum_warm_tone_separation, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamNegativeBleachBypass, "Bleach Bypass", 0.0, 0.0, 1.0, kGroupNegative)
        || !define_choice_parameter(parameter_set, kParamPrintProfile, "Stock", print_options.data(), static_cast<int>(print_options.size()), 0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrintFlash, "Flash (%)", 0.0, 0.0, 25.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrintBleachBypass, "Bleach Bypass", 0.0, 0.0, 1.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightRed, "Printer Light Red", 25.0, 0.0, 50.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightGreen, "Printer Light Green", 25.0, 0.0, 50.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightBlue, "Printer Light Blue", 25.0, 0.0, 50.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightMaster, "Printer Light Master", 0.0, -25.0, 25.0, kGroupPrint)
        || !define_choice_parameter(parameter_set, kParamOutputProfile, "Output", output_profiles, 2, 1, kGroupSetup)
        || !define_choice_parameter(parameter_set, kParamFilmFormat, "Film Format", format_options.data(), static_cast<int>(format_options.size()), format_default, kGroupSpatial)
        || !define_double_parameter(parameter_set, kParamImageWidthMm, "Custom Image Width (mm)", 24.89, 1.0, 100.0, kGroupSpatial)
        || !define_double_parameter(parameter_set, kParamNegativeMtf, "Negative MTF", 0.0, 0.0, 2.0, kGroupSpatial)
        || !define_double_parameter(parameter_set, kParamPrintMtf, "Print MTF", 0.0, 0.0, 2.0, kGroupSpatial)
        || !define_boolean_parameter(parameter_set, kParamEnableGrain, "Enable", 0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamNegativeGrain, "Negative", 0.0, 0.0, 2.0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamPrintGrain, "Print", 0.0, 0.0, 2.0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamGrainSize, "Scale", 1.0, 1.0, 5.0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamGrainChroma, "Chroma", 1.0, 0.0, 2.0, kGroupGrain)
        || !define_integer_parameter(parameter_set, kParamGrainSeed, "Seed", 1, 0, 1000000, kGroupGrain)
        || !define_boolean_parameter(parameter_set, kParamEnableHalation, "Enable", 0, kGroupHalation)
        || !define_double_parameter(parameter_set, kParamHalationStrength, "Strength", 0.0, 0.0, 1.0, kGroupHalation)
        || !define_double_parameter(parameter_set, kParamHalationRadius, "Radius", 12.0, 0.0, 200.0, kGroupHalation)
        || !define_double_parameter(parameter_set, kParamHalationThreshold, "Threshold", 0.7, 0.0, 4.0, kGroupHalation)
        || !define_double_parameter(parameter_set, kParamMiddleGray, "Middle Gray", 0.18, 0.01, 1.0, kGroupAdvanced)
        || !define_integer_parameter(parameter_set, kParamWorkerThreads, "Worker Threads", 0, 0, 64, kGroupAdvanced)) {

        return kOfxStatFailed;
    }

    return kOfxStatOK;
}

OfxStatus
render(
    OfxImageEffectHandle effect,
    OfxPropertySetHandle in_args)
{
    if (!gEffectSuite
        || !gPropertySuite
        || !gParameterSuite) {

        return kOfxStatErrMissingHostFeature;
    }

    InstanceData* instance =
        instance_data(
            effect);

    if (!instance) {
        return kOfxStatFailed;
    }

    double time = 0.0;
    int render_window[4] = {0, 0, 0, 0};
    int interactive_render = 0;
    int draft_render = 0;

    if (gPropertySuite->propGetDouble(
            in_args,
            kOfxPropTime,
            0,
            &time) != kOfxStatOK
        || gPropertySuite->propGetIntN(
            in_args,
            kOfxImageEffectPropRenderWindow,
            4,
            render_window) != kOfxStatOK) {

        return kOfxStatFailed;
    }

    // These are optional host hints. Resolve supplies the interactive flag
    // while a parameter is actively being adjusted.
    gPropertySuite->propGetInt(
        in_args,
        kOfxImageEffectPropInteractiveRenderStatus,
        0,
        &interactive_render);

    gPropertySuite->propGetInt(
        in_args,
        kOfxImageEffectPropRenderQualityDraft,
        0,
        &draft_render);

    OfxPropertySetHandle source_image = nullptr;
    OfxPropertySetHandle output_image = nullptr;

    if (gEffectSuite->clipGetImage(
            instance->source_clip,
            time,
            nullptr,
            &source_image) != kOfxStatOK
        || !source_image) {

        return kOfxStatFailed;
    }

    if (gEffectSuite->clipGetImage(
            instance->output_clip,
            time,
            nullptr,
            &output_image) != kOfxStatOK
        || !output_image) {

        gEffectSuite->clipReleaseImage(
            source_image);

        return kOfxStatFailed;
    }

    auto release_images =
        [&]() {
            gEffectSuite->clipReleaseImage(
                output_image);

            gEffectSuite->clipReleaseImage(
                source_image);
        };

    char* source_depth = nullptr;
    char* output_depth = nullptr;
    char* source_components = nullptr;
    char* output_components = nullptr;

    gPropertySuite->propGetString(
        source_image,
        kOfxImageEffectPropPixelDepth,
        0,
        &source_depth);

    gPropertySuite->propGetString(
        output_image,
        kOfxImageEffectPropPixelDepth,
        0,
        &output_depth);

    gPropertySuite->propGetString(
        source_image,
        kOfxImageEffectPropComponents,
        0,
        &source_components);

    gPropertySuite->propGetString(
        output_image,
        kOfxImageEffectPropComponents,
        0,
        &output_components);

    if (!source_depth
        || !output_depth
        || std::strcmp(
            source_depth,
            kOfxBitDepthFloat) != 0
        || std::strcmp(
            output_depth,
            kOfxBitDepthFloat) != 0
        || !source_components
        || !output_components
        || std::strcmp(
            source_components,
            kOfxImageComponentRGBA) != 0
        || std::strcmp(
            output_components,
            kOfxImageComponentRGBA) != 0) {

        release_images();
        return kOfxStatErrUnsupported;
    }

    FilmVizOfxFrame source_frame;
    FilmVizOfxFrame output_frame;

    int source_bounds[4] = {0, 0, 0, 0};
    int output_bounds[4] = {0, 0, 0, 0};

    if (gPropertySuite->propGetIntN(
            source_image,
            kOfxImagePropBounds,
            4,
            source_bounds) != kOfxStatOK
        || gPropertySuite->propGetIntN(
            output_image,
            kOfxImagePropBounds,
            4,
            output_bounds) != kOfxStatOK) {

        release_images();
        return kOfxStatFailed;
    }

    source_frame.x1 = source_bounds[0];
    source_frame.y1 = source_bounds[1];
    source_frame.x2 = source_bounds[2];
    source_frame.y2 = source_bounds[3];

    output_frame.x1 = output_bounds[0];
    output_frame.y1 = output_bounds[1];
    output_frame.x2 = output_bounds[2];
    output_frame.y2 = output_bounds[3];

    int source_row_bytes = 0;
    int output_row_bytes = 0;
    void* source_data = nullptr;
    void* output_data = nullptr;

    if (gPropertySuite->propGetInt(
            source_image,
            kOfxImagePropRowBytes,
            0,
            &source_row_bytes) != kOfxStatOK
        || gPropertySuite->propGetInt(
            output_image,
            kOfxImagePropRowBytes,
            0,
            &output_row_bytes) != kOfxStatOK
        || gPropertySuite->propGetPointer(
            source_image,
            kOfxImagePropData,
            0,
            &source_data) != kOfxStatOK
        || !source_data
        || gPropertySuite->propGetPointer(
            output_image,
            kOfxImagePropData,
            0,
            &output_data) != kOfxStatOK
        || !output_data) {

        release_images();
        return kOfxStatFailed;
    }

    source_frame.row_bytes =
        source_row_bytes;

    output_frame.row_bytes =
        output_row_bytes;

    FilmVizOfxRenderSettings settings;
    int backend = 0;

    if (!read_settings(
            *instance,
            time,
            settings,
            backend)) {

        release_images();
        return kOfxStatFailed;
    }

    const bool interactive =
        interactive_render != 0
        || draft_render != 0;

    if (interactive
        && !instance->processor.has_transform(
            settings,
            instance->resources_directory)) {
        FilmVizOfxRenderSettings preview =
            interactive_transform_settings(settings);

        // Runtime-only edits such as Exposure retain the resident full-quality
        // transform. Transform-changing edits get a small temporary LUT, then
        // settle at the requested size after the drag.
        if (!instance->processor.has_transform(
                preview,
                instance->resources_directory)) {
            preview.lut_size =
                std::min(
                    preview.lut_size,
                    kInteractiveLutSize);
        }

        settings = preview;
    }

    std::ostringstream render_details;
    render_details
        << "node=" << instance
        << " time=" << time
        << " backend_request=" << backend
        << " negative=" << settings.negative_profile
        << " print=" << settings.print_profile
        << " exposure=" << settings.exposure_stops
        << " lut=" << settings.lut_size
        << " interactive=" << (interactive ? 1 : 0)
        << " format=" << settings.film_format
        << " width_mm=" << settings.image_width_mm
        << " negative_mtf=" << settings.negative_mtf_amount
        << " print_mtf=" << settings.print_mtf_amount
        << " grain=" << (settings.grain_enabled ? 1 : 0)
        << " halation=" << (settings.halation_enabled ? 1 : 0);

    const bool requires_cpu_spatial =
        settings.negative_mtf_amount > 0.0f
        || settings.print_mtf_amount > 0.0f;

    FilmVizOfxLog::Scope render_scope(
        "render",
        render_details.str());

    int metal_enabled = 0;
    void* metal_command_queue = nullptr;

#if FILMVIZ_HAS_METAL
#ifdef kOfxImageEffectPropMetalEnabled
    gPropertySuite->propGetInt(
        in_args,
        kOfxImageEffectPropMetalEnabled,
        0,
        &metal_enabled);
#endif
#ifdef kOfxImageEffectPropMetalCommandQueue
    if (metal_enabled) {
        gPropertySuite->propGetPointer(
            in_args,
            kOfxImageEffectPropMetalCommandQueue,
            0,
            &metal_command_queue);
    }
#endif
#endif

#if FILMVIZ_HAS_METAL
    if (metal_enabled
        && metal_command_queue) {

        FilmVizOfxMetalFrame metal_source;
        metal_source.x1 = source_frame.x1;
        metal_source.y1 = source_frame.y1;
        metal_source.x2 = source_frame.x2;
        metal_source.y2 = source_frame.y2;
        metal_source.row_bytes = source_row_bytes;
        metal_source.buffer = source_data;

        FilmVizOfxMetalFrame metal_output;
        metal_output.x1 = output_frame.x1;
        metal_output.y1 = output_frame.y1;
        metal_output.x2 = output_frame.x2;
        metal_output.y2 = output_frame.y2;
        metal_output.row_bytes = output_row_bytes;
        metal_output.buffer = output_data;

        std::string error;
        bool rendered = false;

        if (!instance->processor.configure(
                settings,
                instance->resources_directory,
                error)) {

            release_images();
            return kOfxStatFailed;
        }

        // Processing choices:
        //   Auto  -> Metal when Resolve supplied Metal buffers.
        //   Metal -> Metal when available, otherwise CPU fallback below.
        //   CPU   -> explicitly stage through shared buffers so the CPU
        //            reference path can still be compared in a Metal render.
        if (backend == 2 || requires_cpu_spatial) {
            rendered =
                instance->metal_processor.render_cpu_bridge(
                    instance->processor,
                    metal_command_queue,
                    metal_source,
                    metal_output,
                    render_window[0],
                    render_window[1],
                    render_window[2],
                    render_window[3],
                    time,
                    [&]() {
                        return
                            gEffectSuite->abort(
                                effect) != 0;
                    },
                    error);
        }
        else {
            rendered =
                instance->metal_processor.configure(
                    instance->processor,
                    metal_command_queue,
                    error)
                && instance->metal_processor.render(
                    settings,
                    metal_command_queue,
                    metal_source,
                    metal_output,
                    render_window[0],
                    render_window[1],
                    render_window[2],
                    render_window[3],
                    time,
                    error);
        }

        release_images();

        if (!rendered) {
            render_scope.finish(
                std::string("backend=")
                    + (backend == 2 || requires_cpu_spatial
                        ? "cpu_bridge"
                        : "metal")
                    + " result=failed error=" + error);

            if (error == "render aborted") {
                return kOfxStatOK;
            }

            return backend == 2 || requires_cpu_spatial
                ? kOfxStatFailed
                : kOfxStatGPURenderFailed;
        }

        render_scope.finish(
            std::string("backend=")
                + (backend == 2 || requires_cpu_spatial
                    ? "cpu_bridge"
                    : "metal")
                + " result=ok");
        return kOfxStatOK;
    }
#endif

    // Resolve supplied ordinary CPU image pointers. Auto and CPU use the
    // reference CPU renderer; explicit Metal also falls back here when the
    // host did not enable Metal for this render action.
    source_frame.data =
        static_cast<float*>(
            source_data);

    output_frame.data =
        static_cast<float*>(
            output_data);

    std::string error;

    if (!instance->processor.configure(
            settings,
            instance->resources_directory,
            error)) {

        release_images();
        return kOfxStatFailed;
    }

    const bool rendered =
        instance->processor.render(
            source_frame,
            output_frame,
            render_window[0],
            render_window[1],
            render_window[2],
            render_window[3],
            time,
            [&]() {
                return
                    gEffectSuite->abort(
                        effect) != 0;
            },
            error);

    release_images();

    if (!rendered) {
        render_scope.finish(
            "backend=cpu result=failed error=" + error);
        return error == "render aborted"
            ? kOfxStatOK
            : kOfxStatFailed;
    }

    render_scope.finish("backend=cpu result=ok");
    return kOfxStatOK;
}

OfxStatus
plugin_main(
    const char* action,
    const void* handle,
    OfxPropertySetHandle in_args,
    OfxPropertySetHandle)
{
    const OfxImageEffectHandle effect =
        reinterpret_cast<OfxImageEffectHandle>(
            const_cast<void*>(handle));

    if (std::strcmp(
            action,
            kOfxActionLoad) == 0) {
        const bool loaded = fetch_suites();
        FilmVizOfxLog::write(
            "plugin_load",
            std::string("result=") + (loaded ? "ok" : "missing_host_feature")
                + " log=" + FilmVizOfxLog::path());
        return loaded
            ? kOfxStatOK
            : kOfxStatErrMissingHostFeature;
    }

    if (std::strcmp(
            action,
            kOfxActionUnload) == 0) {
        FilmVizOfxLog::write("plugin_unload");
        return kOfxStatOK;
    }

    if (std::strcmp(
            action,
            kOfxActionDescribe) == 0) {
        return describe(
            effect);
    }

    if (std::strcmp(
            action,
            kOfxImageEffectActionDescribeInContext) == 0) {
        return describe_in_context(
            effect);
    }

    if (std::strcmp(
            action,
            kOfxImageEffectActionRender) == 0) {
        return render(
            effect,
            in_args);
    }

    if (std::strcmp(
            action,
            kOfxActionCreateInstance) == 0) {
        return create_instance(
            effect);
    }

    if (std::strcmp(
            action,
            kOfxActionDestroyInstance) == 0) {
        return destroy_instance(
            effect);
    }

    return kOfxStatReplyDefault;
}

void
set_host(
    OfxHost* host)
{
    gHost = host;
}

OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi,
    1,
    kPluginIdentifier,
    1,
    1,
    set_host,
    plugin_main
};

} // namespace

extern "C"
{

OfxExport int
OfxGetNumberOfPlugins(void)
{
    return 1;
}

OfxExport OfxPlugin*
OfxGetPlugin(int index)
{
    return
        index == 0
            ? &gPlugin
            : nullptr;
}

} // extern "C"
