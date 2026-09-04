#!/bin/sh
set -eu

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

"${project_directory}/build/Debug/rgb2spec_opt" \
    64 \
    "${project_directory}/Resources/spectral/reconstruction/ACES2065_1.spec" \
    ACES2065_1
