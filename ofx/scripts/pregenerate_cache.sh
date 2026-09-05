#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
set -eu

build_dir="${1:-build}"
project_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
resources="${FILMVIZ_RESOURCES:-$project_root/resources}"
output="${2:-$build_dir/ofx/prebaked}"
lut_size="${FILMVIZ_OFX_PREBAKE_LUT_SIZE:-33}"

if [ -x "$build_dir/ofx/filmviz_ofx_pregenerate" ]; then
    tool="$build_dir/ofx/filmviz_ofx_pregenerate"
elif [ -x "$build_dir/ofx/Release/filmviz_ofx_pregenerate" ]; then
    tool="$build_dir/ofx/Release/filmviz_ofx_pregenerate"
elif [ -x "$build_dir/ofx/Debug/filmviz_ofx_pregenerate" ]; then
    tool="$build_dir/ofx/Debug/filmviz_ofx_pregenerate"
else
    echo "FilmViz OFX pregenerator is not built under: $build_dir/ofx"
    exit 1
fi

exec "$tool" \
    --resources "$resources" \
    --output "$output" \
    --lut-size "$lut_size"
