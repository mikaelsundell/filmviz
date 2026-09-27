// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

// FilmViz OpenFX front end with Metal and OpenCL render backends.


#include "colorprofilecatalog.h"
#include "outputtransform.h"
#include "inputtransform.h"
#include <exception>
#include <stdexcept>

#include "ofxCore.h"
#include "filmvizofxprocessor.h"
#include "filmvizsavedialog.h"
#include "filmvizofxlog.h"
#include "filmcolorresponse.h"
#include "filmformat.h"
#include "negativeprofile.h"
#include "printprofile.h"

#if FILMVIZ_HAS_METAL
#include "filmvizdirectmetalprocessor.h"
#endif
#if FILMVIZ_HAS_OPENCL
#include "filmvizdirectopenclprocessor.h"
#endif

#include "ofxImageEffect.h"
#include "ofxGPURender.h"
#include "ofxMessage.h"
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
constexpr const char* kPluginLabel = "FilmViz " FILMVIZ_VERSION_STRING;
constexpr const char* kPluginGrouping = "FilmViz";

constexpr const char* kParamInputProfile = "inputProfile";
constexpr const char* kParamInputSpace = "inputColorSpace";
constexpr const char* kParamInputTransfer = "inputTransferFunction";
constexpr const char* kParamOutputSpace = "outputColorSpace";
constexpr const char* kParamOutputTransfer = "outputTransferFunction";
constexpr const char* kParamNegativeProfile = "negativeProfile";
constexpr const char* kParamPrintProfile = "printProfile";
constexpr const char* kParamOutputProfile = "outputProfile";
constexpr const char* kParamExposure = "exposure";
constexpr const char* kParamNegativeFlash = "negativeFlash";
constexpr const char* kParamPrintFlash = "printFlash";
constexpr const char* kParamPushPull = "pushPull";
constexpr const char* kParamColorDensity = "colorDensity";
constexpr const char* kParamColorDepth = "colorDepth";
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

constexpr const char* kGroupSetup = "groupSetup";
constexpr const char* kGroupNegative = "groupNegative";
constexpr const char* kGroupPrint = "groupPrint";
constexpr const char* kGroupSpatial = "groupSpatial";
constexpr const char* kGroupGrain = "groupGrain";
constexpr const char* kGroupHalation = "groupHalation";
constexpr const char* kGroupAdvanced = "groupAdvanced";

OfxHost* gHost = nullptr;
const OfxImageEffectSuiteV1* gEffectSuite = nullptr;
const OfxPropertySuiteV1* gPropertySuite = nullptr;
const OfxParameterSuiteV1* gParameterSuite = nullptr;
const OfxMessageSuiteV2* gMessageSuite = nullptr;
const OfxMessageSuiteV1* gMessageSuiteV1 = nullptr;

void
set_gpu_error(
    OfxImageEffectHandle effect,
    const std::string& message)
{
    if (gMessageSuite && gMessageSuite->setPersistentMessage) {
        gMessageSuite->setPersistentMessage(
            effect,
            kOfxMessageError,
            "filmviz.gpu",
            "%s",
            message.c_str());
    }
    else if (gMessageSuiteV1 && gMessageSuiteV1->message) {
        gMessageSuiteV1->message(
            effect,
            kOfxMessageError,
            "filmviz.gpu",
            "%s",
            message.c_str());
    }
}

void
clear_gpu_error(
    OfxImageEffectHandle effect)
{
    if (gMessageSuite && gMessageSuite->clearPersistentMessage) {
        gMessageSuite->clearPersistentMessage(effect);
    }
}

struct InstanceData
{
    OfxImageClipHandle source_clip = nullptr;
    OfxImageClipHandle output_clip = nullptr;

    OfxParamHandle input = nullptr;
    OfxParamHandle input_space = nullptr;
    OfxParamHandle input_transfer = nullptr;
    OfxParamHandle output_space = nullptr;
    OfxParamHandle output_transfer = nullptr;
    std::vector<ColorProfileCatalog::Entry> input_entries;
    std::vector<ColorProfileCatalog::Entry> output_entries;
    bool syncing_color_controls = false;
    OfxParamHandle negative = nullptr;
    OfxParamHandle print = nullptr;
    OfxParamHandle output = nullptr;
    OfxParamHandle exposure = nullptr;
    OfxParamHandle negative_flash = nullptr;
    OfxParamHandle print_flash = nullptr;
    OfxParamHandle push_pull = nullptr;
    OfxParamHandle color_density = nullptr;
    OfxParamHandle color_depth = nullptr;
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

    OfxParamHandle grainShadows = nullptr;
    OfxParamHandle grainMidtones = nullptr;
    OfxParamHandle grainHighlights = nullptr;
    OfxParamHandle response_response_amount = nullptr;
    OfxParamHandle response_chroma_compression = nullptr;
    OfxParamHandle response_chroma_knee = nullptr;
    OfxParamHandle response_density_center = nullptr;
    OfxParamHandle response_density_width = nullptr;
    OfxParamHandle response_warm_protection = nullptr;
    OfxParamHandle response_warm_hue_center = nullptr;
    OfxParamHandle response_warm_hue_width = nullptr;
    OfxParamHandle response_warm_hue_shift = nullptr;
    OfxParamHandle grainTonalEnabled = nullptr;
    OfxParamHandle responseEnabled = nullptr;
    OfxParamHandle fullQualityEffects = nullptr;
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
#if FILMVIZ_HAS_METAL
    FilmVizDirectMetalProcessor direct_metal_processor;
#endif
#if FILMVIZ_HAS_OPENCL
    FilmVizDirectOpenCLProcessor direct_opencl_processor;
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

// The persistent profile remains authoritative for rendering and old projects.
// The two visible selectors are derived, non-animating UI controls.
void
sync_color_controls(InstanceData& instance, bool output)
{
    const auto& entries = output ? instance.output_entries : instance.input_entries;
    const auto spaces = ColorProfileCatalog::color_spaces(entries);
    const auto profile = output ? instance.output : instance.input;
    const auto space = output ? instance.output_space : instance.input_space;
    const auto transfer = output ? instance.output_transfer : instance.input_transfer;
    int index = output ? 1 : 0;
    gParameterSuite->paramGetValue(profile, &index);
    const auto* entry = ColorProfileCatalog::find(entries, index);
    if (!entry) throw std::runtime_error("Unknown saved OCIO profile index");
    OfxPropertySetHandle properties = nullptr;
    gParameterSuite->paramGetPropertySet(transfer, &properties);
    gPropertySuite->propReset(properties, kOfxParamPropChoiceOption);
    int transfer_index = 0, selected_transfer = 0;
    int selected_space = 0;
    for (std::size_t i = 0; i < spaces.size(); ++i)
        if (spaces[i] == entry->color_space) selected_space = static_cast<int>(i);
    for (const auto& choice : entries) {
        if (choice.color_space != entry->color_space) continue;
        gPropertySuite->propSetString(properties, kOfxParamPropChoiceOption,
            transfer_index, choice.transfer_function.c_str());
        if (choice.transfer_function == entry->transfer_function) selected_transfer = transfer_index;
        ++transfer_index;
    }
    gParameterSuite->paramSetValue(space, selected_space);
    gParameterSuite->paramSetValue(transfer, selected_transfer);
}

OfxStatus
color_controls_changed(OfxImageEffectHandle effect, OfxPropertySetHandle in_args)
{
    auto* instance = instance_data(effect);
    if (!instance || instance->syncing_color_controls) return kOfxStatReplyDefault;
    char* name = nullptr;
    if (gPropertySuite->propGetString(in_args, kOfxPropName, 0, &name) != kOfxStatOK || !name)
        return kOfxStatReplyDefault;
    const bool output = std::strcmp(name, kParamOutputSpace) == 0
        || std::strcmp(name, kParamOutputTransfer) == 0 || std::strcmp(name, kParamOutputProfile) == 0;
    if (!output && std::strcmp(name, kParamInputSpace) != 0
        && std::strcmp(name, kParamInputTransfer) != 0 && std::strcmp(name, kParamInputProfile) != 0)
        return kOfxStatReplyDefault;
    struct Guard {
        bool& flag;
        explicit Guard(bool& value) : flag(value) { flag = true; }
        ~Guard() { flag = false; }
    } guard(instance->syncing_color_controls);
    const auto& entries = output ? instance->output_entries : instance->input_entries;
    const auto profile = output ? instance->output : instance->input;
    if (std::strcmp(name, output ? kParamOutputProfile : kParamInputProfile) != 0) {
        const auto spaces = ColorProfileCatalog::color_spaces(entries);
        int space_index = 0, transfer_index = 0, old_profile = 0;
        gParameterSuite->paramGetValue(output ? instance->output_space : instance->input_space, &space_index);
        gParameterSuite->paramGetValue(output ? instance->output_transfer : instance->input_transfer, &transfer_index);
        gParameterSuite->paramGetValue(profile, &old_profile);
        const auto* previous = ColorProfileCatalog::find(entries, old_profile);
        const auto& selected_space = spaces.at(static_cast<std::size_t>(space_index));
        const bool space_changed = std::strcmp(name, output ? kParamOutputSpace : kParamInputSpace) == 0;
        const ColorProfileCatalog::Entry* selected = nullptr;
        int index = 0;
        for (const auto& entry : entries) {
            if (entry.color_space != selected_space) continue;
            if (!selected) selected = &entry;
            if ((space_changed && previous && entry.transfer_function == previous->transfer_function)
                || (!space_changed && index == transfer_index)) {
                selected = &entry;
                break;
            }
            ++index;
        }
        if (!selected) return kOfxStatFailed;
        gParameterSuite->paramSetValue(profile, selected->profile_index);
    }
    sync_color_controls(*instance, output);
    return kOfxStatOK;
}

bool
read_settings(
    InstanceData& instance,
    double time,
    FilmVizOfxRenderSettings& settings)
{
    int input = 0;
    int negative = 0;
    int print = 0;
    int output = 1;
    int film_format = 5;

    int grain_enabled = 0;
    int grain_seed = 1;
    int halation_enabled = 0;

    double exposure = 0.0;
    double negative_flash = 0.0;
    double print_flash = 0.0;
    double push_pull = 0.0;
    double color_density = 0.0;
    double color_depth = FilmColorResponse::standard_color_depth;
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
        gParameterSuite->paramGetValueAtTime(instance.input, time, &input),
        gParameterSuite->paramGetValueAtTime(instance.negative, time, &negative),
        gParameterSuite->paramGetValueAtTime(instance.print, time, &print),
        gParameterSuite->paramGetValueAtTime(instance.output, time, &output),
        gParameterSuite->paramGetValueAtTime(instance.exposure, time, &exposure),
        gParameterSuite->paramGetValueAtTime(instance.negative_flash, time, &negative_flash),
        gParameterSuite->paramGetValueAtTime(instance.print_flash, time, &print_flash),
        gParameterSuite->paramGetValueAtTime(instance.push_pull, time, &push_pull),
        gParameterSuite->paramGetValueAtTime(instance.color_density, time, &color_density),
        gParameterSuite->paramGetValueAtTime(instance.color_depth, time, &color_depth),
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
    settings.threads = 0; // Automatic worker selection for the OFX adapter.
    settings.exposure_stops = static_cast<float>(exposure);
    settings.negative_flash_percent = static_cast<float>(negative_flash);
    settings.print_flash_percent = static_cast<float>(print_flash);
    settings.push_pull_stops = static_cast<float>(push_pull);
    settings.color_density = static_cast<float>(color_density);
    settings.color_depth = static_cast<float>(color_depth);
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

    double value_grainShadows = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.grainShadows, time, &value_grainShadows) != kOfxStatOK) return false;
    settings.grain_shadows = static_cast<float>(value_grainShadows);
    double value_grainMidtones = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.grainMidtones, time, &value_grainMidtones) != kOfxStatOK) return false;
    settings.grain_midtones = static_cast<float>(value_grainMidtones);
    double value_grainHighlights = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.grainHighlights, time, &value_grainHighlights) != kOfxStatOK) return false;
    settings.grain_highlights = static_cast<float>(value_grainHighlights);
    double value_response_response_amount = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_response_amount, time, &value_response_response_amount) != kOfxStatOK) return false;
    settings.color_response.response_amount = static_cast<float>(value_response_response_amount);
    double value_response_chroma_compression = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_chroma_compression, time, &value_response_chroma_compression) != kOfxStatOK) return false;
    settings.color_response.chroma_compression = static_cast<float>(value_response_chroma_compression);
    double value_response_chroma_knee = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_chroma_knee, time, &value_response_chroma_knee) != kOfxStatOK) return false;
    settings.color_response.chroma_knee = static_cast<float>(value_response_chroma_knee);
    double value_response_density_center = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_density_center, time, &value_response_density_center) != kOfxStatOK) return false;
    settings.color_response.density_center = static_cast<float>(value_response_density_center);
    double value_response_density_width = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_density_width, time, &value_response_density_width) != kOfxStatOK) return false;
    settings.color_response.density_width = static_cast<float>(value_response_density_width);
    double value_response_warm_protection = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_warm_protection, time, &value_response_warm_protection) != kOfxStatOK) return false;
    settings.color_response.warm_protection = static_cast<float>(value_response_warm_protection);
    double value_response_warm_hue_center = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_warm_hue_center, time, &value_response_warm_hue_center) != kOfxStatOK) return false;
    settings.color_response.warm_hue_center = static_cast<float>(value_response_warm_hue_center);
    double value_response_warm_hue_width = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_warm_hue_width, time, &value_response_warm_hue_width) != kOfxStatOK) return false;
    settings.color_response.warm_hue_width = static_cast<float>(value_response_warm_hue_width);
    double value_response_warm_hue_shift = 0.0;
    if (gParameterSuite->paramGetValueAtTime(instance.response_warm_hue_shift, time, &value_response_warm_hue_shift) != kOfxStatOK) return false;
    settings.color_response.warm_hue_shift = static_cast<float>(value_response_warm_hue_shift);
    int tonal = 1, response_enabled = 1;
    if (gParameterSuite->paramGetValueAtTime(instance.grainTonalEnabled, time, &tonal) != kOfxStatOK
        || gParameterSuite->paramGetValueAtTime(instance.responseEnabled, time, &response_enabled) != kOfxStatOK) return false;
    settings.grain_tonal_enabled = tonal != 0;
    if (!response_enabled) settings.color_response.response_amount = 0.0f;
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
        fetch_param(parameter_set, kParamInputProfile, instance->input)
        && fetch_param(parameter_set, kParamInputSpace, instance->input_space)
        && fetch_param(parameter_set, kParamInputTransfer, instance->input_transfer)
        && fetch_param(parameter_set, kParamOutputSpace, instance->output_space)
        && fetch_param(parameter_set, kParamOutputTransfer, instance->output_transfer)
        && fetch_param(parameter_set, kParamNegativeProfile, instance->negative)
        && fetch_param(parameter_set, kParamPrintProfile, instance->print)
        && fetch_param(parameter_set, kParamOutputProfile, instance->output)
        && fetch_param(parameter_set, kParamExposure, instance->exposure)
        && fetch_param(parameter_set, kParamNegativeFlash, instance->negative_flash)
        && fetch_param(parameter_set, kParamPrintFlash, instance->print_flash)
        && fetch_param(parameter_set, kParamPushPull, instance->push_pull)
        && fetch_param(parameter_set, kParamColorDensity, instance->color_density)
        && fetch_param(parameter_set, kParamColorDepth, instance->color_depth)
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
        && fetch_param(parameter_set, "grainShadows", instance->grainShadows)
        && fetch_param(parameter_set, "grainMidtones", instance->grainMidtones)
        && fetch_param(parameter_set, "grainHighlights", instance->grainHighlights)
        && fetch_param(parameter_set, "response_response_amount", instance->response_response_amount)
        && fetch_param(parameter_set, "response_chroma_compression", instance->response_chroma_compression)
        && fetch_param(parameter_set, "response_chroma_knee", instance->response_chroma_knee)
        && fetch_param(parameter_set, "response_density_center", instance->response_density_center)
        && fetch_param(parameter_set, "response_density_width", instance->response_density_width)
        && fetch_param(parameter_set, "response_warm_protection", instance->response_warm_protection)
        && fetch_param(parameter_set, "response_warm_hue_center", instance->response_warm_hue_center)
        && fetch_param(parameter_set, "response_warm_hue_width", instance->response_warm_hue_width)
        && fetch_param(parameter_set, "response_warm_hue_shift", instance->response_warm_hue_shift)
        && fetch_param(parameter_set, "grainTonalEnabled", instance->grainTonalEnabled)
        && fetch_param(parameter_set, "responseEnabled", instance->responseEnabled)
        && fetch_param(parameter_set, "fullQualityEffects", instance->fullQualityEffects)
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

    instance->input_entries = ColorProfileCatalog::profiles(false, instance->resources_directory);
    instance->output_entries = ColorProfileCatalog::profiles(true, instance->resources_directory);
    instance->syncing_color_controls = true;
    sync_color_controls(*instance, false);
    sync_color_controls(*instance, true);
    instance->syncing_color_controls = false;

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
        FilmVizOfxLog::memory("memory_node_created", stream.str());
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
    {
        std::ostringstream stream;
        stream << "node=" << pointer;
        FilmVizOfxLog::memory("memory_node_destroyed", stream.str());
    }

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

    gMessageSuite =
        reinterpret_cast<const OfxMessageSuiteV2*>(
            gHost->fetchSuite(
                gHost->host,
                kOfxMessageSuite,
                2));

    gMessageSuiteV1 =
        gMessageSuite
            ? reinterpret_cast<const OfxMessageSuiteV1*>(gMessageSuite)
            : reinterpret_cast<const OfxMessageSuiteV1*>(
                gHost->fetchSuite(
                    gHost->host,
                    kOfxMessageSuite,
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
        kOfxPropIcon,
        0,
        "icons/filmviz.svg");

    gPropertySuite->propSetString(
        properties,
        kOfxPropIcon,
        1,
        "icons/filmviz.png");

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

#if FILMVIZ_HAS_OPENCL
#ifdef kOfxImageEffectPropOpenCLRenderSupported
    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropOpenCLRenderSupported,
        0,
        "true");
#endif
#endif

#ifdef kOfxImageEffectPropCPURenderSupported
    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPropCPURenderSupported,
        0,
        "false");
#endif

    gPropertySuite->propSetString(
        properties,
        kOfxImageEffectPluginRenderThreadSafety,
        0,
        kOfxImageEffectRenderInstanceSafe);

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

    if (std::strcmp(name, "fullQualityEffects") == 0)
        gPropertySuite->propSetString(properties, kOfxParamPropHint, 0,
            "Enable grain and halation at their stored strengths for non-draft renders. "
            "This can include the viewer: OFX cannot reliably distinguish final export. "
            "Hosts without a draft flag always qualify. Leave off for manual control.");
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

    if (std::strcmp(name, kParamInputProfile) == 0 || std::strcmp(name, kParamOutputProfile) == 0) {
        gPropertySuite->propSetInt(properties, kOfxParamPropSecret, 0, 1);
    } else if (std::strcmp(name, kParamInputSpace) == 0 || std::strcmp(name, kParamInputTransfer) == 0
        || std::strcmp(name, kParamOutputSpace) == 0 || std::strcmp(name, kParamOutputTransfer) == 0) {
        gPropertySuite->propSetInt(properties, kOfxParamPropAnimates, 0, 0);
        gPropertySuite->propSetInt(properties, kOfxParamPropPersistant, 0, 0);
        gPropertySuite->propSetInt(properties, kOfxParamPropEvaluateOnChange, 0, 0);
        gPropertySuite->propSetInt(properties, kOfxParamPropCanUndo, 0, 0);
    }

    return set_parameter_parent(properties, parent);
}

bool
define_color_selectors(OfxParamSetHandle parameters, bool output)
{
    const auto entries = ColorProfileCatalog::profiles(output, bundle_resources_directory());
    const auto spaces = ColorProfileCatalog::color_spaces(entries);
    const auto* initial = ColorProfileCatalog::find(entries, output ? 1 : 0);
    if (!initial) return false;
    std::vector<const char*> options, transfers;
    int default_space = 0, default_transfer = 0;
    for (std::size_t i = 0; i < spaces.size(); ++i) {
        options.push_back(spaces[i].c_str());
        if (spaces[i] == initial->color_space) default_space = static_cast<int>(i);
    }
    for (const auto& entry : entries) {
        if (entry.color_space != initial->color_space) continue;
        if (entry.transfer_function == initial->transfer_function) default_transfer = static_cast<int>(transfers.size());
        transfers.push_back(entry.transfer_function.c_str());
    }
    const char* space_name = output ? kParamOutputSpace : kParamInputSpace;
    const char* transfer_name = output ? kParamOutputTransfer : kParamInputTransfer;
    if (!define_choice_parameter(parameters, space_name, output ? "Output Color Space" : "Input Color Space",
            options.data(), static_cast<int>(options.size()), default_space, kGroupSetup)
        || !define_choice_parameter(parameters, transfer_name, output ? "Output Transfer Function" : "Input Transfer Function",
            transfers.data(), static_cast<int>(transfers.size()), default_transfer, kGroupSetup)) return false;
    return true;
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

bool
define_lut_export_parameters(OfxParamSetHandle parameters)
{
    if (!define_group_parameter(parameters, "groupLutExport", "LUT Export", false)) return false;
    const char* sizes[] = {"17", "33", "65"};
    OfxPropertySetHandle properties = nullptr;
    if (gParameterSuite->paramDefine(parameters, kOfxParamTypeChoice, "lutExportSize", &properties) != kOfxStatOK) return false;
    set_parameter_parent(properties, "groupLutExport");
    gPropertySuite->propSetString(properties, kOfxPropLabel, 0, "LUT Size");
    for (int index = 0; index < 3; ++index)
        gPropertySuite->propSetString(properties, kOfxParamPropChoiceOption, index, sizes[index]);
    gPropertySuite->propSetInt(properties, kOfxParamPropDefault, 0, 1);
    gPropertySuite->propSetInt(properties, kOfxParamPropAnimates, 0, 0);
    gPropertySuite->propSetInt(properties, kOfxParamPropEvaluateOnChange, 0, 0);
    if (gParameterSuite->paramDefine(parameters, kOfxParamTypePushButton, "exportLut", &properties) != kOfxStatOK) return false;
    set_parameter_parent(properties, "groupLutExport");
    gPropertySuite->propSetString(properties, kOfxPropLabel, 0, "Export LUT");
    gPropertySuite->propSetInt(properties, kOfxParamPropEvaluateOnChange, 0, 0);
    gPropertySuite->propSetString(properties, kOfxParamPropHint, 0,
        "Export this node's color transform at the current frame, including input/output profiles. "
        "Input domain 0..1. Excludes grain, halation and MTF. Existing files are never overwritten. "
        "Generation is synchronous and may take some time.");
    return true;
}

OfxStatus
export_lut(OfxImageEffectHandle effect, OfxPropertySetHandle arguments)
{
    char* reason = nullptr;
    if (gPropertySuite->propGetString(arguments, kOfxPropChangeReason, 0, &reason) != kOfxStatOK
        || !reason || std::strcmp(reason, kOfxChangeUserEdited) != 0) return kOfxStatOK;
    auto* instance = instance_data(effect);
    if (!instance) return kOfxStatFailed;
    double time = 0.0;
    gPropertySuite->propGetDouble(arguments, kOfxPropTime, 0, &time);
    FilmVizOfxRenderSettings settings;
    OfxParamSetHandle parameters = nullptr;
    OfxParamHandle size_handle = nullptr;
    int size_index = 1;
    if (!read_settings(*instance, time, settings)
        || gEffectSuite->getParamSet(effect, &parameters) != kOfxStatOK
        || !fetch_param(parameters, "lutExportSize", size_handle)
        || gParameterSuite->paramGetValue(size_handle, &size_index) != kOfxStatOK) return kOfxStatFailed;
    const int sizes[] = {17, 33, 65};
    std::string error;
    const std::string path = filmviz_save_lut_dialog(error);
    if (path.empty() && error.empty()) return kOfxStatOK;
    const bool success = !path.empty() && filmviz_export_cube(settings, instance->resources_directory,
        path, sizes[std::clamp(size_index, 0, 2)], error);
    const std::string message = success ? std::string("LUT exported: ") + path : error;
    if (gMessageSuiteV1) gMessageSuiteV1->message(effect,
        success ? kOfxMessageMessage : kOfxMessageError, "FilmVizLUT", "%s", message.c_str());
    return kOfxStatOK;
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

    const auto& inputs = InputTransform::profiles(bundle_resources_directory());
    std::vector<const char*> input_profiles;
    for (const auto& input : inputs) input_profiles.push_back(input.c_str());

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

    const auto& outputs = OutputTransform::profiles(bundle_resources_directory());
    std::vector<const char*> output_profiles;
    for (const auto& output : outputs) output_profiles.push_back(output.c_str());

    if (!define_group_parameter(parameter_set, kGroupSetup, "Pipeline", true)
        || !define_group_parameter(parameter_set, kGroupNegative, "Negative", true)
        || !define_group_parameter(parameter_set, kGroupPrint, "Print", true)
        || !define_group_parameter(parameter_set, kGroupSpatial, "Spatial Response", false)
        || !define_group_parameter(parameter_set, kGroupGrain, "Grain", false)
        || !define_group_parameter(parameter_set, kGroupHalation, "Halation", false)
        || !define_choice_parameter(parameter_set, kParamInputProfile, "Input profile", input_profiles.data(), static_cast<int>(input_profiles.size()), 0, kGroupSetup)
        || !define_color_selectors(parameter_set, false)
        || !define_choice_parameter(parameter_set, kParamNegativeProfile, "Stock", negative_options.data(), static_cast<int>(negative_options.size()), 0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamExposure, "Exposure", 0.0, -8.0, 8.0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamNegativeFlash, "Flash (%)", 0.0, 0.0, 25.0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamPushPull, "Push / Pull", 0.0, -3.0, 3.0, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamColorDensity, "Color Separation", 0.0, FilmColorResponse::minimum_trim, FilmColorResponse::maximum_trim, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamColorDepth, "Color Depth", FilmColorResponse::standard_color_depth, FilmColorResponse::minimum_color_depth, FilmColorResponse::maximum_color_depth, kGroupNegative)
        || !define_double_parameter(parameter_set, kParamNegativeBleachBypass, "Bleach Bypass", 0.0, 0.0, 1.0, kGroupNegative)
        || !define_choice_parameter(parameter_set, kParamPrintProfile, "Stock", print_options.data(), static_cast<int>(print_options.size()), 0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrintFlash, "Flash (%)", 0.0, 0.0, 25.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrintBleachBypass, "Bleach Bypass", 0.0, 0.0, 1.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightRed, "Printer Light Red", 25.0, 0.0, 50.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightGreen, "Printer Light Green", 25.0, 0.0, 50.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightBlue, "Printer Light Blue", 25.0, 0.0, 50.0, kGroupPrint)
        || !define_double_parameter(parameter_set, kParamPrinterLightMaster, "Printer Light Master", 0.0, -25.0, 25.0, kGroupPrint)
        || !define_choice_parameter(parameter_set, kParamOutputProfile, "Output profile", output_profiles.data(), static_cast<int>(output_profiles.size()), 1, kGroupSetup)
        || !define_color_selectors(parameter_set, true)
        || !define_choice_parameter(parameter_set, kParamFilmFormat, "Film Format", format_options.data(), static_cast<int>(format_options.size()), format_default, kGroupSpatial)
        || !define_double_parameter(parameter_set, kParamImageWidthMm, "Custom Image Width (mm)", 24.89, 1.0, 100.0, kGroupSpatial)
        || !define_double_parameter(parameter_set, kParamNegativeMtf, "Negative MTF", 0.0, 0.0, 2.0, kGroupSpatial)
        || !define_double_parameter(parameter_set, kParamPrintMtf, "Print MTF", 0.0, 0.0, 2.0, kGroupSpatial)
        || !define_group_parameter(parameter_set, "groupColorResponse", "Color Response", false)
        || !define_double_parameter(parameter_set, "grainShadows", "Shadow grain", 1, 0, 2, kGroupGrain)
        || !define_double_parameter(parameter_set, "grainMidtones", "Midtone grain", 1, 0, 2, kGroupGrain)
        || !define_double_parameter(parameter_set, "grainHighlights", "Highlight grain", 1, 0, 2, kGroupGrain)
        || !define_double_parameter(parameter_set, "response_response_amount", "Response amount", 1, 0, 2, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_chroma_compression", "Chroma compression", 0.22, 0, 2, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_chroma_knee", "Chroma knee", 0.5, 0.05, 2, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_density_center", "Density center", 1.25, 0, 3, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_density_width", "Density width", 1, 0.25, 3, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_warm_protection", "Warm protection", 0.5, 0, 1, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_warm_hue_center", "Warm hue center (degrees)", 0, -180, 180, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_warm_hue_width", "Warm hue width", 1, 0.25, 3, "groupColorResponse")
        || !define_double_parameter(parameter_set, "response_warm_hue_shift", "Warm hue shift (degrees)", 0, -45, 45, "groupColorResponse")
        || !define_boolean_parameter(parameter_set, "grainTonalEnabled", "Enable tonal grain shaping", 1, kGroupGrain)
        || !define_boolean_parameter(parameter_set, "responseEnabled", "Enable Color Response", 1, "groupColorResponse")
        || !define_boolean_parameter(parameter_set, "fullQualityEffects", "Enable both in full-quality renders", 0, kGroupSpatial)
        || !define_boolean_parameter(parameter_set, kParamEnableGrain, "Enable Grain", 0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamNegativeGrain, "Negative", 0.0, 0.0, 2.0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamPrintGrain, "Print", 0.0, 0.0, 2.0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamGrainSize, "Scale multiplier", 1.0, 0.25, 10.0, kGroupGrain)
        || !define_double_parameter(parameter_set, kParamGrainChroma, "Chroma", 1.0, 0.0, 2.0, kGroupGrain)
        || !define_integer_parameter(parameter_set, kParamGrainSeed, "Seed", 1, 0, 1000000, kGroupGrain)
        || !define_boolean_parameter(parameter_set, kParamEnableHalation, "Enable Halation", 0, kGroupHalation)
        || !define_double_parameter(parameter_set, kParamHalationStrength, "Strength", 0.0, 0.0, 1.0, kGroupHalation)
        || !define_double_parameter(parameter_set, kParamHalationRadius, "Radius", 12.0, 0.0, 200.0, kGroupHalation)
        || !define_double_parameter(parameter_set, kParamHalationThreshold, "Threshold", 0.7, 0.0, 4.0, kGroupHalation)
        || !define_group_parameter(parameter_set, kGroupAdvanced, "Advanced", false)
        || !define_double_parameter(parameter_set, kParamMiddleGray, "Middle Gray", 0.18, 0.01, 1.0, kGroupAdvanced)
        || !define_lut_export_parameters(parameter_set)) {

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

    FilmVizOfxRenderSettings settings;

    if (!read_settings(
            *instance,
            time,
            settings)) {

        release_images();
        return kOfxStatFailed;
    }

    int full_quality_effects = 0;
    int draft = 0;
    gParameterSuite->paramGetValueAtTime(instance->fullQualityEffects, time, &full_quality_effects);
    // OFX defines missing draft quality as full quality. This is not a Deliver detector.
    gPropertySuite->propGetInt(in_args, kOfxImageEffectPropRenderQualityDraft, 0, &draft);
    if (full_quality_effects && !draft) {
        settings.grain_enabled = true;
        settings.halation_enabled = true;
    }

    std::ostringstream render_details;
    render_details
        << "node=" << instance
        << " time=" << time
        << " negative=" << settings.negative_profile
        << " print=" << settings.print_profile
        << " exposure=" << settings.exposure_stops
        << " format=" << settings.film_format
        << " width_mm=" << settings.image_width_mm
        << " negative_mtf=" << settings.negative_mtf_amount
        << " print_mtf=" << settings.print_mtf_amount
        << " grain=" << (settings.grain_enabled ? 1 : 0)
        << " halation=" << (settings.halation_enabled ? 1 : 0);

    FilmVizOfxLog::Scope render_scope(
        "render",
        render_details.str());
    FilmVizOfxLog::memory("memory_render_begin", render_details.str());

    int metal_enabled = 0;
    void* metal_command_queue = nullptr;
    int opencl_enabled = 0;
    void* opencl_command_queue = nullptr;

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

#if FILMVIZ_HAS_OPENCL
#ifdef kOfxImageEffectPropOpenCLEnabled
    gPropertySuite->propGetInt(
        in_args,
        kOfxImageEffectPropOpenCLEnabled,
        0,
        &opencl_enabled);
#endif
#ifdef kOfxImageEffectPropOpenCLCommandQueue
    if (opencl_enabled) {
        gPropertySuite->propGetPointer(
            in_args,
            kOfxImageEffectPropOpenCLCommandQueue,
            0,
            &opencl_command_queue);
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

        const auto log_metal_stage = [&](const char* event) {
            if (!FilmVizOfxLog::memory_enabled()) {
                return;
            }
            std::ostringstream details;
            details << render_details.str()
                    << " metal_device_bytes="
                    << FilmVizDirectMetalProcessor::device_allocated_bytes(
                        metal_command_queue);
            FilmVizOfxLog::memory(event, details.str());
        };
        log_metal_stage("memory_metal_render_begin");
        std::string error;
        const bool configured =
            instance->direct_metal_processor.configure(
                settings,
                instance->resources_directory,
                metal_command_queue,
                error);
        log_metal_stage("memory_metal_configured");
        const bool rendered = configured
            && instance->direct_metal_processor.render(
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
        log_metal_stage("memory_metal_rendered");
        release_images();
        log_metal_stage("memory_images_released");

        if (!rendered) {
            render_scope.finish(
                "backend=metal_direct result=failed error=" + error);
            set_gpu_error(
                effect,
                "FilmViz Metal rendering failed: " + error);
            return kOfxStatGPURenderFailed;
        }

        clear_gpu_error(effect);
        render_scope.finish("backend=metal_direct result=ok");
        return kOfxStatOK;
    }
#endif

#if FILMVIZ_HAS_OPENCL
    if (opencl_enabled
        && opencl_command_queue) {

        FilmVizOfxOpenCLFrame opencl_source;
        opencl_source.x1 = source_frame.x1;
        opencl_source.y1 = source_frame.y1;
        opencl_source.x2 = source_frame.x2;
        opencl_source.y2 = source_frame.y2;
        opencl_source.row_bytes = source_row_bytes;
        opencl_source.buffer = source_data;

        FilmVizOfxOpenCLFrame opencl_output;
        opencl_output.x1 = output_frame.x1;
        opencl_output.y1 = output_frame.y1;
        opencl_output.x2 = output_frame.x2;
        opencl_output.y2 = output_frame.y2;
        opencl_output.row_bytes = output_row_bytes;
        opencl_output.buffer = output_data;

        std::string error;
        const bool configured =
            instance->direct_opencl_processor.configure(
                settings,
                instance->resources_directory,
                opencl_command_queue,
                error);
        FilmVizOfxLog::memory("memory_opencl_configured", render_details.str());
        const bool rendered = configured
            && instance->direct_opencl_processor.render(
                settings,
                opencl_command_queue,
                opencl_source,
                opencl_output,
                render_window[0],
                render_window[1],
                render_window[2],
                render_window[3],
                time,
                error);

        FilmVizOfxLog::memory("memory_opencl_rendered", render_details.str());
        release_images();
        FilmVizOfxLog::memory("memory_images_released", render_details.str());

        if (!rendered) {
            render_scope.finish(
                "backend=opencl_direct result=failed error=" + error);
            set_gpu_error(
                effect,
                "FilmViz OpenCL rendering failed: " + error);
            return kOfxStatGPURenderFailed;
        }

        clear_gpu_error(effect);
        render_scope.finish("backend=opencl_direct result=ok");
        return kOfxStatOK;
    }
#endif

    release_images();

#if defined(_WIN32)
    const std::string error =
        "FilmViz requires Resolve to provide an OpenCL GPU render queue on "
        "Windows. CUDA is not currently supported. Select OpenCL under "
        "Preferences > Memory and GPU.";
#elif defined(__APPLE__)
    const std::string error =
        "FilmViz requires Resolve to provide a Metal or OpenCL GPU render "
        "queue. Select Metal (recommended) or OpenCL under Preferences > "
        "Memory and GPU.";
#else
    const std::string error =
        "FilmViz requires Resolve to provide a supported OpenCL GPU render "
        "queue.";
#endif

    set_gpu_error(effect, error);
    render_scope.finish("backend=unavailable result=failed error=" + error);
    return kOfxStatErrUnsupported;
}

OfxStatus
plugin_main(
    const char* action,
    const void* handle,
    OfxPropertySetHandle in_args,
    OfxPropertySetHandle)
try {
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

    if (std::strcmp(action, kOfxActionInstanceChanged) == 0) {
        char* parameter = nullptr;
        gPropertySuite->propGetString(in_args, kOfxPropName, 0, &parameter);
        if (FilmVizOfxLog::memory_enabled()) {
            std::ostringstream details;
            details << "node=" << effect << " parameter="
                    << (parameter ? parameter : "unknown");
            FilmVizOfxLog::memory("memory_parameter_changed", details.str());
        }
        if (parameter && std::strcmp(parameter, "exportLut") == 0)
            return export_lut(effect, in_args);
        return color_controls_changed(effect, in_args);
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

catch (const std::exception& exception) {
    FilmVizOfxLog::write("error", exception.what());
    return kOfxStatFailed;
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
    FILMVIZ_OFX_VERSION_MAJOR,
    FILMVIZ_OFX_VERSION_MINOR,
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
