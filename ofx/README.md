# FilmViz OpenFX plug-in

This folder contains the production OpenFX front end for FilmViz. The plug-in
uses `filmviz_core` as the authoritative film model and renders through the GPU
API supplied by the OpenFX host.

## GPU backend selection

There is no plug-in-local backend control. FilmViz follows Resolve's configured
GPU processing mode:

- **Metal** evaluates rgb2spec, negative exposure/development,
  Status-M closure, colour response, 2383 exposure/development and D55 viewing
  in Metal without a colour-transform LUT.
- **OpenCL** evaluates the same direct spectral kernel through the
  OpenFX host command queue. It is available when OpenCL was found at build
  time.

Metal is preferred on macOS; OpenCL supports older macOS configurations and is
the current Windows backend. The OFX plug-in does not silently fall back to CPU.
If Resolve supplies neither API, FilmViz reports a persistent configuration
error. CPU remains available in the standalone validation tools.

## Direct GPU behavior

Direct GPU rendering does not use a preview-LUT path. Measured profile and rgb2spec
tables are uploaded once per profile/device and shared across nodes; flash,
push/pull, Color Separation, Color Depth, bleach bypass, printer lights, middle
gray, input/output selection, grain and halation remain live kernel parameters.
Halation is evaluated in negative-exposure space before development. Measured
MTF is applied after the direct film render using per-channel kernels generated
from the profile curves. Metal and OpenCL share the same packed parameter layout
and canonical kernel algorithm.

## Standalone CPU reference caches

The standalone CPU reference utilities check transform caches in this order:

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

A normal OFX build pre-generates the neutral/common CPU reference combinations at LUT
size 33 when `FILMVIZ_OFX_PREBAKE_CACHE=ON` (default). The generated cache
contains both negative stocks, both input profiles, and both output profiles,
with Kodak Vision 2383/3383, 25/25/25 printer lights, 3200 K, zero push/pull,
zero bleach
bypass, and middle gray 0.18. Each `.fvcache` contains the input-to-negative-
exposure LUT, the log-exposure development domain, the developed/output LUT,
and the granularity sigma field for validation and comparison tools.

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

## GPU profile caches

Direct Metal and OpenCL nodes separately share immutable measured spectral,
sensitometric, dye, viewer, granularity and rgb2spec buffers. These resources
are keyed by API context/device and stock selection, so creative control changes
require no LUT build or GPU re-upload.

Both direct backends accelerate the complete spectral colour path,
negative/print grain, negative-stage halation and measured negative/print MTF.
Only the small measured MTF coefficient calculation remains on the CPU; all
image-sized spatial work stays on the GPU command queue.

## Standalone CPU/Metal comparison

`filmviz_metal_compare` reads a linear AP0 image, evaluates every pixel through
both `FilmPipeline` and Metal Direct, writes `<prefix>_cpu.exr`,
`<prefix>_metal.exr` and `<prefix>_side_by_side.exr`, and prints RMS and maximum
AP0 error:

```bash
filmviz_metal_compare input.exr comparison/output /path/to/resources
```

Add `--spatial` after the resource path to enable a representative MTF and
halation validation pass against the CPU OFX reference. In that mode the CPU
side intentionally uses the production 33^3 OFX transform, so its reported
error also includes the LUT approximation relative to the direct GPU pipeline.

The cross-platform OpenCL equivalent writes `_cpu.exr`, `_opencl.exr`, and
`_side_by_side.exr`:

```bash
filmviz_opencl_compare input.exr comparison/output /path/to/resources
```

OpenCL is detected with CMake's `FindOpenCL`. When it is unavailable the OFX
plug-in still builds with CPU support (and Metal support on macOS). A Windows
GPU build needs the vendor OpenCL runtime and development import library/header
available to CMake.

## Timeline/performance logging

FilmViz writes a thread-safe OFX performance log by default. On macOS:

```text
~/Library/Logs/FilmViz/filmviz_ofx.log
```

The log includes plug-in load/unload, node creation/destruction, frame time,
exposure, stock selection, GPU profile upload/reuse, and render/encode timing.
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

Resolve presents the controls in collapsible groups:

Setup:

- Input and output profiles

Negative:

- Stock: Kodak Verita 200D 5206/7206 or Kodak Vision3 50D 5203/7203
- Exposure, flash and push/pull
- Color Separation
- Color Depth
- Bleach bypass

Print:

- Stock: Kodak Vision 2383/3383
- Flash and bleach bypass
- Printer R/G/B lights, neutral at 25/25/25
- Printer master timing

Spatial Response:

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

Advanced:

- Middle gray
- Worker threads

MTF, grain and halation are disabled by default. Enabling MTF uses the measured
cycles/mm response and the selected active-image width. Metal Direct and
OpenCL Direct execute the image-sized MTF and halation passes on the GPU. CPU
remains available only in the standalone reference and comparison tools.
Color Separation operates in calibrated negative dye-coordinate space before
spectral density synthesis. Increasing it progressively calms chroma. Zero is
the accepted standard response, -4 is calibrated bypass, and +4 is twice the
standard response. Color Depth independently scales chroma-weighted depth
through print exposure; one is standard, zero removes chromatic darkening and
negative values provide a controlled lift. Both are live parameters in Metal
Direct. Fixed warm shaping
retains more of the warm mid-density dye-coordinate branch and gently guides
near-warm trajectories toward yellow/orange rather than magenta.
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
  -DCMAKE_PREFIX_PATH=/path/to/filmviz-dependencies/arm64.release \
  -DFILMVIZ_BUILD_OFX=ON

cmake --build build --config Release -j
```

Set `CMAKE_PREFIX_PATH` to the directory where you installed FilmViz's
third-party dependencies.

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
        profiles/
        colorimetry/
        spectral/
        cache/
          ofx/
            manifest.txt
            *.fvcache
```

Runtime dependencies are copied into `Contents/Libraries`, rewritten to
bundle-relative load paths, stripped of absolute build-machine `LC_RPATH`
entries, and signed during macOS packaging. Only production runtime resources
are bundled; reference images, charts, documents and supplementary APD material
remain outside the plug-in. For local experiments the
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
- Metal is available on macOS and OpenCL is available when found at build time.
- CUDA is not yet supported; Windows Resolve must supply an OpenCL queue.
- The production OFX plug-in has no CPU fallback.
- Full-frame rendering is requested because halation and MTF are spatial.
- No custom-drawn OFX UI; Resolve renders the standard parameter controls.
- Metal halation uses Metal Performance Shaders Gaussian blur while the CPU
  reference uses FilmViz's CPU spatial approximation, so pixel-level blur can
  differ slightly even though the film-stage model and scatter parameters match.

## Look controls and A/B workflow

Color Response exposes its enable switch, amount, chroma compression/knee,
density center/width, and warm protection/hue controls. Grain exposes tonal
shaping and shadow, midtone and highlight gains. These use the same settings
as the Python application and preserve the measured stock profiles.

**Enable Grain** and **Enable Halation** provide manual A/B control without
changing the stored strengths. Under Spatial Response, **Enable both in
full-quality renders** optionally overrides both switches for non-draft renders.
It defaults off. Set nonzero effect strengths before using the override.

OFX's draft-quality flag is not a final-export detector: full-quality viewer
updates can also enable both effects, and hosts omitting the flag count as full
quality. Use manual checkboxes when the host does not distinguish draft viewer
work from output rendering. Verify the host behavior before relying on the
optional override for delivery.

Parameters are saved with the host project and can be captured using the host's
preset facilities where available. Python app presets remain separate.

## Export LUT

In **LUT Export**, choose a grid size (17, 33 or 65; default 33), then press
**Export LUT** to open the native save dialog on macOS or Windows. Choose a
new `.cube` filename; cancelling the dialog does not generate a LUT. Generation runs synchronously and
may temporarily block the host UI. Existing files are not overwritten.

The LUT samples this node's current-frame color settings through the CPU
spectral pipeline, including selected input/output transforms and Color
Response. It does not include other nodes or the host's color management.
Grain, halation and negative/print MTF are spatial effects and are excluded.
The input domain is 0..1 per channel; linear HDR inputs outside that range
cannot be represented by this export. A sampled LUT approximates the direct
GPU renderer; it is not a replacement for its spatial processing.
