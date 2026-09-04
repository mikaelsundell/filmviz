# FilmViz tests

The active suite contains small, deterministic executables registered with
CTest. Every source and executable uses the `test_*` naming convention.

- `test_input_transform` checks profile parsing, AP0 pass-through and the ARRI
  LogC3 EI800 middle-gray conversion.
- `test_apd_resource` checks the canonical ACES Academy Printing Density
  scanner grid, ordering and calibrated peak values.
- `test_lut3d` checks LUT generation, storage, trilinear interpolation and
  direct-versus-LUT validation.
- `test_density_calibration` checks the production nonlinear Status-M closure,
  its exact zero-stop anchor and the measured D-min boundary.
- `test_pipeline` exercises representative neutral and chromatic AP0 samples
  through the complete negative, print and viewing pipeline.

Build and run the suite with:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build -C Debug --output-on-failure
```

The calibration evidence behind these invariants is summarized in
`CALIBRATION.md`. Current API demonstrations live under `examples/`; they are
not regression tests and are intentionally excluded from CTest.
