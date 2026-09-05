#!/bin/sh
set -eu

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

"${project_directory}/build/Debug/rgb2spec_opt" \
    128 \
    "${project_directory}/resources/spectral/reconstruction/ACES2065_1_128.spec" \
    ACES2065_1
