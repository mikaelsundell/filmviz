// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "filmvizofxlog.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace FilmVizOfxLog
{
namespace
{

std::mutex gMutex;
constexpr std::uintmax_t kRotateBytes = 32u * 1024u * 1024u;

bool
environment_enabled()
{
    const char* value = std::getenv("FILMVIZ_OFX_LOG");

    if (!value || !*value) {
        return true;
    }

    return
        std::string(value) != "0"
        && std::string(value) != "false"
        && std::string(value) != "off";
}

std::filesystem::path
log_path()
{
    if (const char* override_path = std::getenv("FILMVIZ_OFX_LOG_PATH")) {
        if (*override_path) {
            return std::filesystem::path(override_path);
        }
    }

#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return
            std::filesystem::path(home)
            / "Library"
            / "Logs"
            / "FilmViz"
            / "filmviz_ofx.log";
    }
#elif defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        return
            std::filesystem::path(local)
            / "FilmViz"
            / "Logs"
            / "filmviz_ofx.log";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return
            std::filesystem::path(home)
            / ".cache"
            / "filmviz"
            / "filmviz_ofx.log";
    }
#endif

    return std::filesystem::path("filmviz_ofx.log");
}

std::string
timestamp()
{
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const std::time_t time = Clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
        % 1000;

    std::tm value = {};
#if defined(_WIN32)
    localtime_s(&value, &time);
#else
    localtime_r(&time, &value);
#endif

    std::ostringstream stream;
    stream
        << std::put_time(&value, "%Y-%m-%dT%H:%M:%S")
        << '.'
        << std::setfill('0')
        << std::setw(3)
        << milliseconds.count();

    return stream.str();
}

void
rotate_if_needed(
    const std::filesystem::path& file)
{
    std::error_code error;

    if (!std::filesystem::exists(file, error)
        || std::filesystem::file_size(file, error) < kRotateBytes) {
        return;
    }

    const std::filesystem::path previous =
        file.string() + ".1";

    std::filesystem::remove(previous, error);
    error.clear();
    std::filesystem::rename(file, previous, error);
}

} // namespace

bool
enabled()
{
    return environment_enabled();
}

std::string
path()
{
    return log_path().string();
}

void
write(
    const char* event,
    const std::string& details)
{
    if (!enabled()) {
        return;
    }

    const std::lock_guard<std::mutex> lock(gMutex);
    const std::filesystem::path file = log_path();

    std::error_code error;
    std::filesystem::create_directories(
        file.parent_path(),
        error);

    rotate_if_needed(file);

    std::ofstream stream(
        file,
        std::ios::out | std::ios::app);

    if (!stream) {
        return;
    }

    stream
        << timestamp()
        << " tid="
        << std::this_thread::get_id()
        << " event="
        << (event ? event : "unknown");

    if (!details.empty()) {
        stream
            << ' '
            << details;
    }

    stream << '\n';
}

Scope::Scope(
    const char* event,
    std::string details)
    : event_(event ? event : "unknown")
    , details_(std::move(details))
    , start_(std::chrono::steady_clock::now())
{
}

Scope::~Scope()
{
    if (!finished_) {
        finish();
    }
}

void
Scope::finish(
    const std::string& details)
{
    if (finished_) {
        return;
    }

    finished_ = true;

    const double milliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_)
            .count();

    std::ostringstream stream;

    if (!details_.empty()) {
        stream << details_;
    }

    if (!details.empty()) {
        if (stream.tellp() > 0) {
            stream << ' ';
        }
        stream << details;
    }

    if (stream.tellp() > 0) {
        stream << ' ';
    }

    stream
        << "elapsed_ms="
        << std::fixed
        << std::setprecision(3)
        << milliseconds;

    write(
        event_.c_str(),
        stream.str());
}

} // namespace FilmVizOfxLog
