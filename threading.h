// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

// Process-wide worker-thread configuration shared by LUT generation, image
// processing, the command-line tool and the Python application.
class FilmVizThreading
{
public:
    // Zero selects the hardware concurrency. Positive values set an explicit
    // worker count.
    static void set_thread_count(int count);

    // Returns the configured value. Zero means automatic.
    static int thread_count();

    // Resolves automatic mode and limits workers to the available work.
    static int effective_thread_count(int work_items);
};
