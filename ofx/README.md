# FilmViz OpenFX plug-in

This folder contains the CPU OpenFX front end for FilmViz. The plug-in uses
`filmviz_core` as the authoritative film model and exposes the same production
controls as the Python application, plus explicit enable switches for the two
spatial/noise effects.

## Controls

Pipeline:

- Input profile: ARRI AWG3 / LogC3 EI800, ACES2065-1 AP0 linear
- Negative: Kodak Verita 200D, Kodak VISION3 50D 5203/7203
- Print: Kodak 2383
- Output profile: ACES2065-1 AP0 linear, Rec.709 Gamma 2.4
- Exposure stops
- Push/pull stops
- Negative bleach bypass
- Print bleach bypass
- Printer R/G/B lights, neutral at 25/25/25
- Printer temperature
- Middle gray

Grain:

- Enable grain
- Negative grain
- Print grain
- Grain size
- Grain chroma
- Grain seed

Halation:

- Enable halation
- Strength
- Radius
- Threshold

Performance:

- LUT size
- Worker threads

A strength of zero still disables the associated effect. The explicit enable
switches are useful in Resolve when comparing the expensive spatial paths.

## OpenFX SDK

FilmViz uses only the public OpenFX C headers. The Academy Software Foundation
OpenFX project provides those headers. Point CMake at the directory containing
`ofxImageEffect.h`:

```bash
-DFILMVIZ_OFX_INCLUDE_DIR=/path/to/openfx/include
```

The OpenFX project is BSD-3-Clause licensed. Do not copy its headers into the
FilmViz repository unless you intentionally want to vendor the SDK.

## Build on macOS

Example using the same FilmViz dependency prefix as the main application:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/Volumes/Projects/github/3rdparty/build/macosx/arm64.release \
  -DFILMVIZ_BUILD_OFX=ON \
  -DFILMVIZ_OFX_INCLUDE_DIR=/path/to/openfx/include

cmake --build build --config Release --target filmviz_ofx_bundle -j
```

The bundle is produced at:

```text
build/ofx/FilmViz.ofx.bundle
```

The bundle follows the standard OpenFX package layout:

```text
FilmViz.ofx.bundle/
  Contents/
    Info.plist
    MacOS/              # Win64 / Linux-x86-64 on those platforms
      FilmViz.ofx
    Libraries/
      libfilmviz_core.*
      ...bundled non-system runtime dependencies...
    Resources/
      filmviz/           # FilmViz measured profile/colorimetry resources
```

The OpenFX standard uses `/Library/OFX/Plugins` on macOS,
`C:\Program Files\Common Files\OFX\Plugins` on Windows, and
`/usr/OFX/Plugins` on Linux. The install directory is configurable with
`FILMVIZ_OFX_INSTALL_DIR`.

For a user-local macOS development install:

```bash
cmake -S . -B build \
  -DFILMVIZ_BUILD_OFX=ON \
  -DFILMVIZ_OFX_INCLUDE_DIR=/path/to/openfx/include \
  -DFILMVIZ_OFX_INSTALL_DIR="$HOME/Library/OFX/Plugins"

cmake --build build --config Release --target filmviz_ofx_bundle -j
cmake --install build --config Release --component ofx
```

Restart DaVinci Resolve after installing or replacing the bundle.

## Runtime resources

The build copies the current FilmViz `resources` tree into the OFX bundle. At
runtime the plug-in resolves that bundled directory automatically. For local
experiments it can be overridden with:

```bash
export FILMVIZ_RESOURCES=/absolute/path/to/filmviz/resources
```

## Processing and cache behavior

The plug-in keeps an instance-local FilmViz cache. A parameter change that
alters the film transform rebuilds the relevant cached LUTs. Production color
LUTs are sampled with tetrahedral interpolation; the granularity sigma field
remains smoothly trilinear-interpolated because it represents a statistical
field rather than final RGB color. Plain color and grain rendering uses the
cached final 3D LUT and cached granularity sigma field. Negative-stage
halation additionally caches the input-to-negative-exposure LUT; its spatial
scatter and post-halation development LUT are built for the current rendered
frame.

Grain itself is generated deterministically from pixel coordinate, channel,
stage, seed, and frame time rather than storing a full noise image in memory.
The same frame is repeatable, while successive Resolve frames receive different
grain. This also makes the result independent of worker scheduling.

The first implementation intentionally declares `supportsTiles = false` so
Resolve supplies a whole image. That keeps negative-stage halation correct at
frame boundaries and avoids a premature tile/ROI implementation. The next
performance step can add expanded OFX regions-of-interest and tile-safe
halation without changing the FilmViz model.

## Current limitations

- CPU implementation only.
- Float RGBA input/output only.
- Full-frame rendering is requested because halation is spatial.
- No custom drawn OFX UI; Resolve renders the standard OFX parameter controls.
- Runtime dependencies are bundled into `Contents/Libraries` and rewritten to
  bundle-relative load paths during macOS packaging.


## Install helper

On macOS and Linux, CMake generates:

```text
build/ofx/install_filmviz_ofx.sh
```

A normal all-target build already builds `filmviz_ofx` and assembles
`FilmViz.ofx.bundle` when `FILMVIZ_BUILD_OFX=ON` and the OpenFX headers are
available. To install the built bundle into the configured OFX plug-in
directory, run:

```bash
./build/ofx/install_filmviz_ofx.sh
```

The default macOS destination is `/Library/OFX/Plugins`. The script requests
administrator permission only when the configured destination requires it.
Restart DaVinci Resolve after installation so it rescans OpenFX plug-ins.


## OpenFX SDK submodule

FilmViz expects the Academy Software Foundation OpenFX repository as a Git
submodule at:

```text
external/openfx
```

For a new checkout:

```bash
git clone --recurse-submodules <filmviz-repository>
```

For an existing checkout:

```bash
git submodule update --init --recursive
```

When applying this source bundle to an existing FilmViz checkout for the first
time, the included helper can register the actual Git submodule:

```bash
./scripts/setup_openfx_submodule.sh
```

The default CMake lookup is then:

```text
external/openfx/include
```

`FILMVIZ_OFX_INCLUDE_DIR` remains available as an explicit override.

A source archive can contain `.gitmodules`, but Git's submodule gitlink itself
is repository metadata and cannot be represented by a normal ZIP file. Run the
helper once in the FilmViz Git checkout, then commit `.gitmodules` and the
`external/openfx` gitlink.
