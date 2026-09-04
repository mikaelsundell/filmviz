// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "cieobserver.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::vector<std::string>
split_csv_line(const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }

    return fields;
}

bool
parse_double(const std::string& text, double& value)
{
    if (text.empty()) {
        return false;
    }

    try {
        std::size_t pos = 0;
        value = std::stod(text, &pos);

        while (pos < text.size()
               && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }

        return pos == text.size();
    }
    catch (...) {
        return false;
    }
}

} // namespace

CIEObserver::CIEObserver(const std::string& filename)
{
    load(filename);
}

bool
CIEObserver::load(const std::string& filename)
{
    clear();

    std::ifstream file(filename.c_str());
    if (!file) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        const auto fields = split_csv_line(line);

        if (fields.size() < 4) {
            continue;
        }

        double wavelength = 0.0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;

        if (!parse_double(fields[0], wavelength)
            || !parse_double(fields[1], x)
            || !parse_double(fields[2], y)
            || !parse_double(fields[3], z)) {
            continue;
        }

        const float lambda = static_cast<float>(wavelength);

        xbar_.x.push_back(lambda);
        xbar_.y.push_back(static_cast<float>(x));

        ybar_.x.push_back(lambda);
        ybar_.y.push_back(static_cast<float>(y));

        zbar_.x.push_back(lambda);
        zbar_.y.push_back(static_cast<float>(z));
    }

    if (!valid()) {
        clear();
        return false;
    }

    filename_ = filename;
    return true;
}

void
CIEObserver::clear()
{
    xbar_ = SampledCurve();
    ybar_ = SampledCurve();
    zbar_ = SampledCurve();
    filename_.clear();
}

bool
CIEObserver::valid() const
{
    return xbar_.valid()
        && ybar_.valid()
        && zbar_.valid();
}

const std::string&
CIEObserver::filename() const
{
    return filename_;
}

const SampledCurve&
CIEObserver::xbar() const
{
    return xbar_;
}

const SampledCurve&
CIEObserver::ybar() const
{
    return ybar_;
}

const SampledCurve&
CIEObserver::zbar() const
{
    return zbar_;
}
