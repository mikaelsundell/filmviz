# FilmViz runtime resources

This directory contains measured data and reference material. Production code
receives this directory through `FilmPipeline::Settings::resources_directory`.

```text
profiles/
  verita_200d/       Kodak Verita 200D 5206/7206 measured CSV data
  kodak_2383/        Kodak Vision 2383/3383 measured and corrected CSV data
colorimetry/
  illuminants/       standard and measured illuminant spectra
  observers/         CIE observer data
densitometry/
  apd/               ACES Academy Printing Density scanner responses
spectral/
  reconstruction/    rgb2spec coefficient tables
references/
  images/            source and expected-result images
  charts/            ColorChecker and DigitalSG chart definitions
  documents/         source technical publications
```

Profile JSON files are neither required nor supported by the production
pipeline. Editable artwork, digitization SVGs and table-generation inputs live
under the project-level `working/` directory instead.

The production Kodak Vision 2383/3383 dye file is explicitly named
`kodak_2383_corrected_spectral_dye_density_curves.csv`; the uncorrected measured
curve remains beside it for comparison.

The APD resource is supplementary measurement data. It is not an AP0 colour
conversion and is not used by the production Verita Status-M calibration.
