# FilmViz architecture

## Design goal

FilmViz models the photographic chain with measured spectral data. Each class
owns one physical or mathematical responsibility. Production code should not
re-introduce calibration logic into `main.cpp` or merge measurement spaces that
have deliberately been separated.

## Coordinate domains

Three density domains must be kept distinct:

1. **Scene/log exposure** — physical negative exposure and the x-axis of the
   characteristic curves.
2. **Published sensitometric density** — Kodak Verita R/G/B characteristic
   curves, interpreted as ISO Status-M measurements.
3. **Spectral-model coordinates** — the internal coefficients consumed by
   `FilmDyeModel` to synthesize wavelength-dependent optical density.

Calibrate5-7 demonstrated that domains 2 and 3 are not numerically identical.
`FilmDensityCalibration` is the explicit bridge.

## Main classes

### `InputTransform`

Converts supported encoded camera RGB into linear ACES2065-1/AP0. The current
production input is AWG3/LogC3 EI800. It also accepts AP0 directly for tests and
future workflows.

### `SpectralReconstructor`

Uses the rgb2spec table to reconstruct an approximate scene spectral factor
from AP0 RGB.

### `FilmProcessor`

Integrates the illuminated scene spectrum against the negative stock's measured
spectral sensitivities and evaluates the characteristic curves. For Verita,
the returned `FilmDensity` is a Status-M sensitometric coordinate.

### `FilmDyeModel`

Synthesizes the negative's total spectral optical density from internal
spectral-basis coordinates. It is intentionally ignorant of Status-M. Do not
feed raw Kodak characteristic-curve densities directly into this class in
production.

### `StatusMDensitometer`

Remeasures a synthesized negative spectrum with ISO Status-M spectral products.
This lets FilmViz compare the synthetic spectrum in the same measurement
coordinates as Kodak's characteristic curves.

### Supplementary APD resource

`Resources/densitometry/apd/aces_apd_scanner_responsivities.csv` contains ACES
Academy Printing Density scanner responses. APD is a separate densitometric
measurement system, not an AP0 colour conversion. It is retained for future
measurement work and is not part of the production pipeline.

### `FilmDensityCalibration`

Solves the inverse coordinate problem:

```text
requested Kodak Status-M density
             |
             v
candidate FilmDyeModel coordinate
             |
             v
synthesized D(lambda)
             |
             v
ISO Status-M remeasurement
             |
             +---- iterate until measurement matches target
```

The zero-stop measured spectrum is an exact anchor. The measured D-min spectrum
is a lower physical boundary. Interior targets normally close to numerical
precision. Unreachable extreme chromatic LUT corners retain the nearest finite
least-squares spectral state rather than failing LUT generation.

### `PrintFilmProcessor`

Exposes Kodak 2383 through the negative transmittance and evaluates the print
characteristic curves. The current printer source is a 3200 K Planckian
approximation.

### `PrintViewer`

Views the print spectral transmittance under D55, integrates CIE 1931 XYZ,
adapts the viewed white to D60 and returns linear AP0.

This AP0 value represents the already-rendered print result. It is not a
scene-referred signal awaiting an ACES RRT.

### `ColorTransform`

Performs colour-space matrices and transfer functions only. It does not contain
an ACES RRT or ODT. In FilmViz it is used to make the optional Rec.709/Gamma 2.4
preview.

### `Lut3D`

Samples the full spectral pipeline into an RGB 3D LUT, writes `.cube`, applies
trilinear interpolation and validates the LUT against direct spectral samples.

### `GranularityModel`

Loads the digitized diffuse-RMS granularity curves for Verita 200D and Kodak
2383 and maps each stage's developed density to an RGB density standard
deviation. Random sampling is deterministic for a seed and pixel coordinate.

### `ImageProcessor`

Reads and writes images, builds the spectral LUT used for the image conversion,
and applies the two independent granularity fields. Negative-density noise and
print-density noise have opposite signs in the final transmittance response:
more negative density makes a lighter print, while more print density makes a
darker viewed result.

Grain is a first-order image-domain propagation of the measured per-stage RMS
density, not a new spectral solve for every grain sample. `grain_size_pixels`
controls spatial correlation and is a rendering parameter rather than a stock
calibration value. Grain is intentionally absent from deterministic `.cube`
output.

The optional grain-chroma rendering control decomposes the combined density
noise into a Rec.709-weighted neutral component and channel-difference
components. Scaling the differences leaves weighted luminance noise unchanged:
zero produces neutral grain and one preserves the measured per-channel result.

### `FilmPipeline`

Owns and connects the production components. Its public contract is deliberately
small:

```cpp
FilmPipeline pipeline;
pipeline.initialize(settings);
FilmPipeline::Result result = pipeline.process(ap0_linear);
```

## Fixed production calibration

The current Kodak 2383 spectral dye amplitudes are:

```text
C = 1.10093
M = 1.09650
Y = 1.14626
```

These came from the Calibrate2 JIS/D55 compromise and are profile calibration
values, not user look controls.

The Verita Status-M calibration is data-driven and replaces the earlier
experimental global density/contrast multiplier.

Exposure compensation shifts negative log exposure by stops. Push/pull scales
negative Status-M density around the calibrated middle-gray density by
`2^(0.2 * stops)`. This is explicitly an approximation until measured
alternate-development characteristic curves are available.

## Display output

The production LUT output should normally be `ap0-linear`.

`rec709-gamma24` is provided as a convenient preview path:

```text
viewed AP0 -> AP0/Rec709 matrix -> Gamma 2.4
```

No ACES RRT/ODT is applied. If FilmViz is integrated into an ACES colour
management pipeline, use the AP0 output and apply the intended ACES Output
Transform separately.
