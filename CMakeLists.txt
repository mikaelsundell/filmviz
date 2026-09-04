# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2022 - present Mikael Sundell.

cmake_minimum_required(VERSION 3.23)

project(
    filmviz
    VERSION 0.1.0
    LANGUAGES C CXX
)

option(FILMVIZ_BUILD_RGB2SPEC_OPT "Build the rgb2spec table optimizer" ON)
option(FILMVIZ_BUILD_EXAMPLES "Build current FilmViz API examples" ON)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Keep single-config executables in build/bin, matching the multi-config
# build/Debug layout used during development.
if(NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

find_package(Imath CONFIG REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(FILMVIZ_CORE_SOURCES
    cieobserver.cpp
    colorimetry.cpp
    colortransform.cpp
    diagramwriter.cpp
    filmdensitycalibration.cpp
    filmdyemodel.cpp
    filmpipeline.cpp
    filmprocessor.cpp
    filmstock.cpp
    granularitymodel.cpp
    imageprocessor.cpp
    inputtransform.cpp
    lut3d.cpp
    printdyemodel.cpp
    printercalibration.cpp
    printfilmprocessor.cpp
    printfilmstock.cpp
    printviewer.cpp
    spectralcolorimetry.cpp
    spectralilluminant.cpp
    spectralreconstructor.cpp
    statusmdensitometer.cpp
    mitsuba/rgb2spec.c
)

add_library(filmviz_core SHARED
    ${FILMVIZ_CORE_SOURCES}
)

# Compatibility name for older local CMake snippets/documentation.
add_library(filmsim_core ALIAS filmviz_core)

target_include_directories(filmviz_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(filmviz_core
    PUBLIC
        Imath::Imath
        OpenImageIO::OpenImageIO
)

target_compile_features(filmviz_core PUBLIC cxx_std_17)

add_executable(filmviz
    main.cpp
)

target_link_libraries(filmviz
    PRIVATE
        filmviz_core
)

# ---------------------------------------------------------------------------
# Resource layout
# ---------------------------------------------------------------------------
#
# FilmViz expects a directory named Resources next to the executable at
# runtime. Source archives may intentionally omit the measured resource files.
# If Resources (or legacy lowercase resources) exists in the checkout, copy it
# next to built executables. Missing resources do not make compilation fail.

set(FILMVIZ_RESOURCE_DIR "")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    set(FILMVIZ_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Resources")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    set(FILMVIZ_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()

function(filmviz_copy_resources target)
    if(FILMVIZ_RESOURCE_DIR)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${FILMVIZ_RESOURCE_DIR}"
                "$<TARGET_FILE_DIR:${target}>/Resources"
            VERBATIM
        )
    endif()
endfunction()

filmviz_copy_resources(filmviz)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# The active suite contains only deterministic tests.

if(BUILD_TESTING)
    set(FILMVIZ_TEST_SOURCES
        tests/test_apd_resource.cpp
        tests/test_density_calibration.cpp
        tests/test_granularity.cpp
        tests/test_input_transform.cpp
        tests/test_lut3d.cpp
        tests/test_pipeline.cpp
    )

    foreach(test_source IN LISTS FILMVIZ_TEST_SOURCES)
        get_filename_component(test_name "${test_source}" NAME_WE)

        add_executable(${test_name}
            "${test_source}"
        )

        target_link_libraries(${test_name}
            PRIVATE
                filmviz_core
        )

        target_compile_features(${test_name} PRIVATE cxx_std_17)

        target_compile_definitions(${test_name}
            PRIVATE
                FILMVIZ_TEST_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Resources"
        )

        add_test(
            NAME ${test_name}
            COMMAND $<TARGET_FILE:${test_name}>
        )

        set_tests_properties(${test_name}
            PROPERTIES
                TIMEOUT 120
        )
    endforeach()

    set_tests_properties(
        test_density_calibration
        test_pipeline
        PROPERTIES LABELS "integration;spectral"
    )

    set_tests_properties(
        test_apd_resource
        test_granularity
        test_input_transform
        test_lut3d
        PROPERTIES LABELS "unit"
    )
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(FILMVIZ_BUILD_EXAMPLES)
    set(FILMVIZ_EXAMPLE_SOURCES
        examples/example_convert_arri_image.cpp
        examples/example_density_calibration.cpp
        examples/example_profile_diagrams.cpp
        examples/example_process_pixel.cpp
    )

    foreach(example_source IN LISTS FILMVIZ_EXAMPLE_SOURCES)
        get_filename_component(example_name "${example_source}" NAME_WE)

        add_executable(${example_name}
            "${example_source}"
        )

        target_link_libraries(${example_name}
            PRIVATE
                filmviz_core
        )

        target_compile_features(${example_name} PRIVATE cxx_std_17)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# rgb2spec optimizer
# ---------------------------------------------------------------------------

if(FILMVIZ_BUILD_RGB2SPEC_OPT)
    add_executable(rgb2spec_opt
        mitsuba/rgb2spec_opt.cpp
        mitsuba/rgb2spec.c
        mitsuba/rgb2spec.h
        mitsuba/macros.h
        mitsuba/details/cie1931.h
        mitsuba/details/lu.h
        mitsuba/LICENSE.txt
        mitsuba/README.md
    )

    target_include_directories(rgb2spec_opt
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mitsuba
            ${CMAKE_CURRENT_SOURCE_DIR}/mitsuba/details
    )

    target_link_libraries(rgb2spec_opt
        PRIVATE
            Threads::Threads
    )

    target_compile_features(rgb2spec_opt PRIVATE cxx_std_17)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

install(
    TARGETS filmviz filmviz_core
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(
    FILES
        filmviz.h
        filmdata.h
        filmdensitycalibration.h
        filmdyemodel.h
        filmpipeline.h
        filmprocessor.h
        filmstock.h
        inputtransform.h
        lut3d.h
        printfilmprocessor.h
        printfilmstock.h
        printviewer.h
        spectralilluminant.h
        spectralreconstructor.h
        statusmdensitometer.h
    DESTINATION include/filmviz
)

if(FILMVIZ_BUILD_RGB2SPEC_OPT)
    install(
        TARGETS rgb2spec_opt
        RUNTIME DESTINATION bin
    )
endif()

if(FILMVIZ_RESOURCE_DIR)
    install(
        DIRECTORY "${FILMVIZ_RESOURCE_DIR}/"
        DESTINATION bin/Resources
    )
endif()

install(
    FILES
        README.md
        ARCHITECTURE.md
        CALIBRATION.md
        DEVELOPMENT.md
        AGENTS.md
    DESTINATION share/filmviz
)
