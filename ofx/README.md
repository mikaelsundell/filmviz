# FilmViz OpenFX plug-in

This folder contains the production OpenFX front end for FilmViz. The plug-in
uses `filmviz_core` as the authoritative film model and supports both the CPU
reference renderer and a Metal-accelerated renderer on macOS.

## Processing backends

The **Processing** control selects the render backend:

- **Auto** — uses Metal when the OpenFX host supplies a Metal render, otherwise
  uses the CPU reference renderer. This is the recommended setting.
- **Metal** — prefers the Metal implementation. If the host does not enable
  Metal for a render action, FilmViz falls back to CPU.
- **CPU** — forces the reference CPU implementation. When Resolve has supplied
  Metal buffers, this mode stages the image through shared buffers and is
  intended primarily for validation/comparison rather than performance.

On non-macOS builds the UI exposes Auto and CPU only.

The CPU path remains the reference implementation. The Metal path accelerates
the complete per-frame image path: cached color LUT evaluation, negative and
print grain, and negative-stage halation. Film profile parsing, spectral model
setup, calibration, and cached LUT construction remain on CPU because they are
parameter-change work rather than per-pixel frame work.

## Controls

Pipeline:

- Processing backend
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
- Worker threads (CPU processing and cached LUT generation)

Grain and halation are disabled by default.

## Metal implementation

On macOS FilmViz advertises OpenFX Metal rendering. When Resolve enables Metal,
`kOfxImagePropData` is treated as an `id<MTLBuffer>` and work is enqueued on
the host-provided `id<MTLCommandQueue>`. Normal Metal rendering is asynchronous
and does not wait for final GPU completion before returning from Render.

The Metal backend keeps the cached FilmViz LUT products resident in GPU buffers:

- final color LUT
- negative and print granularity sigma fields
- negative-exposure LUT for halation
- post-halation development LUT and granularity fields

Color LUTs use tetrahedral interpolation. Granularity sigma fields remain
trilinear because they represent smooth statistical fields rather than final
RGB transforms. Grain is generated deterministically per pixel, channel, stage,
seed, and frame time.

Halation runs at the negative stage. The Metal path extracts the highlight-
weighted negative exposure, performs near/far Gaussian scatter with Metal
Performance Shaders, adds record-dependent scatter, and evaluates the cached
post-halation development LUT. The CPU path remains available for reference
comparison.

The first Metal render on a plug-in instance may include a one-time shader
compile/upload cost. Pipelines and LUT buffers are then cached per Metal device
and FilmViz cache revision.

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

The OpenFX project is BSD-3-Clause licensed.

## Build on macOS

Example using the same FilmViz dependency prefix as the main application:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/Volumes/Projects/github/3rdparty/build/macosx/arm64.release \
  -DFILMVIZ_BUILD_OFX=ON

cmake --build build --config Release -j
```

A normal all-target build includes the OFX plug-in. The bundle is produced at:

```text
build/ofx/FilmViz.ofx.bundle
```

On macOS the OFX target links Foundation, Metal, and MetalPerformanceShaders.
Other platforms build the CPU backend only.

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
```

Runtime dependencies are copied into `Contents/Libraries`, rewritten to
bundle-relative load paths, and signed as part of the macOS packaging step.
The FilmViz `resources` tree is copied to `Contents/Resources/filmviz`.

For local experiments the resource path can be overridden with:

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

The default macOS destination is `/Library/OFX/Plugins`. The installer clears
quarantine metadata, signs/verifies the installed bundle, and removes Resolve's
OFX cache so the rebuilt plug-in is discovered on the next launch. Restart
DaVinci Resolve after installation.

## Cache and rendering behavior

The plug-in keeps an instance-local FilmViz cache. Parameter changes that alter
the film transform rebuild only the relevant cached products. Grain strength,
size, chroma, seed, and halation spatial controls do not unnecessarily rebuild
the base color LUT.

The plug-in currently requests full-frame rendering (`supportsTiles = false`)
because halation is spatial and needs neighboring image data. A future ROI/tile
implementation can expand the requested source region by the halation support
radius without changing the FilmViz model.

## Current limitations

- Float RGBA input/output only.
- Metal acceleration is macOS-only; CPU remains available on all platforms.
- Full-frame rendering is currently requested because halation is spatial.
- No custom-drawn OFX UI; Resolve renders the standard parameter controls.
- The Metal halation blur uses Metal Performance Shaders Gaussian blur, while
  the CPU reference uses FilmViz's CPU spatial approximation. Their film-stage
  model and scatter parameters match, but pixel-level blur results may differ
  slightly.
