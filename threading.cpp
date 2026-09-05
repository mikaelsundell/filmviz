// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "threading.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace
{

std::atomic<int> configured_thread_count(0);

} // namespace

void
FilmVizThreading::set_thread_count(
    int count)
{
    configured_thread_count.store(
        std::max(0, count),
        std::memory_order_relaxed);
}

int
FilmVizThreading::thread_count()
{
    return
        configured_thread_count.load(
            std::memory_order_relaxed);
}

int
FilmVizThreading::effective_thread_count(
    int work_items)
{
    if (work_items <= 0) {
        return 0;
    }

    int count = thread_count();

    if (count == 0) {
        count =
            static_cast<int>(
                std::thread::hardware_concurrency());
    }

    return
        std::min(
            work_items,
            std::max(1, count));
}
