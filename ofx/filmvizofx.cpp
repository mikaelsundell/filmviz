// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

// Minimal OpenFX host-compatibility test for FilmViz.
//
// This intentionally has no FilmViz, OpenImageIO, Imath, threading, or other
// third-party runtime dependencies. It advertises a basic OFX image effect and
// fills the requested output render window with solid red. Once Resolve loads
// and renders this plug-in, the full FilmViz implementation can be restored
// incrementally behind the same OFX contract.

#include "ofxCore.h"
#include "filmvizofxprocessor.h"

#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

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

constexpr const char* kParamEnable = "enable";
constexpr const char* kParamInputProfile = "inputProfile";
constexpr const char* kParamNegativeProfile = "negativeProfile";
constexpr const char* kParamPrintProfile = "printProfile";
constexpr const char* kParamOutputProfile = "outputProfile";
constexpr const char* kParamExposure = "exposure";
constexpr const char* kParamPushPull = "pushPull";
constexpr const char* kParamNegativeBleachBypass = "negativeBleachBypass";
constexpr const char* kParamPrintBleachBypass = "printBleachBypass";
constexpr const char* kParamPrinterLightRed = "printerLightRed";
constexpr const char* kParamPrinterLightGreen = "printerLightGreen";
constexpr const char* kParamPrinterLightBlue = "printerLightBlue";
constexpr const char* kParamPrinterTemperature = "printerTemperature";
constexpr const char* kParamMiddleGray = "middleGray";
constexpr const char* kParamLutSize = "lutSize";
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

OfxHost* gHost = nullptr;
const OfxImageEffectSuiteV1* gEffectSuite = nullptr;
const OfxPropertySuiteV1* gPropertySuite = nullptr;
const OfxParameterSuiteV1* gParameterSuite = nullptr;

struct InstanceData
{
    OfxImageClipHandle source_clip = nullptr;
    OfxImageClipHandle output_clip = nullptr;

    OfxParamHandle enable = nullptr;
    OfxParamHandle input = nullptr;
    OfxParamHandle negative = nullptr;
    OfxParamHandle print = nullptr;
    OfxParamHandle output = nullptr;
    OfxParamHandle exposure = nullptr;
    OfxParamHandle push_pull = nullptr;
    OfxParamHandle negative_bypass = nullptr;
    OfxParamHandle print_bypass = nullptr;
    OfxParamHandle printer_r = nullptr;
    OfxParamHandle printer_g = nullptr;
    OfxParamHandle printer_b = nullptr;
    OfxParamHandle printer_k = nullptr;
    OfxParamHandle middle_gray = nullptr;
    OfxParamHandle lut_size = nullptr;
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
    bool& enabled)
{
    int enable = 1;
    int input = 0;
    int negative = 0;
    int print = 0;
    int output = 1;
    int lut_size = 33;
    int threads = 0;

    int grain_enabled = 0;
    int grain_seed = 1;
    int halation_enabled = 0;

    double exposure = 0.0;
    double push_pull = 0.0;
    double negative_bypass = 0.0;
    double print_bypass = 0.0;
    double printer_r = 25.0;
    double printer_g = 25.0;
    double printer_b = 25.0;
    double printer_k = 3200.0;
    double middle_gray = 0.18;

    double negative_grain = 0.0;
    double print_grain = 0.0;
    double grain_size = 1.0;
    double grain_chroma = 1.0;

    double halation_strength = 0.0;
    double halation_radius = 12.0;
    double halation_threshold = 0.7;

    const OfxStatus status[] = {
        gParameterSuite->paramGetValueAtTime(instance.enable, time, &enable),
        gParameterSuite->paramGetValueAtTime(instance.input, time, &input),
        gParameterSuite->paramGetValueAtTime(instance.negative, time, &negative),
        gParameterSuite->paramGetValueAtTime(instance.print, time, &print),
        gParameterSuite->paramGetValueAtTime(instance.output, time, &output),
        gParameterSuite->paramGetValueAtTime(instance.exposure, time, &exposure),
        gParameterSuite->paramGetValueAtTime(instance.push_pull, time, &push_pull),
        gParameterSuite->paramGetValueAtTime(instance.negative_bypass, time, &negative_bypass),
        gParameterSuite->paramGetValueAtTime(instance.print_bypass, time, &print_bypass),
        gParameterSuite->paramGetValueAtTime(instance.printer_r, time, &printer_r),
        gParameterSuite->paramGetValueAtTime(instance.printer_g, time, &printer_g),
        gParameterSuite->paramGetValueAtTime(instance.printer_b, time, &printer_b),
        gParameterSuite->paramGetValueAtTime(instance.printer_k, time, &printer_k),
        gParameterSuite->paramGetValueAtTime(instance.middle_gray, time, &middle_gray),
        gParameterSuite->paramGetValueAtTime(instance.lut_size, time, &lut_size),
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

    enabled = enable != 0;

    settings.input_profile = input;
    settings.negative_profile =
        negative == 1
            ? "kodak-50d"
            : "verita-200d";
    settings.output_profile = output;
    settings.lut_size = lut_size;
    settings.threads = threads;
    settings.exposure_stops = static_cast<float>(exposure);
    settings.push_pull_stops = static_cast<float>(push_pull);
    settings.negative_bleach_bypass = static_cast<float>(negative_bypass);
    settings.print_bleach_bypass = static_cast<float>(print_bypass);
    settings.printer_light_red = static_cast<float>(printer_r);
    settings.printer_light_green = static_cast<float>(printer_g);
    settings.printer_light_blue = static_cast<float>(printer_b);
    settings.printer_temperature = static_cast<float>(printer_k);
    settings.middle_gray = static_cast<float>(middle_gray);

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

    (void)print;
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
        fetch_param(parameter_set, kParamEnable, instance->enable)
        && fetch_param(parameter_set, kParamInputProfile, instance->input)
        && fetch_param(parameter_set, kParamNegativeProfile, instance->negative)
        && fetch_param(parameter_set, kParamPrintProfile, instance->print)
        && fetch_param(parameter_set, kParamOutputProfile, instance->output)
        && fetch_param(parameter_set, kParamExposure, instance->exposure)
        && fetch_param(parameter_set, kParamPushPull, instance->push_pull)
        && fetch_param(parameter_set, kParamNegativeBleachBypass, instance->negative_bypass)
        && fetch_param(parameter_set, kParamPrintBleachBypass, instance->print_bypass)
        && fetch_param(parameter_set, kParamPrinterLightRed, instance->printer_r)
        && fetch_param(parameter_set, kParamPrinterLightGreen, instance->printer_g)
        && fetch_param(parameter_set, kParamPrinterLightBlue, instance->printer_b)
        && fetch_param(parameter_set, kParamPrinterTemperature, instance->printer_k)
        && fetch_param(parameter_set, kParamMiddleGray, instance->middle_gray)
        && fetch_param(parameter_set, kParamLutSize, instance->lut_size)
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

    gPropertySuite->propSetPointer(
        properties,
        kOfxPropInstanceData,
        0,
        instance.release());

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
        1);

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
define_boolean_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    int default_value)
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

    return true;
}

bool
define_choice_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    const char* const* options,
    int option_count,
    int default_value)
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

    return true;
}

bool
define_integer_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    int default_value,
    int minimum,
    int maximum)
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

    return true;
}

bool
define_double_parameter(
    OfxParamSetHandle parameter_set,
    const char* name,
    const char* label,
    double default_value,
    double minimum,
    double maximum)
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

    return true;
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

    static const char* input_profiles[] = {
        "ARRI Wide Gamut 3 / LogC3 EI800",
        "ACES2065-1 / AP0"
    };

    static const char* negative_profiles[] = {
        "Kodak Verita 200D",
        "Kodak VISION3 50D 5203/7203"
    };

    static const char* print_profiles[] = {
        "Kodak 2383"
    };

    static const char* output_profiles[] = {
        "ACES2065-1 / AP0",
        "Rec.709 / Gamma 2.4"
    };

    if (!define_boolean_parameter(parameter_set, kParamEnable, "Enable", 1)
        || !define_choice_parameter(parameter_set, kParamInputProfile, "Input", input_profiles, 2, 0)
        || !define_choice_parameter(parameter_set, kParamNegativeProfile, "Negative", negative_profiles, 2, 0)
        || !define_choice_parameter(parameter_set, kParamPrintProfile, "Print", print_profiles, 1, 0)
        || !define_choice_parameter(parameter_set, kParamOutputProfile, "Output", output_profiles, 2, 1)
        || !define_double_parameter(parameter_set, kParamExposure, "Exposure", 0.0, -8.0, 8.0)
        || !define_double_parameter(parameter_set, kParamPushPull, "Push / Pull", 0.0, -3.0, 3.0)
        || !define_double_parameter(parameter_set, kParamNegativeBleachBypass, "Negative Bleach Bypass", 0.0, 0.0, 1.0)
        || !define_double_parameter(parameter_set, kParamPrintBleachBypass, "Print Bleach Bypass", 0.0, 0.0, 1.0)
        || !define_double_parameter(parameter_set, kParamPrinterLightRed, "Printer Light Red", 25.0, 0.0, 50.0)
        || !define_double_parameter(parameter_set, kParamPrinterLightGreen, "Printer Light Green", 25.0, 0.0, 50.0)
        || !define_double_parameter(parameter_set, kParamPrinterLightBlue, "Printer Light Blue", 25.0, 0.0, 50.0)
        || !define_double_parameter(parameter_set, kParamPrinterTemperature, "Printer Temperature", 3200.0, 1000.0, 10000.0)
        || !define_double_parameter(parameter_set, kParamMiddleGray, "Middle Gray", 0.18, 0.01, 1.0)
        || !define_integer_parameter(parameter_set, kParamLutSize, "LUT Size", 33, 17, 65)
        || !define_boolean_parameter(parameter_set, kParamEnableGrain, "Enable Grain", 0)
        || !define_double_parameter(parameter_set, kParamNegativeGrain, "Negative Grain", 0.0, 0.0, 2.0)
        || !define_double_parameter(parameter_set, kParamPrintGrain, "Print Grain", 0.0, 0.0, 2.0)
        || !define_double_parameter(parameter_set, kParamGrainSize, "Grain Size", 1.0, 0.1, 5.0)
        || !define_double_parameter(parameter_set, kParamGrainChroma, "Grain Chroma", 1.0, 0.0, 2.0)
        || !define_integer_parameter(parameter_set, kParamGrainSeed, "Grain Seed", 1, 0, 1000000)
        || !define_boolean_parameter(parameter_set, kParamEnableHalation, "Enable Halation", 0)
        || !define_double_parameter(parameter_set, kParamHalationStrength, "Halation Strength", 0.0, 0.0, 1.0)
        || !define_double_parameter(parameter_set, kParamHalationRadius, "Halation Radius", 12.0, 0.0, 200.0)
        || !define_double_parameter(parameter_set, kParamHalationThreshold, "Halation Threshold", 0.7, 0.0, 4.0)
        || !define_integer_parameter(parameter_set, kParamWorkerThreads, "Worker Threads", 0, 0, 64)) {

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

    source_frame.data =
        static_cast<float*>(
            source_data);

    output_frame.data =
        static_cast<float*>(
            output_data);

    FilmVizOfxRenderSettings settings;
    bool enabled = true;

    if (!read_settings(
            *instance,
            time,
            settings,
            enabled)) {

        release_images();
        return kOfxStatFailed;
    }

    if (!enabled) {
        const int x1 =
            std::max(
                render_window[0],
                std::max(
                    source_frame.x1,
                    output_frame.x1));

        const int y1 =
            std::max(
                render_window[1],
                std::max(
                    source_frame.y1,
                    output_frame.y1));

        const int x2 =
            std::min(
                render_window[2],
                std::min(
                    source_frame.x2,
                    output_frame.x2));

        const int y2 =
            std::min(
                render_window[3],
                std::min(
                    source_frame.y2,
                    output_frame.y2));

        for (int y = y1; y < y2; ++y) {
            const char* src_row =
                reinterpret_cast<const char*>(
                    source_frame.data)
                + static_cast<std::ptrdiff_t>(
                    y - source_frame.y1)
                    * source_frame.row_bytes;

            char* dst_row =
                reinterpret_cast<char*>(
                    output_frame.data)
                + static_cast<std::ptrdiff_t>(
                    y - output_frame.y1)
                    * output_frame.row_bytes;

            const float* src =
                reinterpret_cast<const float*>(
                    src_row)
                + static_cast<std::ptrdiff_t>(
                    x1 - source_frame.x1) * 4;

            float* dst =
                reinterpret_cast<float*>(
                    dst_row)
                + static_cast<std::ptrdiff_t>(
                    x1 - output_frame.x1) * 4;

            for (int x = x1; x < x2; ++x) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];

                src += 4;
                dst += 4;
            }
        }

        release_images();
        return kOfxStatOK;
    }

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
        return error == "render aborted"
            ? kOfxStatOK
            : kOfxStatFailed;
    }

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
        return fetch_suites()
            ? kOfxStatOK
            : kOfxStatErrMissingHostFeature;
    }

    if (std::strcmp(
            action,
            kOfxActionUnload) == 0) {
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
    0,
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
