# FilmViz

FilmViz is experimental software for learning about spectral colour-negative
and print-film processing. It is a research and education project, not a
production-certified film-stock or colour-management product. The simulator
uses measured stock data to make each stage inspectable: spectral exposure,
densitometric development, dye-density synthesis, print exposure, viewing,
LUT generation and image-grain rendering.

The current pipeline is derived from documented Status-M validation work and is
split into reusable C++ classes, a command-line application and a Python GUI.

The current production profile is:

- input: ARRI Wide Gamut 3 / LogC3 EI800, or linear ACES2065-1
- negative: Kodak Verita 200D
- negative densitometry: ISO Status-M
- print: corrected Kodak 2383
- printer-light approximation: 3200 K
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
    -DCMAKE_PREFIX_PATH=/Volumes/Projects/github/3rdparty/build/macosx/arm64.debug
cmake --build build -j
```

On macOS, select exactly one matching dependency tree per build directory:
`arm64.debug` for Debug or `arm64.release` for Release. The Python application
uses that configured prefix and sets `DYLD_IMAGE_SUFFIX=_debug` when launching
against the debug Qt frameworks.

By default the CTest regression suite and `rgb2spec_opt` are also built. For a
production-only build:

```bash
cmake -S . -B build \
    -DBUILD_TESTING=OFF \
    -DFILMVIZ_BUILD_EXAMPLES=OFF \
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
./build/Debug/example_process_pixel resources
```

Inspect a nonlinear Status-M closure solve:

```bash
./build/Debug/example_density_calibration resources
```

Write diagrams for the current Verita 200D and corrected Kodak 2383 profiles:

```bash
./build/Debug/example_profile_diagrams resources build/profile_diagrams
```

Convert the bundled ARRI AWG3/LogC3 reference image to a 16-bit Rec.709/Gamma
2.4 TIFF through a production 33^3 LUT, with measured Verita and 2383 grain:

```bash
./build/Debug/filmviz \
    -v \
    --resources resources \
    --input awg3-logc3-ei800 \
    --output rec709-gamma24 \
    --input-image resources/references/images/ARRI_Helen_John_ALEXA_Mini_LF_AWG3_LogC3.tif \
    --output-image build/ARRI_Helen_John_filmviz_rec709_gamma24_grain.tif \
    --lutsize 33 \
    --threads 0 \
    --exposure 0 \
    --push-pull 0 \
    --negative-grain 1 \
    --print-grain 1 \
    --grain-size 1.5 \
    --grain-chroma 1 \
    --grain-seed 42
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
rgb2spec spectral reconstruction
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
Kodak 2383 development + dye synthesis
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

The PySide6 application exposes the same supported profiles and controls as the
main command-line tool. It converts images and writes `.cube` LUTs through the
`filmviz_python` pybind11 module, without launching a subprocess. Build its
target and use the generated environment-aware launcher:

```bash
cmake --build build --config Debug --target python_filmviz_app
./build/Debug/python_filmviz_app.sh
```

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
- `lut3d.*` — LUT generation, interpolation, validation and `.cube` output
- `granularitymodel.*` — measured negative/print diffuse-RMS lookup and seeded noise
- `imageprocessor.*` — image I/O, LUT application and two-stage grain rendering
- `threading.*` — process-wide worker configuration
- `python/` — pybind11 module and PySide6 image/LUT application
- `filmprocessor.*` — negative spectral exposure + characteristic development
- `filmdyemodel.*` — negative spectral-density synthesis
- `printfilmprocessor.*` — print exposure/development
- `printviewer.*` — spectral print viewing -> XYZ -> D60/AP0
- `tests/` — deterministic CTest regression suite
- `examples/` — current production API and calibration examples
- `resources/` — organized runtime data and references
- `working/` — non-runtime source artwork and digitization material

See [ARCHITECTURE.md](ARCHITECTURE.md) for the model boundaries,
[CALIBRATION.md](CALIBRATION.md) for the measurements behind the current model,
and [DEVELOPMENT.md](DEVELOPMENT.md) for the test workflow.
