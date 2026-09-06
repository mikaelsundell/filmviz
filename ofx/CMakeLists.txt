# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 - present Mikael Sundell.

# ---------------------------------------------------------------------------
# Production OpenFX front end.
#
# The CPU path remains the reference implementation. On macOS the plug-in also
# advertises OpenFX Metal rendering and uses Resolve's supplied command queue.
# ---------------------------------------------------------------------------

option(FILMVIZ_OFX_PREBAKE_CACHE
    "Pre-generate common FilmViz OFX transform caches during the build" ON)

add_library(filmviz_ofx_processing STATIC
    filmvizofxprocessor.cpp
    filmvizofxprocessor.h
    filmvizofxcache.cpp
    filmvizofxcache.h
    filmvizofxlog.cpp
    filmvizofxlog.h
)

target_include_directories(filmviz_ofx_processing
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(filmviz_ofx_processing
    PUBLIC
        filmviz_core
)

target_compile_features(filmviz_ofx_processing PUBLIC cxx_std_17)
set_target_properties(filmviz_ofx_processing PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_executable(filmviz_ofx_pregenerate
    filmviz_ofx_pregenerate.cpp
)

target_link_libraries(filmviz_ofx_pregenerate
    PRIVATE
        filmviz_ofx_processing
)

target_compile_features(filmviz_ofx_pregenerate PRIVATE cxx_std_17)

add_library(filmviz_ofx MODULE
    filmvizofx.cpp
)

if(APPLE)
    enable_language(OBJCXX)

    target_sources(filmviz_ofx
        PRIVATE
            filmvizmetalprocessor.mm
            filmvizmetalprocessor.h
    )

    target_compile_definitions(filmviz_ofx PRIVATE FILMVIZ_HAS_METAL=1)
    target_compile_options(filmviz_ofx PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
    )

    target_link_libraries(filmviz_ofx
        PRIVATE
            "-framework Foundation"
            "-framework Metal"
            "-framework MetalPerformanceShaders"
    )
else()
    target_compile_definitions(filmviz_ofx PRIVATE FILMVIZ_HAS_METAL=0)
endif()

target_include_directories(filmviz_ofx
    PRIVATE
        ${FILMVIZ_OFX_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(filmviz_ofx
    PRIVATE
        filmviz_ofx_processing
)

target_compile_features(filmviz_ofx PRIVATE cxx_std_17)

set_target_properties(filmviz_ofx PROPERTIES
    PREFIX ""
    OUTPUT_NAME "FilmViz"
    SUFFIX ".ofx"
    BUNDLE FALSE
)

set(FILMVIZ_OFX_PREBAKED_DIR
    "${CMAKE_BINARY_DIR}/ofx/prebaked")
set(FILMVIZ_OFX_PREBAKED_MANIFEST
    "${FILMVIZ_OFX_PREBAKED_DIR}/manifest.txt")
file(MAKE_DIRECTORY "${FILMVIZ_OFX_PREBAKED_DIR}")

if(FILMVIZ_OFX_PREBAKE_CACHE AND EXISTS "${CMAKE_SOURCE_DIR}/resources")
    file(GLOB_RECURSE FILMVIZ_OFX_PREBAKE_RESOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/resources/profiles/*"
        "${CMAKE_SOURCE_DIR}/resources/colorimetry/*"
        "${CMAKE_SOURCE_DIR}/resources/spectral/*")

    add_custom_command(
        OUTPUT "${FILMVIZ_OFX_PREBAKED_MANIFEST}"
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "${FILMVIZ_OFX_PREBAKED_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${FILMVIZ_OFX_PREBAKED_DIR}"
        COMMAND ${CMAKE_COMMAND} -E env
            "FILMVIZ_OFX_CACHE_DIR=${FILMVIZ_OFX_PREBAKED_DIR}"
            "FILMVIZ_OFX_LOG=0"
            $<TARGET_FILE:filmviz_ofx_pregenerate>
            --resources "${CMAKE_SOURCE_DIR}/resources"
            --output "${FILMVIZ_OFX_PREBAKED_DIR}"
            --lut-size 33
        DEPENDS
            filmviz_ofx_pregenerate
            ${FILMVIZ_OFX_PREBAKE_RESOURCE_FILES}
        COMMENT "Pre-generating common FilmViz OFX transform caches"
        VERBATIM
    )

    add_custom_target(filmviz_ofx_prebaked
        DEPENDS "${FILMVIZ_OFX_PREBAKED_MANIFEST}"
    )

    add_dependencies(filmviz_ofx filmviz_ofx_prebaked)
endif()

if(APPLE)
    set(FILMVIZ_OFX_ARCH_DIRECTORY "MacOS")
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(FILMVIZ_OFX_ARCH_DIRECTORY "Win-arm64ec")
    else()
        set(FILMVIZ_OFX_ARCH_DIRECTORY "Win64")
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(FILMVIZ_OFX_ARCH_DIRECTORY "Linux-aarch64")
    else()
        set(FILMVIZ_OFX_ARCH_DIRECTORY "Linux-x86-64")
    endif()
endif()

set(FILMVIZ_OFX_BUNDLE_DIR
    "${CMAKE_BINARY_DIR}/ofx/FilmViz.ofx.bundle"
    CACHE INTERNAL "FilmViz OFX bundle directory")

set(FILMVIZ_OFX_BINARY_DIR
    "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/${FILMVIZ_OFX_ARCH_DIRECTORY}")

if(APPLE)
    set(FILMVIZ_OFX_PACKAGE_COMMAND
        sh
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_bundle.sh"
        "${FILMVIZ_OFX_BUNDLE_DIR}"
        "FilmViz")
else()
    # Dependency rewriting and code signing are macOS packaging operations.
    # Other platforms retain the standard OFX bundle assembled below.
    set(FILMVIZ_OFX_PACKAGE_COMMAND
        "${CMAKE_COMMAND}" -E true)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY
)

add_custom_command(
    TARGET filmviz_ofx
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "${FILMVIZ_OFX_BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${FILMVIZ_OFX_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Resources/filmviz"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Libraries"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:filmviz_ofx>"
        "${FILMVIZ_OFX_BINARY_DIR}/FilmViz.ofx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:filmviz_core>"
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Libraries/$<TARGET_FILE_NAME:filmviz_core>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/profiles"
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Resources/filmviz/profiles"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/colorimetry"
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Resources/filmviz/colorimetry"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/spectral"
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Resources/filmviz/spectral"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Resources/filmviz/cache/ofx"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FILMVIZ_OFX_PREBAKED_DIR}"
        "${FILMVIZ_OFX_BUNDLE_DIR}/Contents/Resources/filmviz/cache/ofx"
    COMMAND ${FILMVIZ_OFX_PACKAGE_COMMAND}
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_custom_target(filmviz_ofx_bundle
    DEPENDS filmviz_ofx
    COMMENT "Build FilmViz OFX full-processing bundle"
)

if(UNIX)
    set(FILMVIZ_OFX_INSTALL_SCRIPT
        "${CMAKE_BINARY_DIR}/ofx/install_filmviz_ofx.sh")

    file(GENERATE
        OUTPUT "${FILMVIZ_OFX_INSTALL_SCRIPT}"
        CONTENT "#!/bin/sh
set -eu

bundle=\"${FILMVIZ_OFX_BUNDLE_DIR}\"
destination=\"${FILMVIZ_OFX_INSTALL_DIR}\"
target=\"$destination/FilmViz.ofx.bundle\"

if [ ! -d \"$bundle\" ]; then
    echo \"FilmViz OFX bundle has not been built yet:\"
    echo \"  $bundle\"
    exit 1
fi

echo \"Installing FilmViz OFX plug-in\"
echo \"  from: $bundle\"
echo \"  to:   $target\"

install_bundle()
{
    mkdir -p \"$destination\"
    rm -rf \"$target\"
    cp -R \"$bundle\" \"$target\"
}

if [ -d \"$destination\" ] && [ -w \"$destination\" ]; then
    install_bundle
elif [ ! -e \"$destination\" ] && [ -w \"$(dirname \"$destination\")\" ]; then
    install_bundle
else
    echo \"Administrator permission is required for $destination\"
    sudo mkdir -p \"$destination\"
    sudo rm -rf \"$target\"
    sudo cp -R \"$bundle\" \"$target\"
fi

if [ \"$(uname -s)\" = \"Darwin\" ]; then
    sudo xattr -dr com.apple.quarantine \"$target\" 2>/dev/null || true
    sudo codesign --force --deep --sign - --timestamp=none \"$target\"
    sudo codesign --verify --deep --strict \"$target\"

    cache=\"$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml\"
    if [ -f \"$cache\" ]; then
        echo \"Removing Resolve OFX cache:\"
        echo \"  $cache\"
        rm -f \"$cache\"
    fi
fi

echo
echo \"Installed FilmViz.\"
echo \"Restart DaVinci Resolve and search OpenFX for 'FilmViz'.\"
"
        FILE_PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
    )
endif()

install(
    DIRECTORY "${FILMVIZ_OFX_BUNDLE_DIR}"
    DESTINATION "${FILMVIZ_OFX_INSTALL_DIR}"
    COMPONENT ofx
    USE_SOURCE_PERMISSIONS
)
