# FilmViz OpenFX plug-in

This folder contains the production OpenFX front end for FilmViz. The plug-in
uses `filmviz_core` as the authoritative film model and supports both the CPU
reference renderer and a Metal-accelerated renderer on macOS.

## Processing backends

The **Processing** control selects the render backend:

- **Auto** — uses Metal when the OpenFX host supplies a Metal render, otherwise
  uses the CPU reference renderer. This is the recommended setting.
- **Metal** — prefers Metal and falls back to CPU when the host does not provide
  a Metal render action.
- **CPU** — forces the reference CPU implementation. In a Metal render this
  stages through shared buffers and is intended mainly for validation.

The CPU and Metal paths use the same cached FilmViz transform products. Color
LUTs use tetrahedral interpolation; granularity sigma fields remain trilinear.
The input-to-negative-exposure LUT uses the production exposure-separated
rgb2spec reconstruction: AP0/D60 values above Y=0.18 are reconstructed at
Y=0.18 and then spectrally rescaled.

## Interactive transform behavior

The OFX transform cache is process-wide, not node-local. Resolve nodes with
matching film-transform settings share the input-to-negative-exposure LUT,
development/output LUT, development log domain, and granularity field. The
heavy `FilmPipeline` is needed only to build a cache miss and is released once
those immutable transform products have been generated. Cache ownership uses
`weak_ptr`, so unused in-memory transforms can be released automatically.

Exposure is deliberately excluded from the expensive transform key. The cached
transform is split at FilmViz's physical negative-exposure boundary:

```text
encoded input
  -> cached input-to-negative-exposure LUT
  -> raw FilmExposure * 2^ExposureStops
  -> logarithmic development shaper
  -> cached negative-development / print / output LUT
```

Multiplying `FilmExposure` by `2^stops` is mathematically the same operation as
the LogE exposure offset used by `FilmPipeline::relative_negative_log_exposure`.
Changing Exposure therefore preserves the FilmViz model while avoiding LUT
regeneration and Metal re-upload. Measured MTF, grain and halation spatial
controls are also live parameters and do not invalidate the shared transform.

Transform-changing controls such as negative stock, flash, push/pull, bleach
bypass, printer lights, middle gray, and input/output profile select or build a
different shared transform cache.

During an interactive parameter drag, Resolve's interactive/draft render hint
selects a quantized 9^3 preview transform when the requested full-quality
transform is not already resident. This substantially reduces spectral cache
generation time while preserving the same physical pipeline. When interaction
ends, FilmViz generates or loads the exact parameter value at the fixed
production LUT size. Runtime-only Exposure, MTF, grain, and halation changes
continue to reuse the resident full-quality transform without entering preview
mode.

## Persistent and bundled caches

FilmViz checks transform caches in this order:

1. process-wide shared memory cache;
2. pre-generated cache bundled in `Contents/Resources/filmviz/cache/ofx`;
3. persistent user cache;
4. generate the transform and save it to the persistent cache.

The cache format carries a model revision. Caches produced before the current
exposure-separated reconstruction are rejected automatically and regenerated.

On macOS the persistent cache defaults to:

```text
~/Library/Caches/FilmViz/ofx
```

Override it with:

```bash
export FILMVIZ_OFX_CACHE_DIR=/path/to/cache
```

### Pre-generating common combinations

A normal OFX build pre-generates the neutral/common FilmViz combinations at LUT
size 33 when `FILMVIZ_OFX_PREBAKE_CACHE=ON` (default). The generated cache
contains both negative stocks, both input profiles, and both output profiles,
with Kodak Vision 2383/3383, 25/25/25 printer lights, 3200 K, zero push/pull,
zero bleach
bypass, and middle gray 0.18. Each `.fvcache` contains the input-to-negative-
exposure LUT, the log-exposure development domain, the developed/output LUT,
and the granularity sigma field, so those common combinations are ready when
Resolve creates the node.

The build tool is:

```text
filmviz_ofx_pregenerate
```

and the convenience script is:

```bash
./ofx/scripts/pregenerate_cache.sh build
```

The cache is generated under:

```text
build/ofx/prebaked
```

and copied automatically into the OFX bundle. Disable build-time pre-generation
with:

```bash
-DFILMVIZ_OFX_PREBAKE_CACHE=OFF
```

## Metal cache

On macOS, matching Resolve nodes on the same `MTLDevice` also share the uploaded
Metal LUT/granularity buffers. The first matching node uploads the transform;
subsequent nodes reuse the same GPU resources. Exposure changes do not trigger
another upload.

Metal accelerates:

- tetrahedral final color LUT evaluation;
- negative and print grain;
- negative-stage halation extraction and development;
- near/far Gaussian scatter through Metal Performance Shaders.

Profile parsing, calibration, and cache generation remain CPU tasks because
they occur only on transform cache misses.

## Timeline/performance logging

FilmViz writes a thread-safe OFX performance log by default. On macOS:

```text
~/Library/Logs/FilmViz/filmviz_ofx.log
```

The log includes plug-in load/unload, node creation/destruction, frame time,
backend requests, exposure, stock selection, cache hits/misses, bundled/disk
cache hits, LUT-generation time, Metal upload/reuse, and render/encode timing.
The log rotates at approximately 32 MB.

Open it with:

```bash
./ofx/scripts/open_log.sh
```

Summarize the recent node/cache/render timeline with:

```bash
./ofx/scripts/analyze_log.py --last 200
```

The analyzer also reports cache-result counts and average/max timing by event,
which makes it easy to see whether a slow node load was a bundled hit, a
process-wide hit, a disk hit, or an actual transform generation.

Disable logging with:

```bash
export FILMVIZ_OFX_LOG=0
```

or override the path with:

```bash
export FILMVIZ_OFX_LOG_PATH=/path/to/filmviz_ofx.log
```

## Controls

Pipeline:

- Enable
- Processing backend
- Input profile: ARRI AWG3 / LogC3 EI800, ACES2065-1 AP0 linear
- Negative: Kodak Verita 200D 5206/7206, Kodak Vision3 50D 5203/7203
- Print: Kodak Vision 2383/3383
- Output profile: ACES2065-1 AP0 linear, Rec.709 Gamma 2.4
- Exposure stops
- Negative flash
- Print flash
- Push/pull stops
- Negative bleach bypass
- Print bleach bypass
- Printer R/G/B lights, neutral at 25/25/25
- Printer master timing
- Middle gray

Spatial response:

- Film format: Regular 8, Super 8, 16mm, Super 16, 35mm, Super 35, 65mm, Custom
- Custom image width in millimetres
- Measured negative MTF amount
- Measured print MTF amount

Grain:

- Enable grain
- Negative grain
- Print grain
- Grain scale
- Grain chroma
- Grain seed

Halation:

- Enable halation
- Strength
- Radius
- Threshold

Performance:

- Worker threads

MTF, grain and halation are disabled by default. Enabling MTF uses the measured
cycles/mm response and the selected active-image width. Because the current
Metal kernel is pointwise, measured MTF automatically uses the CPU spatial
bridge while retaining the cached colour transform.
The OFX production transform is fixed at 33^3 and the calibrated Kodak Vision
2383/3383
printer illuminant approximation is fixed at 3200 K. These are profile and
quality constants rather than creative controls. The standalone cache
pregenerator retains a LUT-size argument for development diagnostics, while
bundled production caches are always generated at 33^3.

## OpenFX SDK

FilmViz uses the Academy Software Foundation OpenFX repository as a Git
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

CMake automatically uses `external/openfx/include`.
`FILMVIZ_OFX_INCLUDE_DIR` remains available as an explicit override.

## Build on macOS

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/Volumes/Projects/github/3rdparty/build/macosx/arm64.release \
  -DFILMVIZ_BUILD_OFX=ON

cmake --build build --config Release -j
```

A normal all-target build includes the OFX plug-in and pre-generated caches.
The bundle is produced at:

```text
build/ofx/FilmViz.ofx.bundle
```

## Bundle layout

```text
FilmViz.ofx.bundle/
  Contents/
    Info.plist
    MacOS/
      FilmViz.ofx
    Libraries/
      libfilmviz_core.*
      ...bundled non-system runtime dependencies...
    Resources/
      filmviz/
        cache/
          ofx/
            manifest.txt
            *.fvcache
```

Runtime dependencies are copied into `Contents/Libraries`, rewritten to
bundle-relative load paths, stripped of absolute build-machine `LC_RPATH`
entries, and signed during macOS packaging. For local experiments the
profile/resource root can be overridden with:

```bash
export FILMVIZ_RESOURCES=/absolute/path/to/filmviz/resources
```

## Install

CMake generates:

```text
build/ofx/install_filmviz_ofx.sh
```

After building:

```bash
./build/ofx/install_filmviz_ofx.sh
```

The default macOS destination is `/Library/OFX/Plugins`. Restart DaVinci
Resolve after installation.

## Current limitations

- Float RGBA input/output only.
- Metal acceleration is macOS-only; CPU remains available on all platforms.
- Full-frame rendering is requested because halation and MTF are spatial.
- No custom-drawn OFX UI; Resolve renders the standard parameter controls.
- Metal halation uses Metal Performance Shaders Gaussian blur while the CPU
  reference uses FilmViz's CPU spatial approximation, so pixel-level blur can
  differ slightly even though the film-stage model and scatter parameters match.
