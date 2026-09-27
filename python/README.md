# FilmViz Python application

The Python application uses PySide6 for its interface and the `filmviz_python`
pybind11 module for direct access to the same C++ spectral pipeline as the
`filmviz` command-line tool. Both LUT sampling and image rows use FilmViz's
process-wide worker-thread setting.

Build the project normally with `FILMVIZ_BUILD_PYTHON_APP=ON` (the default).
The binding is built when pybind11 and Python development files are available.

```bash
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH=/path/to/filmviz-dependencies/arm64.debug
cmake --build build -j
```

For a release build, configure with the release dependency tree instead:

```bash
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH=/path/to/filmviz-dependencies/arm64.release
cmake --build build --config Release -j
```

Set `CMAKE_PREFIX_PATH` to the directory where you installed FilmViz's
third-party dependencies. Use exactly one dependency prefix per build
directory. Reconfigure or use a separate build directory when switching
between debug and release dependencies.

Build the application target and run its generated launcher:

```bash
cmake --build build --config Debug --target python_filmviz_app
./build/Debug/python_filmviz_app.sh
```

For Release, use `--config Release` and run
`./build/Release/python_filmviz_app.sh`. The launcher embeds the Python
interpreter, dependency prefix, extension-module directory and project root
selected by CMake. It also applies the matching macOS Qt framework suffix, so
no shell environment setup is required.

Running `python3 python/filmviz_app.py` directly remains supported when that
Python environment can already import both PySide6 and `filmviz_python`.

The image interface exposes negative and print flash, linked master printer
timing, Color Separation, Color Depth, density-dependent negative/print grain,
and measured negative/print MTF. Color Separation is a
neutral-preserving transform in calibrated negative dye-coordinate space that
progressively calms chroma. Zero is the accepted standard response, -4 restores
calibrated bypass, and +4 applies twice the standard response. Color Depth
independently controls chroma-weighted depth through print exposure: one is the
accepted response, zero removes chromatic darkening and negative values lift
chromatic regions. The standard response retains more of the warm mid-density
branch and gently guides near-warm trajectories toward yellow/orange rather
than magenta. Film-format presets supply the physical active-image width
used to convert the MTF curves from cycles/mm to pixels; Custom enables direct
width entry. These controls call the shared C++ models rather than duplicating
film logic in Python.

If PySide6 is installed in the same non-system dependency prefix, the app reads
`build/CMakeCache.txt` and adds that prefix's Python site-packages directory.
You can also set `FILMVIZ_DEPENDENCY_PREFIX` explicitly.

On macOS the selected dependency prefix also controls Qt framework selection.
For a prefix ending in `.debug`, the app re-launches with the CMake-selected
Python interpreter and `DYLD_IMAGE_SUFFIX=_debug`; a `.release` prefix runs
without that suffix. This prevents debug and release Qt frameworks from being
loaded into the same process.

For an automated startup check that constructs the complete window without
entering the event loop, set `FILMVIZ_APP_SMOKE_TEST=1`.

### Color Response tuning

The Color Response tab exposes response amount, chroma compression, chroma knee,
color depth, density center/width, warm protection, warm hue center/width and
warm hue shift. Bypass preserves the slider settings; Reset Color Response
restores the previous standard look. Density controls operate in normalized
negative dye coordinates. Hue angles are dye-plane rotations, not display HSL.

The diagnostic plot compares a fixed dye-chroma slice with calibrated bypass
(gray), the standard response (blue), and the tuned response (amber). It is not
a gamut diagram or a measurement of the rendered image. Use the image and scopes
to judge the final look, including color depth, which the chroma-only plot omits.

Python `process_image`, `generate_lut`, and `probe_image_pixel` accept an optional
`color_response` dictionary with the nine tuning keys (Color Depth remains the
existing `color_depth` argument). Realtime Metal preview consumes the same
settings. Saved LUT comments include the tuning dictionary.

Film format and active image width appear above the image grain controls. Grain
scale is a multiplier referenced to 2048-pixel-wide Super 35; both preview and
export derive its pixel footprint from their image width and the active film
width. Smaller formats enlarge grain at equal output resolution. Negative and
print strength remain separate controls; their defaults have not been boosted.

The revised grain renderer uses pixel-area integration and a reference-aperture
normalization. At the same numeric settings it can have more presence than the
previous texture. Metal preview evaluates each noisy film stage; CPU exports
use cached local density-response derivatives. Preview and export therefore
share the model but can differ for strong grain or near clipped colors. Grain
response LUT preparation is more expensive. The stage MTF no longer blurs both
grain components as one finished image. No additional strength boost is applied.
# Tonal grain rendering controls

The **Preset** row saves named looks in local application settings (QSettings,
organization/application `FilmViz`). Save creates a preset or confirms replacing
an existing name; Load restores it; Delete confirms removal. Names are sorted
alphabetically and presets persist across app restarts.

Presets include input/output color profiles, film stocks, color response,
exposure and printer controls, film format, grain, MTF, halation and effect
bypasses. They retain slider values even for bypassed effects. They exclude
file/resource paths, runtime edits to measured profile curves, performance
settings and preview state. Loading validates available profiles before
applying the look; with live preview off, convert again to see the result.

**Enable grain** and **Enable halation** independently bypass their effects
for A/B comparison in conversion and live preview. Bypass sends zero effect
strength without altering the displayed values, so re-enabling restores the
chosen settings. Both switches default to enabled; image reset restores them.
With live preview off, convert again to see the selected state.

The image controls expose **Enable tonal grain shaping**, **Shadow grain**,
**Midtone grain**, and **Highlight grain**. These are empirical rendering
controls, not modifications to measured stock data. Multipliers range from
0 to 2; all three at 1 preserve the current look. Unchecking tonal shaping
bypasses the entire tonal attenuation, leaving strength, chroma, size and
format controls active. Reset restores enabled shaping and multipliers of 1.

Smooth overlapping tonal weights use the noise-free maximum working-linear
RGB channel. These are fixed working-space ranges, not scene exposure stops.
Both image conversion and live Metal preview receive the controls. Python
`process_image` accepts `grain_tonal_enabled`, `grain_shadows`,
`grain_midtones`, and `grain_highlights` as optional trailing keywords.
Image metadata and application logs retain the settings. LUTs are unaffected.
