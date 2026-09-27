// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.
#include "filmvizsavedialog.h"
#import <AppKit/AppKit.h>
#include <dispatch/dispatch.h>

std::string filmviz_save_lut_dialog(std::string& error)
{
    error.clear();
    __block NSString* selected = nil;
    void (^showPanel)(void) = ^{
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = @"Export FilmViz LUT";
        panel.nameFieldStringValue = @"FilmViz.cube";
        panel.allowedFileTypes = @[@"cube"];
        panel.canCreateDirectories = YES;
        if ([panel runModal] == NSModalResponseOK) selected = panel.URL.path;
    };
    if ([NSThread isMainThread]) showPanel();
    else dispatch_sync(dispatch_get_main_queue(), showPanel);
    return selected ? std::string(selected.UTF8String) : std::string();
}
