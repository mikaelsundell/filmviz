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

measurement-validation diagnostics demonstrated that domains 2 and 3 are not numerically identical.
`FilmDensityCalibration` is the explicit bridge.

## Main classes

### `InputTransform`

Converts supported encoded camera RGB into linear ACES2065-1/AP0. The current
production input is AWG3/LogC3 EI800. It also accepts AP0 directly for tests and
future workflows.

### `SpectralReconstructor`

Uses the rgb2spec table to reconstruct an approximate scene spectral factor
from AP0 RGB. `FilmPipeline` separates scene exposure before reconstruction:
AP0 values above D60 luminance 0.18 are reconstructed at Y=0.18, then the
resulting spectrum is scaled back to the original scene exposure. This keeps
spectral shape homogeneous through highlights and allows scene spectral power
to exceed the bounded-reflectance range without forcing rgb2spec to select a
different metamer.

### `NegativeProfileCatalog`

Owns the canonical identifiers, display names and resource locations for every
camera-negative profile supported by `FilmPipeline`. CLI, Python and OpenFX
adapters read this catalog rather than maintaining interface-specific stock
names. It contains profile metadata only; loading and spectral processing stay
within their existing model classes.

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

`resources/densitometry/apd/aces_apd_scanner_responsivities.csv` contains ACES
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

### `FilmColorResponse`

Applies an optional neutral-axis colour-shaping transform after Status-M
calibration and before negative spectral-density synthesis. It normalizes the
three calibrated dye coordinates by the stock's D-min-to-midscale increments,
preserves exact neutrals, progressively compresses channel differences and
reduces their common negative-density coordinate in proportion to chroma. The
latter sends more exposure to the print stage so saturated colours become
deeper instead of merely moving toward grey. This keeps the operation in the
negative/print density model rather than adding an RGB saturation adjustment
after the film pipeline.

This is an explicitly empirical look control, not a reconstruction of measured
interimage chemistry. The public trim is centred at zero on the accepted
standard response, corresponding to the earlier experimental amount 1.5.
Trim -4 is a strict calibrated bypass and +4 applies twice the standard amount.
Within that response, Warm-Tone Separation smoothly reduces chroma compression
along the red-plus-green/low-blue dye-coordinate direction through ordinary
midscale densities. The protection fades near neutral, outside the mid-density
range and at extreme chroma, retaining the outer red roll-off. Zero selects
uniform compression, one is the accepted standard and two is maximum warm
protection. A small direction-preserving radial stage is followed by gentle
guidance of near-warm trajectories toward the yellow/orange axis, preventing
the protected branch from curling toward magenta. This is empirical and does
not classify people or claim a measured interimage mechanism.

### `PrintFilmProcessor`

Exposes Kodak Vision 2383/3383 through the negative transmittance and evaluates
the print characteristic curves. The current printer source is a 3200 K
Planckian approximation.

### `PrintProfileCatalog`

Owns the stable identifiers, display names and resource filenames for supported
print stocks. The pipeline, CLI, Python interface, OFX interface and examples
all consume this metadata rather than maintaining independent labels or paths.

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
Blue-axis slices are evaluated concurrently through the global FilmViz worker
setting while retaining deterministic LUT ordering.

### `GranularityModel`

Loads the digitized diffuse-RMS granularity curves for Kodak Verita 200D
5206/7206 and Kodak Vision 2383/3383, then maps each stage's developed density
to an RGB density standard deviation. Random sampling is deterministic for a
seed and pixel coordinate.

### `FilmFormatCatalog`

Owns the shared active-image widths for Regular 8, Super 8, 16mm, Super 16,
35mm, Super 35 and 65mm. CLI, Python and OpenFX consume these identifiers and
labels from one source. `custom` carries an explicitly supplied physical width.

### `SpatialResponseModel`

Loads the measured negative and print modulation-transfer curves and converts
their cycles/mm axes to pixel frequency using the active image width. The two
stage responses are cascaded into one channel-dependent, DC-preserving FIR.
Amounts of zero bypass a stage; one uses its measured curve. This is a
small-signal system-response approximation applied to the rendered image, not
a claim that the nonlinear negative and print stages are spatially simulated
at microscopic resolution.

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

After colour and density-dependent grain rendering, the optional system MTF
filters the composite result, approximating the spatial response of the
negative/print path delivered by a scan. It is intentionally absent from
deterministic `.cube` output.

Image conversion distributes independent output rows over the same global
worker setting. Image I/O remains serialized, and seeded grain remains
deterministic regardless of worker count.

### `FilmVizThreading`

Owns the process-wide worker count used by both the C++ CLI and Python binding.
Zero selects hardware concurrency; positive values impose an explicit limit.

### `FilmPipeline`

Owns and connects the production components. Its public contract is deliberately
small:

```cpp
FilmPipeline pipeline;
pipeline.initialize(settings);
FilmPipeline::Result result = pipeline.process(ap0_linear);
```

## Fixed production calibration

The current Kodak Vision 2383/3383 spectral dye amplitudes are:

```text
C = 1.10093
M = 1.09650
Y = 1.14626
```

These came from the JIS/D55 calibration JIS/D55 compromise and are profile calibration
values, not user look controls.

The Verita Status-M calibration is data-driven and replaces the earlier
experimental global density/contrast multiplier.

Exposure compensation shifts negative log exposure by stops. Push/pull scales
negative Status-M density around the calibrated middle-gray density by
`2^(0.2 * stops)`. This is explicitly an approximation until measured
alternate-development characteristic curves are available.

Negative flash adds a uniform exposure equal to a percentage of the calibrated
middle-gray record exposure before negative development. Print flash does the
same using the neutral reference printer exposure before print development.
The linked master printer timing adds the same printer-light point offset to all
three records; one point remains 0.025 LogE. These are explicit look controls
and default to zero, leaving the calibrated production baseline unchanged.

Color Density shapes calibrated negative spectral-dye coordinates around their
stock-relative neutral axis. Increasing it progressively calms chroma and adds
chroma-weighted density depth through the print stage. It is separate from
exposure, characteristic-curve contrast, printer timing and display saturation.
Zero is the accepted standard response, -4 restores the calibrated bypass and
+4 applies twice the standard amount.
Warm-Tone Separation controls the directional protection inside Color Density:
zero is uniform compression, one is standard and two maximally protects the
warm mid-density branch. It does not alter the common density-depth term.
Inside that branch it gently guides near-warm hue trajectories toward the
yellow/orange dye-coordinate axis.

## Display output

The production LUT output should normally be `ap0-linear`.

`rec709-gamma24` is provided as a convenient preview path:

```text
viewed AP0 -> AP0/Rec709 matrix -> Gamma 2.4
```

No ACES RRT/ODT is applied. If FilmViz is integrated into an ACES colour
management pipeline, use the AP0 output and apply the intended ACES Output
Transform separately.
