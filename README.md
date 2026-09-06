# FilmViz

FilmViz is experimental software for learning about spectral colour-negative
and print-film processing. It is a research and education project, not a
production-certified film-stock or colour-management product. The simulator
uses measured stock data to make each stage inspectable: spectral exposure,
densitometric development, dye-density synthesis, print exposure, viewing,
LUT generation and measured image-space grain/MTF rendering.

FilmViz grew from a few late-night experiments into a useful working tool.
Codex was used throughout as an AI development collaborator for implementation,
investigation and documentation. The underlying model is based on established
colour-science mathematics, published references and measured film data rather
than AI-generated colour recipes. Careful testing, visual inspection, learning,
hands-on training and iteration have all been part of the process, but the
project remains experimental and its results should be evaluated accordingly.

The current pipeline is derived from documented Status-M validation work and is
split into reusable C++ classes, a command-line application and a Python GUI.

The default production profile is:

- input: ARRI Wide Gamut 3 / LogC3 EI800, or linear ACES2065-1
- negative: Kodak Verita 200D 5206/7206
- alternate supported negative: Kodak Vision3 50D 5203/7203
- negative densitometry: ISO Status-M
- print: corrected Kodak Vision 2383/3383
- printer illuminant approximation: 3200 K
- neutral printer-light controls: R/G/B 25/25/25
- print viewing: D55, adapted to ACES D60
- production output: linear ACES2065-1 (AP0)
- optional preview output: Rec.709 / Gamma 2.4, **without an ACES RRT/ODT**

The active regression suite lives under `tests/`. Concise examples under
`examples/` demonstrate the production API and Status-M calibration without
depending on obsolete profile JSON files.

## Build

FilmViz requires CMake 3.23+, a C++17 compiler, Imath and OpenImageIO. The
optional Python application additionally uses pybind11 and PySide6.

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=/Volumes/Projects/github/3rdparty/build/macosx/arm64.debug
cmake --build build -j
```

On macOS, select exactly one matching dependency tree per build directory:
`arm64.debug` for Debug or `arm64.release` for Release. The Python application
uses that configured prefix and sets `DYLD_IMAGE_SUFFIX=_debug` when launching
against the debug Qt frameworks.

With a single-config generator, executables and the Python launcher are written
to `build/bin/`. Multi-config generators such as Xcode instead use the selected
configuration directory, for example `build/Debug/` or `build/Release/`.

The CTest regression suite, examples, Python application, OpenFX plug-in and
`rgb2spec_opt` are enabled by default. Components whose dependencies are not
available are disabled during configuration where supported. For a CLI-only
build:

```bash
cmake -S . -B build \
    -DBUILD_TESTING=OFF \
    -DFILMVIZ_BUILD_EXAMPLES=OFF \
    -DFILMVIZ_BUILD_PYTHON_APP=OFF \
    -DFILMVIZ_BUILD_OFX=OFF \
    -DFILMVIZ_BUILD_RGB2SPEC_OPT=OFF
cmake --build build -j
```

Run the active tests with:

```bash
ctest --test-dir build -C Debug --output-on-failure
```

## Examples

Process one AWG3/LogC3 value through every production stage:

```bash
./build/bin/example_process_pixel resources
```

Inspect a nonlinear Status-M closure solve:

```bash
./build/bin/example_density_calibration resources
```

Write diagrams for the current Kodak Verita 200D 5206/7206 and corrected Kodak
Vision 2383/3383 profiles:

```bash
./build/bin/example_profile_diagrams resources build/profile_diagrams
```

Convert the bundled ARRI AWG3/LogC3 reference image to a 16-bit Rec.709/Gamma
2.4 TIFF through a production 33^3 LUT, with measured Verita and 2383 grain:

```bash
./build/bin/filmviz \
    -v \
    --resources resources \
    --input awg3-logc3-ei800 \
    --output rec709-gamma24 \
    --input-image resources/references/images/ARRI_Helen_John_ALEXA_Mini_LF_AWG3_LogC3.tif \
    --output-image build/ARRI_Helen_John_filmviz_rec709_gamma24_grain.tif \
    --lutsize 33 \
    --threads 0 \
    --exposure 0 \
    --negative-flash 0 \
    --print-flash 0 \
    --push-pull 0 \
    --printer-light-master 0 \
    --negative-grain 1 \
    --print-grain 1 \
    --grain-size 1.5 \
    --grain-chroma 1 \
    --grain-seed 42 \
    --film-format super-35 \
    --negative-mtf 1 \
    --print-mtf 1
```

Image grain is opt-in. Strength `1` uses the digitized diffuse-RMS density
amplitude; `0` disables that stage. Negative and print grain use independent,
deterministic spatial fields. `--grain-size` controls correlation in output
pixels and is a rendering parameter because the Kodak curves do not define a
complete spatial noise spectrum.

`--grain-chroma` controls only the channel differences in the grain while
preserving its Rec.709-weighted luminance component. `0` makes the texture
neutral, `1` preserves the measured independent-channel result, and values
between them reduce colour speckling without weakening luminance grain.

`--exposure` is a camera exposure adjustment in stops. `--push-pull` changes
negative contrast around calibrated middle gray. Push/pull is explicitly an
approximation: FilmViz has no alternate-development Verita measurements.

`--negative-flash` and `--print-flash` add uniform record exposure before the
respective characteristic curves. They are percentages of each stage's
calibrated neutral reference exposure. `--printer-light-master` is a linked
offset added to the R/G/B printer-light values; one point is 0.025 LogE.

`--negative-mtf 1` and `--print-mtf 1` apply the measured stock responses as a
cascaded small-signal system MTF. The selected `--film-format` maps the measured
cycles/mm axis to image pixels using Regular 8, Super 8, 16mm, Super 16, 35mm,
Super 35 or 65mm active-image widths. Use `custom` with `--image-width-mm` for
another aperture or crop. MTF is image-only and cannot be stored in a `.cube`.

`--threads 0` uses the machine's hardware concurrency. A positive value sets a
process-wide worker limit shared by LUT sampling and image-row conversion.

The examples use only the current C++ API and measured CSV resources. They do
not load profile JSON files. See [examples/README.md](examples/README.md).

## Resources

Place the FilmViz `resources` directory next to the built executable,
or pass an explicit path:

```bash
./filmviz --resources /path/to/resources
```

Runtime resources are grouped by purpose beneath `resources/profiles/`,
`resources/colorimetry/` and `resources/spectral/`. Reference images, charts
and source publications live under `resources/references/`. Editable working
material is kept outside the runtime tree under `working/`.

## Basic use

Generate the reference 33³ LUT:

```bash
./filmviz \
    --resources ./resources \
    --input awg3-logc3-ei800 \
    --negative verita-200d \
    --print kodak-2383 \
    --output ap0-linear \
    --lutsize 33 \
    --outputcube verita2383_ap0.cube
```

List supported profiles:

```bash
./filmviz --profiles
```

Show all options:

```bash
./filmviz --help
```

A direct Rec.709/Gamma 2.4 preview LUT can be generated with:

```bash
./filmviz --output rec709-gamma24 --outputcube verita2383_rec709.cube
```

That preview is a colour-space/transfer-function conversion only. FilmViz does
not apply the ACES RRT or an ACES Output Transform. The authoritative spectral
result is `ap0-linear`.

## Production pipeline

```text
ARRI AWG3 / LogC3 EI800
        |
        v
linear ACES2065-1 / AP0
        |
        v
exposure-separated rgb2spec reconstruction
(AP0/D60 luminance above Y=0.18 is reconstructed at Y=0.18,
then exposure is restored by spectral rescaling; scene values may exceed 1)
        |
        v
CIE D60 scene illumination
        |
        v
Verita spectral exposure
        |
        v
Verita characteristic curves
(Kodak Status-M density coordinates)
        |
        v
FilmDensityCalibration
(Status-M -> spectral-model coordinates)
        |
        v
FilmDyeModel spectral density
        |
        v
negative transmittance
        |
        v
3200 K printer exposure
        |
        v
Kodak Vision 2383/3383 development + dye synthesis
        |
        v
D55 print viewing / CIE 1931
        |
        v
Bradford D55 -> D60
        |
        v
linear ACES2065-1 / AP0
```

The important production change established by measurement validation is the explicit
`FilmDensityCalibration` stage. Kodak sensitometric Status-M densities are not
numerically interchangeable with the internal spectral-dye basis coordinates.
The calibration solves that measurement-coordinate conversion nonlinearly and
keeps the measured spectral D-min as the lower physical boundary.

There is no empirical `1.5x` contrast multiplier in the production path.

## Python application

The PySide6 application uses the same C++ spectral pipeline as the command-line
tool. In addition to image and LUT processing controls, it provides interactive
display selection, direct/no-print processing, pixel probes and diagnostic
scopes. It calls the `filmviz_python` pybind11 module directly without launching
a subprocess. Build its target and use the generated environment-aware launcher:

```bash
cmake --build build --config Debug --target python_filmviz_app
./build/bin/python_filmviz_app.sh
```

For a multi-config build, the corresponding launcher is under the selected
configuration directory, such as `build/Debug/python_filmviz_app.sh`.

The launcher uses the Python executable, dependency prefix, module path and
macOS Qt framework suffix selected during CMake configuration. The worker-count
field controls the same global C++ thread setting as `filmviz --threads`. See
[python/README.md](python/README.md) for complete build and launch details.

## Source layout

- `main.cpp` — supported `filmviz` command-line entry point
- `filmpipeline.*` — production end-to-end spectral pipeline
- `filmdensitycalibration.*` — nonlinear Status-M -> spectral coordinate solve
- `statusmdensitometer.*` — ISO Status-M measurement implementation
- `inputtransform.*` — camera/input encoding to AP0
- `negativeprofile.*` — canonical negative-profile names and resource metadata
- `printprofile.*` — canonical print-profile names and resource metadata
- `lut3d.*` — LUT generation, interpolation, validation and `.cube` output
- `granularitymodel.*` — measured negative/print diffuse-RMS lookup and seeded noise
- `filmformat.*` — shared physical active-image format metadata
- `spatialresponsemodel.*` — measured cycles/mm MTF to image-space response
- `imageprocessor.*` — image I/O, LUT application, grain and spatial response
- `threading.*` — process-wide worker configuration
- `python/` — pybind11 module and PySide6 image/LUT application
- `ofx/` — OpenFX front end, shared transform cache and Metal renderer
- `filmprocessor.*` — negative spectral exposure + characteristic development
- `filmdyemodel.*` — negative spectral-density synthesis
- `printfilmprocessor.*` — print exposure/development
- `printviewer.*` — spectral print viewing -> XYZ -> D60/AP0
- `tests/` — deterministic CTest regression suite
- `regression_set/` — committed numerical and visual production stamp
- `examples/` — current production API and calibration examples
- `resources/` — organized runtime data and references
- `working/` — non-runtime source artwork and digitization material

See [ARCHITECTURE.md](ARCHITECTURE.md) for the model boundaries,
[CALIBRATION.md](CALIBRATION.md) for the measurements behind the current model,
and [DEVELOPMENT.md](DEVELOPMENT.md) for the test workflow.
