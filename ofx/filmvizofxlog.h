// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#pragma once

#include <chrono>
#include <string>

namespace FilmVizOfxLog
{

bool enabled();
std::string path();
void write(const char* event, const std::string& details = std::string());

class Scope
{
public:
    Scope(const char* event, std::string details = std::string());
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void finish(const std::string& details = std::string());

private:
    std::string event_;
    std::string details_;
    std::chrono::steady_clock::time_point start_;
    bool finished_ = false;
};

} // namespace FilmVizOfxLog
