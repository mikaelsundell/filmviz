# FilmViz development guide

## Build policy

FilmViz uses C++17 because the production tool uses `std::filesystem`. Do not
lower the project standard while `main.cpp`/`FilmPipeline` depend on it.

The build has three main parts:

- `filmviz_core` — reusable library
- `filmviz` — production CLI
- `tests/test_*.cpp` — deterministic regression executables registered with
  CTest

Current API examples live under `examples/` and are excluded with
`FILMVIZ_BUILD_EXAMPLES=OFF` when a production-only build is desired.

Configure without resource files if necessary; compilation must not depend on
the resource directory being present. Runtime spectral tests do require the
full Resources set.

## Working rule

Keep the production path conservative. Investigate new physical hypotheses in
a focused, clearly named diagnostic before changing the core. If the diagnostic
is useful to API users, keep a concise version under `examples/`. Promote a
behavior into the core and add a focused `test_*` regression only after the
evidence establishes why it belongs there.

Do not replace a measured/modelled relationship with a visual tuning constant
unless the project explicitly decides to add a creative look layer.

## Validation sequence

The measurements and conclusions from the original numbered calibration work
are recorded in `CALIBRATION.md`. The important reasoning chain is:

- `calibrate3` — physical contrast-budget and negative forensics
- `calibrate4` — independent/reference-coordinate investigation
- `calibrate5` — Kodak-vs-Kodak Status-M closure and digitization audit
- `calibrate6` — nonlinear Status-M -> spectral coordinate closure
- `calibrate7` — chromatic closure, separability and stage tracing
- `final1d` — end-to-end validated production baseline and LUT bake

The current production architecture is based on the Calibrate5-7 result, not on
the earlier empirical Final1c contrast multiplier.

## What must remain true

When changing the negative calibration:

- zero-stop spectral state remains anchored exactly;
- measured spectral D-min remains the lower physical boundary;
- neutral Status-M closure remains tight over the physically achievable range;
- chromatic closure remains tight in the useful operating region;
- no hidden global contrast multiplier is introduced.

When changing print/viewing:

- keep the calibrated 2383 C/M/Y amplitudes explicit;
- keep D55 viewing and D55->D60 Bradford adaptation explicit;
- do not silently add an ACES RRT/ODT to `ColorTransform` or `PrintViewer`.

## Recommended production smoke test

First run the deterministic suite:

```bash
ctest --test-dir build -C Debug --output-on-failure
```

For a full 33^3 LUT smoke test with Resources available:

With Resources available:

```bash
./filmviz \
    --resources ./Resources \
    --input awg3-logc3-ei800 \
    --output ap0-linear \
    --lutsize 33 \
    --outputcube build/filmviz_33.cube \
    -v
```

Leave validation enabled. The tool evaluates a 5x5x5 set of off-grid points
through the direct spectral path and compares them to trilinear LUT sampling.

## Resource data

Do not silently rename or reinterpret CSV columns. The recent forensics
explicitly established:

- Verita sensitivity: yellow-forming -> B, magenta-forming -> G,
  cyan-forming -> R;
- Verita characteristic curves: `curve_low` -> R, `curve_mid` -> G,
  `curve_high` -> B;
- Verita stop/logE digitization: `logE = -0.515 + 0.3 * stops`;
- spectral D-min/midscale source is 10 nm data represented on a 5 nm working
  grid by linear interpolation.

If source data changes, rerun the relevant calibration program rather than
assuming compatibility.

## Source-editing policy

Prefer surgical changes. Preserve unrelated formatting and the historical test
programs. Production classes should stay small and single-purpose; diagnostics
may be verbose because they exist to expose intermediate states.
