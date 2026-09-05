// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 - present Mikael Sundell.

#include "lut3d.h"
#include "test_common.h"
#include "threading.h"

#include <array>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

namespace {

bool
affine_pipeline(
    const Lut3D::RGB& input,
    Lut3D::RGB& output)
{
    output = {{
        0.10f + 0.50f * input[0] + 0.10f * input[1],
        0.20f + 0.25f * input[1] + 0.05f * input[2],
        0.05f + 0.30f * input[0] + 0.40f * input[2]
    }};

    return true;
}

} // namespace

int
main()
{
    bool passed = true;
    Lut3D lut;

    passed &= test::check(
        !lut.generate(
            1,
            affine_pipeline),
        "LUT sizes below two are rejected");

    passed &= test::check(
        lut.generate(
            5,
            affine_pipeline),
        "LUT generation succeeds");

    passed &= test::check(
        lut.valid()
        && lut.size() == 5,
        "generated LUT reports the correct shape");

    std::vector<Lut3D::RGB> assigned_values;
    assigned_values.reserve(125);

    for (int blue = 0; blue < 5; ++blue) {
        for (int green = 0; green < 5; ++green) {
            for (int red = 0; red < 5; ++red) {
                assigned_values.push_back(
                    lut.at(red, green, blue));
            }
        }
    }

    Lut3D assigned_lut;

    passed &= test::check(
        assigned_lut.assign(
            5,
            assigned_values),
        "precomputed LUT values can be assigned directly");

    passed &= test::check(
        assigned_lut.valid()
        && assigned_lut.at(3, 2, 4) == lut.at(3, 2, 4),
        "assigned LUT preserves FilmViz storage order");

    const Lut3D::RGB input = {{0.17f, 0.43f, 0.81f}};
    Lut3D::RGB expected;
    affine_pipeline(
        input,
        expected);

    const Lut3D::RGB trilinear =
        lut.sample_trilinear(
            input);

    const Lut3D::RGB tetrahedral =
        lut.sample_tetrahedral(
            input);

    for (int channel = 0;
         channel < 3;
         ++channel) {

        passed &= test::near(
            trilinear[channel],
            expected[channel],
            1e-6,
            "trilinear sampling preserves an affine pipeline");

        passed &= test::near(
            tetrahedral[channel],
            expected[channel],
            1e-6,
            "tetrahedral sampling preserves an affine pipeline");
    }

    const Lut3D::Validation validation =
        lut.validate(
            affine_pipeline,
            5);

    passed &= test::check(
        validation.valid
        && validation.samples == 125
        && validation.max_abs_error < 1e-6,
        "direct-versus-LUT tetrahedral validation is accurate");

    std::mutex worker_mutex;
    std::set<std::thread::id> worker_ids;
    FilmVizThreading::set_thread_count(4);
    Lut3D parallel_lut;

    passed &= test::check(
        parallel_lut.generate(
            8,
            [&](const Lut3D::RGB& threaded_input,
                Lut3D::RGB& threaded_output) {

                {
                    const std::lock_guard<std::mutex> lock(
                        worker_mutex);
                    worker_ids.insert(
                        std::this_thread::get_id());
                }

                std::this_thread::sleep_for(
                    std::chrono::microseconds(100));
                return affine_pipeline(
                    threaded_input,
                    threaded_output);
            }),
        "parallel LUT generation succeeds");

    passed &= test::check(
        worker_ids.size() > 1,
        "global worker setting distributes LUT slices across threads");

    FilmVizThreading::set_thread_count(1);
    Lut3D serial_lut;

    passed &= test::check(
        serial_lut.generate(
            8,
            affine_pipeline),
        "single-thread LUT generation succeeds");

    bool deterministic = true;

    for (int blue = 0; blue < 8; ++blue) {
        for (int green = 0; green < 8; ++green) {
            for (int red = 0; red < 8; ++red) {
                for (int channel = 0; channel < 3; ++channel) {
                    deterministic =
                        deterministic
                        && std::abs(
                            parallel_lut.at(red, green, blue)[channel]
                            - serial_lut.at(red, green, blue)[channel])
                            < 1e-8f;
                }
            }
        }
    }

    passed &= test::check(
        deterministic,
        "LUT values are independent of worker count");

    FilmVizThreading::set_thread_count(0);

    return
        test::finish(
            passed,
            "3D LUT pipeline sampling");
}
