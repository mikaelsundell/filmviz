# FilmViz regression set

This directory contains the committed numerical and visual stamp for the
production pipeline and measured spatial response. `test_regression_set`
checks representative neutral, red, skin-like and blue AP0 samples through the
standard response, calibrated Color Density bypass, uniform warm compression
and a shaped warm-separation setting. It then checks a deterministic synthetic
image through the measured negative-plus-print MTF model.

Run the normal check with CTest. After an intentional model change, inspect the
differences first, then update both references explicitly:

```bash
cmake --build build --target update_regression_set
git diff -- regression_set
```

For a multi-config build, add `--config Release` or the configuration in use.
Commit `reference_stats.csv` and `reference_mtf.ppm` with every accepted model
change. Never update the stamp merely to make a failing regression pass.
