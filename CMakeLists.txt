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
option(FILMVIZ_BUILD_PYTHON_APP "Build the pybind11 module for the PySide6 app" ON)
option(FILMVIZ_BUILD_OFX "Build the FilmViz OpenFX plug-in" ON)

set(FILMVIZ_OFX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx" CACHE PATH
    "OpenFX SDK/source root")
set(FILMVIZ_OFX_INCLUDE_DIR "" CACHE PATH
    "Directory containing ofxImageEffect.h")
set(FILMVIZ_OFX_INSTALL_DIR "" CACHE PATH
    "OpenFX plug-in install directory")

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
    bleachbypass.cpp
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
    halationmodel.cpp
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
    threading.cpp
    mitsuba/rgb2spec.c
)

add_library(filmviz_core SHARED
    ${FILMVIZ_CORE_SOURCES}
)

target_include_directories(filmviz_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(filmviz_core
    PUBLIC
        Imath::Imath
        OpenImageIO::OpenImageIO
        Threads::Threads
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
# FilmViz expects a directory named resources next to the executable at
# runtime. Source archives may intentionally omit the measured resource files.
# If resources exists in the checkout, copy it next to built executables.
# Missing resources do not make compilation fail.

set(FILMVIZ_RESOURCE_DIR "")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    set(FILMVIZ_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()

function(filmviz_copy_resources target)
    if(FILMVIZ_RESOURCE_DIR)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${FILMVIZ_RESOURCE_DIR}"
                "$<TARGET_FILE_DIR:${target}>/resources"
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
        tests/test_apd_densitometry.cpp
        tests/test_apd_resource.cpp
        tests/test_apd_spectral_selectivity.cpp
        tests/test_apd_verita_model.cpp
        tests/test_bleach_bypass.cpp
        tests/test_density_calibration.cpp
        tests/test_exposure_boundary.cpp
        tests/test_granularity.cpp
        tests/test_halation_model.cpp
        tests/test_input_transform.cpp
        tests/test_lut3d.cpp
        tests/test_negative_profile.cpp
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
                FILMVIZ_TEST_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
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
        test_exposure_boundary
        test_negative_profile
        test_pipeline
        PROPERTIES LABELS "integration;spectral"
    )

    set_tests_properties(
        test_apd_resource
        test_bleach_bypass
        test_granularity
        test_halation_model
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
# Python application binding
# ---------------------------------------------------------------------------

if(FILMVIZ_BUILD_PYTHON_APP)
    set(PYBIND11_FINDPYTHON ON)
    find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
    find_package(pybind11 CONFIG QUIET)

    if(Python3_FOUND AND pybind11_FOUND)
        pybind11_add_module(filmviz_python MODULE
            python/filmviz_pybind.cpp
        )

        target_link_libraries(filmviz_python
            PRIVATE
                filmviz_core
        )

        target_compile_features(filmviz_python PRIVATE cxx_std_17)
        filmviz_copy_resources(filmviz_python)

        add_custom_command(
            TARGET filmviz_python
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/python/filmviz_app.py"
                "$<TARGET_FILE_DIR:filmviz_python>/filmviz_app.py"
            VERBATIM
        )

        set(FILMVIZ_PYTHON_DYLD_COMMAND ":")

        if(APPLE AND CMAKE_PREFIX_PATH MATCHES "\\.debug($|;)")
            set(FILMVIZ_PYTHON_DYLD_COMMAND
                "export DYLD_IMAGE_SUFFIX=_debug")
        elseif(APPLE AND CMAKE_PREFIX_PATH MATCHES "\\.release($|;)")
            set(FILMVIZ_PYTHON_DYLD_COMMAND
                "unset DYLD_IMAGE_SUFFIX")
        endif()

        file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:filmviz_python>/python_filmviz_app.sh"
            CONTENT "#!/bin/sh
set -e

filmviz_module_directory=\"$<TARGET_FILE_DIR:filmviz_python>\"
export FILMVIZ_PROJECT_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"
export FILMVIZ_DEPENDENCY_PREFIX=\"${CMAKE_PREFIX_PATH}\"

if [ -n \"$PYTHONPATH\" ]; then
    export PYTHONPATH=\"$filmviz_module_directory:$PYTHONPATH\"
else
    export PYTHONPATH=\"$filmviz_module_directory\"
fi

${FILMVIZ_PYTHON_DYLD_COMMAND}

exec \"${Python3_EXECUTABLE}\" \"$filmviz_module_directory/filmviz_app.py\" \"$@\"
"
            TARGET filmviz_python
            FILE_PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
        )

        add_custom_target(python_filmviz_app
            DEPENDS filmviz_python
            COMMENT
                "Build the FilmViz Python module and generated application launcher"
        )
    else()
        message(STATUS
            "FilmViz Python binding disabled: Python3 development files or pybind11 were not found")
    endif()
endif()


# ---------------------------------------------------------------------------
# OpenFX plug-in
# ---------------------------------------------------------------------------

if(FILMVIZ_BUILD_OFX)
    if(NOT FILMVIZ_OFX_INCLUDE_DIR)
        if(EXISTS "${FILMVIZ_OFX_ROOT}/include/ofxImageEffect.h")
            set(FILMVIZ_OFX_INCLUDE_DIR "${FILMVIZ_OFX_ROOT}/include")
        elseif(EXISTS "${FILMVIZ_OFX_ROOT}/include/openfx/ofxImageEffect.h")
            set(FILMVIZ_OFX_INCLUDE_DIR "${FILMVIZ_OFX_ROOT}/include/openfx")
        elseif(DEFINED ENV{OFX_HEADER_DIR}
               AND EXISTS "$ENV{OFX_HEADER_DIR}/ofxImageEffect.h")
            set(FILMVIZ_OFX_INCLUDE_DIR "$ENV{OFX_HEADER_DIR}")
        endif()
    endif()

    if(NOT FILMVIZ_OFX_INSTALL_DIR)
        if(APPLE)
            set(FILMVIZ_OFX_INSTALL_DIR "/Library/OFX/Plugins")
        elseif(WIN32)
            set(FILMVIZ_OFX_INSTALL_DIR
                "C:/Program Files/Common Files/OFX/Plugins")
        else()
            set(FILMVIZ_OFX_INSTALL_DIR "/usr/OFX/Plugins")
        endif()
    endif()

    if(EXISTS "${FILMVIZ_OFX_INCLUDE_DIR}/ofxImageEffect.h")
        message(STATUS
            "FilmViz OFX headers: ${FILMVIZ_OFX_INCLUDE_DIR}")
        add_subdirectory(ofx)
    else()
        message(STATUS
            "FilmViz OFX disabled: OpenFX headers were not found")
        message(STATUS
            "  expected submodule: ${FILMVIZ_OFX_ROOT}")
        message(STATUS
            "  initialize with: git submodule update --init --recursive")
        message(STATUS
            "  or override FILMVIZ_OFX_INCLUDE_DIR explicitly")
    endif()
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
        bleachbypass.h
        filmviz.h
        filmdata.h
        filmdensitycalibration.h
        filmdyemodel.h
        filmpipeline.h
        filmprocessor.h
        filmstock.h
        granularitymodel.h
        halationmodel.h
        imageprocessor.h
        inputtransform.h
        lut3d.h
        printfilmprocessor.h
        printfilmstock.h
        printviewer.h
        spectralilluminant.h
        spectralreconstructor.h
        statusmdensitometer.h
        threading.h
    DESTINATION include/filmviz
)

if(FILMVIZ_BUILD_RGB2SPEC_OPT)
    install(
        TARGETS rgb2spec_opt
        RUNTIME DESTINATION bin
    )
endif()

if(TARGET filmviz_python)
    install(
        TARGETS filmviz_python
        LIBRARY DESTINATION python
        RUNTIME DESTINATION python
    )

    install(
        PROGRAMS python/filmviz_app.py
        DESTINATION bin
        RENAME filmviz-app
    )

endif()

if(FILMVIZ_RESOURCE_DIR)
    install(
        DIRECTORY "${FILMVIZ_RESOURCE_DIR}/"
        DESTINATION bin/resources
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
