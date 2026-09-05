# AGENTS.md — FilmViz coding context

This file is the entry point for Codex/agent-assisted work on FilmViz.
Read `ARCHITECTURE.md` and `CALIBRATION.md` before changing the spectral model.

## Project purpose

FilmViz is a measured-data spectral film simulator. The supported production
executable is `filmviz`. Current API demonstrations live under `examples/`.

## Canonical production baseline

Treat these as the current architecture:

```text
encoded RGB
 -> InputTransform
 -> AP0 linear
 -> spectral reconstruction + D60
 -> FilmProcessor (Verita exposure/development)
 -> Kodak Status-M density
 -> FilmDensityCalibration
 -> FilmDyeModel spectral coordinates
 -> negative D(lambda) / transmittance
 -> PrintFilmProcessor (Kodak 2383)
 -> print dye D(lambda) / transmittance
 -> PrintViewer (D55, CIE XYZ, Bradford D60)
 -> AP0 linear
```

The direct Rec.709/Gamma 2.4 path is only a preview conversion. Do not add an
ACES RRT/ODT unless a task explicitly asks for a separate display-rendering
stage.

## Critical discovery to preserve

Kodak Verita characteristic-curve densities are ISO Status-M measurement
coordinates. They are **not** numerically the same as the internal coordinates
used by `FilmDyeModel` to synthesize spectral density.

`FilmDensityCalibration` exists to solve that coordinate conversion. It is not
a creative contrast control. Do not bypass it in production by feeding
`FilmProcessor::develop()` directly to `FilmDyeModel::synthesize_density()`.

The earlier experimental ~1.5 contrast/density multiplier has been superseded
by the Status-M closure method.

## Current fixed profile values

- negative: Kodak Verita 200D
- negative zero-stop log exposure: -0.515
- middle gray: AP0 0.18
- scene illuminant: CIE D60
- print: corrected Kodak 2383
- printer blackbody approximation: 3200 K
- print dye amplitudes C/M/Y: 1.10093 / 1.09650 / 1.14626
- viewing illuminant: D55
- wavelength grid: 380..700 nm / 5 nm
- standard production LUT: 33^3

Treat these as profile calibration values, not arbitrary knobs.

Optional image controls do not alter the fixed baseline: grain is rendered only
in image output and never baked into a `.cube`; push/pull is the documented
`2^(0.2 * stops)` approximation until alternate-development curves exist.
Grain chroma is a luminance-preserving rendering control, with `1` retaining
the measured per-channel result and `0` producing neutral grain.

## Code ownership

- `main.cpp`: CLI only. Do not put spectral algorithms here.
- `filmpipeline.*`: orchestration/loading and end-to-end `process(AP0)`.
- `filmdensitycalibration.*`: nonlinear measurement-coordinate solve.
- `statusmdensitometer.*`: ISO Status-M measurement only.
- `filmdyemodel.*`: spectral-density basis synthesis only.
- `filmprocessor.*`: negative exposure + characteristic curves only.
- `printfilmprocessor.*`: print exposure + characteristic curves only.
- `printviewer.*`: viewed spectrum -> XYZ -> D60/AP0 only.
- `inputtransform.*`: encoded input -> AP0 only.
- `lut3d.*`: generic LUT storage/generation/validation only.
- `granularitymodel.*`: measured density-to-RMS curves and deterministic noise only.
- `imageprocessor.*`: image I/O, LUT application and two-stage grain rendering.
- `threading.*`: process-wide worker count for LUT and image parallelism.
- `python/`: thin pybind11 and PySide6 application adapters; no spectral algorithms.

Avoid collapsing these boundaries.

The ACES APD scanner resource under `resources/densitometry/apd/` is a separate
densitometric system. It is not AP0 colour-matching data and must not replace
Status-M calibration without an explicit, independently validated model change.

## Tests and experiments

Active regression programs live in `tests/`, use the `test_*` naming
convention, and are registered with CTest. Each must make deterministic
assertions and return a failing exit status when an invariant is broken.

The forensic conclusions behind the model are recorded in `CALIBRATION.md`.
Examples under `examples/` must use the current production API, have descriptive
names, and must not depend on external profile JSON files.

When adding a new physical hypothesis, create a focused diagnostic first. Do
not modify production rendering to match a visual expectation without
supporting measurement/data. Once accepted in production, record the evidence
in `CALIBRATION.md` and add a focused CTest regression for the resulting
invariant.

## Resources

The source archive may omit measured resource files. Runtime code should find
`resources` beside the executable or use `--resources`. CMake must remain
configurable when the resource directory is absent.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Dependencies: C++17, Imath, OpenImageIO, Threads.

## Editing style

Make focused changes. Preserve existing formatting and test history. Prefer
small reusable classes over copying logic into tests or `main.cpp`.
