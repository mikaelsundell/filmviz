# FilmViz examples

These examples use the current production API and measured CSV resources. They
do not load external profile JSON files.

`example_process_pixel` converts one AWG3/LogC3 EI800 value to AP0 and prints
the important negative, print and viewing stages:

```bash
./build/Debug/example_process_pixel resources
```

`example_density_calibration` demonstrates the nonlinear conversion from a
requested Kodak Status-M density to `FilmDyeModel` spectral coordinates:

```bash
./build/Debug/example_density_calibration resources
```

Both accept the resources directory as their optional first argument.

`example_profile_diagrams` loads the current Verita 200D and corrected Kodak
2383 profiles and writes their measured sensitivities, characteristic curves,
dye-density data and related diagnostics as PNG files:

```bash
./build/Debug/example_profile_diagrams resources build/profile_diagrams
```

Its first argument is the resources directory and its second argument is the
output directory. It uses `DiagramWriter` and does not load profile JSON files.

`example_convert_arri_image` converts the bundled Helen and John AWG3/LogC3
TIFF through the spectral pipeline and writes a 16-bit Rec.709/Gamma 2.4 TIFF
with both measured grain stages enabled:

```bash
./build/Debug/example_convert_arri_image \
    resources \
    resources/references/images/ARRI_Helen_John_ALEXA_Mini_LF_AWG3_LogC3.tif \
    build/ARRI_Helen_John_filmviz_rec709_gamma24_grain.tif \
    33 0 0 1 1 1.5 42 1
```

Arguments after the filenames are LUT size, exposure stops, push/pull stops,
negative-grain strength, print-grain strength, grain size in pixels, seed, and
grain chroma. Grain chroma `0` is neutral and `1` preserves the measured
per-channel result. The defaults are `33 0 0 1 1 1 1 1`.
