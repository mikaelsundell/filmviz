// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#include "colorprofilecatalog.h"
#include "inputtransform.h"
#include "outputtransform.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {

// Locale-independent, case-insensitive ordering for the catalog's ASCII labels.
bool alphabetic_less(const std::string& left, const std::string& right)
{
    const auto fold = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
    };
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [&](unsigned char a, unsigned char b) { return fold(a) < fold(b); });
}

} // namespace

std::vector<ColorProfileCatalog::Entry>
ColorProfileCatalog::profiles(bool output, const std::string& resources)
{
    // Explicit labels for the bundled Studio config. Unknown spaces remain
    // accessible as a single "As configured" choice rather than guessing a curve.
    static const struct { const char* profile; const char* space; const char* transfer; } labels[] = {
        {"ACES2065-1", "ACES AP0", "Linear"},
        {"ap0-linear", "ACES AP0", "Linear"},
        {"rec709-gamma24", "Rec.709 (FilmViz preview)", "Gamma 2.4"},
        {"ACEScg", "ACES AP1", "Linear"},
        {"ACEScc", "ACES AP1", "ACEScc"},
        {"ACEScct", "ACES AP1", "ACEScct"},
        {"ADX10", "ADX density", "ADX10"},
        {"ADX16", "ADX density", "ADX16"},
        {"Apple Log", "Rec.2020", "Apple Log"},
        {"ARRI LogC3 (EI800)", "ARRI Wide Gamut 3", "LogC3 (EI800)"},
        {"ARRI LogC4", "ARRI Wide Gamut 4", "LogC4"},
        {"BMDFilm WideGamut Gen5", "BMD WideGamut Gen5", "BMDFilm Gen5 Log"},
        {"DaVinci Intermediate WideGamut", "DaVinci WideGamut", "DaVinci Intermediate"},
        {"CanonLog2 CinemaGamut D55", "CinemaGamut D55", "Canon Log2"},
        {"CanonLog3 CinemaGamut D55", "CinemaGamut D55", "Canon Log3"},
        {"D-Log D-Gamut", "D-Gamut", "D-Log"},
        {"V-Log V-Gamut", "V-Gamut", "V-Log"},
        {"Log3G10 REDWideGamutRGB", "REDWideGamutRGB", "Log3G10"},
        {"Camera Rec.709", "Rec.709", "Rec.709"},
        {"CIE XYZ-D65 - Scene-referred", "CIE XYZ D65", "Linear"},
        {"Raw", "Raw / data", "Untransformed"},
        {"CIE XYZ-D65 - Display-referred", "CIE XYZ D65 (Display)", "Linear"},
        {"sRGB - Display", "Rec.709 (Display)", "sRGB"},
        {"Gamma 2.2 Rec.709 - Display", "Rec.709 (Display)", "Gamma 2.2"},
        {"Rec.1886 Rec.709 - Display", "Rec.709 (Display)", "Rec.1886"},
        {"Display P3 - Display", "P3-D65 (Display)", "sRGB"},
        {"Display P3 HDR - Display", "P3-D65 (Display)", "Extended sRGB (HDR)"},
        {"P3-D65 - Display", "P3-D65 (Display)", "Gamma 2.6"},
        {"ST2084-P3-D65 - Display", "P3-D65 (Display)", "PQ (ST 2084)"},
        {"Rec.2100-HLG - Display", "Rec.2020 (Display)", "HLG"},
        {"Rec.2100-PQ - Display", "Rec.2020 (Display)", "PQ (ST 2084)"},
        {"S-Log3 S-Gamut3", "S-Gamut3", "S-Log3"},
        {"S-Log3 S-Gamut3.Cine", "S-Gamut3.Cine", "S-Log3"},
        {"S-Log3 Venice S-Gamut3", "Venice S-Gamut3", "S-Log3"},
        {"S-Log3 Venice S-Gamut3.Cine", "Venice S-Gamut3.Cine", "S-Log3"},
        {"Linear ARRI Wide Gamut 3", "ARRI Wide Gamut 3", "Linear"},
        {"Linear ARRI Wide Gamut 4", "ARRI Wide Gamut 4", "Linear"},
        {"Linear BMD WideGamut Gen5", "BMD WideGamut Gen5", "Linear"},
        {"Linear DaVinci WideGamut", "DaVinci WideGamut", "Linear"},
        {"Linear CinemaGamut D55", "CinemaGamut D55", "Linear"},
        {"Linear D-Gamut", "D-Gamut", "Linear"},
        {"Linear V-Gamut", "V-Gamut", "Linear"},
        {"Linear REDWideGamutRGB", "REDWideGamutRGB", "Linear"},
        {"Linear S-Gamut3", "S-Gamut3", "Linear"},
        {"Linear S-Gamut3.Cine", "S-Gamut3.Cine", "Linear"},
        {"Linear Venice S-Gamut3", "Venice S-Gamut3", "Linear"},
        {"Linear Venice S-Gamut3.Cine", "Venice S-Gamut3.Cine", "Linear"},
        {"Linear AdobeRGB", "AdobeRGB", "Linear"},
        {"Linear P3-D65", "P3-D65", "Linear"},
        {"Linear Rec.2020", "Rec.2020", "Linear"},
        {"Linear Rec.709 (sRGB)", "Rec.709", "Linear"},
        {"sRGB Encoded Rec.709 (sRGB)", "Rec.709", "sRGB"},
        {"sRGB Encoded P3-D65", "P3-D65", "sRGB"},
        {"Gamma 2.2 Encoded AdobeRGB", "AdobeRGB", "Gamma 2.2"},
        {"sRGB Encoded AP1", "ACES AP1", "sRGB"},
        {"Gamma 2.2 Encoded AP1", "ACES AP1", "Gamma 2.2"},
        {"Gamma 1.8 Encoded Rec.709", "Rec.709", "Gamma 1.8"},
        {"Gamma 2.2 Encoded Rec.709", "Rec.709", "Gamma 2.2"},
        {"Gamma 2.4 Encoded Rec.709", "Rec.709", "Gamma 2.4"},
    };
    const auto& names = output ? OutputTransform::profiles(resources) : InputTransform::profiles(resources);
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < names.size(); ++i) {
        Entry entry{names[i], "As configured", names[i], static_cast<int>(i), {static_cast<int>(i)}};
        for (const auto& label : labels) {
            if (names[i] == label.profile) {
                entry.color_space = label.space;
                entry.transfer_function = label.transfer;
                break;
            }
        }
        auto duplicate = std::find_if(entries.begin(), entries.end(), [&](const Entry& value) {
            return value.color_space == entry.color_space && value.transfer_function == entry.transfer_function;
        });
        if (duplicate == entries.end()) entries.push_back(std::move(entry));
        else duplicate->equivalent_indices.push_back(static_cast<int>(i));
    }
    // Sort presentation metadata only; persistent OCIO profile indices stay fixed.
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (alphabetic_less(a.color_space, b.color_space)) return true;
        if (alphabetic_less(b.color_space, a.color_space)) return false;
        return alphabetic_less(a.transfer_function, b.transfer_function);
    });
    return entries;
}
std::vector<std::string> ColorProfileCatalog::color_spaces(const std::vector<Entry>& entries)
{
    std::vector<std::string> result;
    for (const auto& entry : entries)
        if (std::find(result.begin(), result.end(), entry.color_space) == result.end()) result.push_back(entry.color_space);
    std::stable_sort(result.begin(), result.end(), alphabetic_less);
    return result;
}
const ColorProfileCatalog::Entry* ColorProfileCatalog::find(const std::vector<Entry>& entries, int profile_index)
{
    for (const auto& entry : entries)
        if (std::find(entry.equivalent_indices.begin(), entry.equivalent_indices.end(), profile_index)
            != entry.equivalent_indices.end()) return &entry;
    return nullptr;
}
std::string ColorProfileCatalog::resolve(bool output, const std::string& color_space,
    const std::string& transfer_function, const std::string& resources)
{
    for (const auto& entry : profiles(output, resources))
        if (entry.color_space == color_space && entry.transfer_function == transfer_function) return entry.profile;
    throw std::invalid_argument("Unsupported color space / transfer function: " + color_space + " / " + transfer_function);
}
