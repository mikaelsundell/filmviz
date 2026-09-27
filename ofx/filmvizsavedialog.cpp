// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#include "filmvizsavedialog.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

std::string filmviz_save_lut_dialog(std::string& error)
{
    error.clear();
#ifdef _WIN32
    wchar_t path[32768] = L"FilmViz.cube";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"Cube LUT (*.cube)\0*.cube\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrDefExt = L"cube";
    dialog.lpstrTitle = L"Export FilmViz LUT";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&dialog)) {
        if (CommDlgExtendedError()) error = "Could not open the LUT save dialog.";
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) { error = "Could not encode LUT filename."; return {}; }
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), bytes, nullptr, nullptr);
    result.pop_back();
    return result;
#else
    error = "Native LUT save dialogs are supported on macOS and Windows.";
    return {};
#endif
}
