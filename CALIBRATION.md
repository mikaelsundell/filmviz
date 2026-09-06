# FilmViz calibration record

This file records the measurements that justified the current production
architecture. It is intentionally concise and should be updated when a new
calibration supersedes one of these conclusions.

## Source-data checks

The source-data validation established the current Verita digitization:

- sensitivity mapping: yellow-forming -> B, magenta-forming -> G,
  cyan-forming -> R;
- sensitivity supports/peaks approximately B 375..510 / 470 nm,
  G 395..600 / 550 nm, R 500..675 / 650 nm;
- sensitometric mapping: `curve_low` -> R, `curve_mid` -> G,
  `curve_high` -> B;
- stop/logE axis: `logE = -0.515 + 0.3 * stops` in the digitized Kodak graph;
- FilmProcessor reproduces the raw 0-stop density row to about 1e-8;
- 10 nm spectral D-min/midscale source -> 5 nm working interpolation error is
  about 3.5e-7 RMS.

These checks are why the Status-M/spectral mismatch is treated as a modelling
coordinate issue rather than a CSV/parser issue.

## Kodak spectral vs sensitometric coordinates

Measured Verita spectral midscale, remeasured with Status M:

```text
R/G/B = 0.727126 / 1.13036 / 1.38487
```

Verita characteristic-curve density at the old `logE = -0.515` anchor:

```text
R/G/B = 1.17967 / 1.56155 / 1.93969
```

Inverting the characteristic curves puts the spectral midscale at approximately:

```text
R/G/B logE = -1.43164 / -1.39982 / -1.43857
```

The three channels clustering near -1.42 was the key evidence that published
Status-M density and FilmDyeModel spectral-basis coordinates must be separated.

## Local Status-M closure Jacobian

At zero stop, the measured response of FilmDyeModel coordinates is roughly:

```text
J = d(StatusM measured) / d(FilmDyeModel coordinate)

[ 0.469495   0.006097   0        ]
[ 0.009798   0.463391   0.002162 ]
[ 0          0.065988   0.381911 ]
```

The diagonal being only about 0.38..0.47 explains why the earlier direct
record-density -> spectral-basis mapping produced a much flatter image.

## Nonlinear closure result

The nonlinear closure analysis solved the Status-M -> spectral coordinate
relationship at every neutral exposure:

```text
current Status-M closure RMS vs raw target     = 0.362475
nonlinear closure RMS vs achievable target     = 0.000723601
nonlinear closure max vs achievable target     = 0.00916583
```

The remaining raw-target error is almost entirely the measured spectral D-min
boundary:

```text
nonlinear RMS vs raw target = 0.224858
D-min projection RMS        = 0.224852
```

The resulting viewed-print midrange gamma changed from:

```text
old direct mapping       0.754725
Status-M calibrated      1.50935
```

This approximately 1.5 response emerged from densitometric closure; it was not
chosen as a visual contrast setting.

## Chromatic closure

The chromatic closure analysis tested isolated and mixed R/G/B Status-M
perturbations around several neutral exposure anchors:

```text
cases                 52
solved                49
closure RMS           0.00229451
closure max           0.0191538
```

All 39 cases from 0 through +4 stops closed essentially at numerical precision.
The three unsolved cases were around -2 stops and involved the lower blue-side
spectral feasibility boundary.

The local Jacobian retains small but systematic cross-channel coupling, with
mean/worst off-diagonal-to-diagonal ratios around 0.067 / 0.076. This is why
production currently keeps the nonlinear coupled solve rather than replacing it
with three independent affine curves.

## Hue trajectory note

Visual inspection of the stronger calibrated response showed a mixed
mid/high-tone phase that can read slightly magenta before the upper shoulder
becomes warmer. Stage tracing confirmed that this chromatic behaviour
exists upstream of the final AP0 -> Rec.709 preview and is not caused by an
ACES/display transform.

This observation is **not a calibration target**. Do not add warm-shifting or
magenta suppression based only on the visual expectation that film shoulders
are usually warm. Any future hue-model change must be supported by stock data
or a clearly separated creative-look layer.

## Scene-exposure spectral reconstruction

Bright saturated AP0 reds exposed a bounded-reflectance failure in the direct
rgb2spec path. A problematic pixel with input AP0 approximately
`0.838 / 0.252 / 0.116` reconstructed to unity at both 400..420 nm and
600..680 nm, with an almost-zero mid-spectrum. Its negative blue/green
exposure ratio was about 4.06 and the viewed result turned magenta. A nearby
healthy red at AP0 approximately `0.275 / 0.084 / 0.048` reconstructed as a
normal rising red edge and had a blue/green exposure ratio around 1.35.

Production therefore separates scene exposure from spectral shape above
AP0/D60 Y=0.18. The lower-exposure AP0 value is passed to rgb2spec and its
reconstructed spectrum is scaled back by the same exposure factor. This
preserves AP0 chromatic ratios while allowing scene spectral power above one.
Image probes confirmed that the red/violet metamer branch collapses into the
healthy red branch, while bright skin samples retain smooth rising spectra and
balanced film-layer exposures. This is reconstruction semantics, not a
red-specific hue correction.

## Reference profile

An independently derived Verita reference profile produced a
similar contrast range (roughly 1.44..1.60 mid-gamma depending on the branch)
and helped identify the missing coordinate concept. The production algorithm
does not depend on or copy that profile; the current method is derived from the
Kodak data and ISO Status-M closure described above.

## Grain and push/pull scope

The Verita and Kodak Vision 2383/3383 diffuse-RMS curves provide density-domain
deviations for the negative and print stages. FilmViz samples the two stages
independently and propagates them as a first-order image-domain approximation.
This preserves their distinct density dependence and opposite effects on final
transmittance without claiming a per-pixel microscopic spectral simulation.

The measured curves define grain amplitude. Spatial grain size is a rendering
control because the source data does not define scan resolution, enlargement,
or scanner MTF.

Grain chroma is likewise a rendering control, not measured stock data. It
scales channel differences around a fixed Rec.709-weighted noise component, so
reducing colour speckling does not also reduce luminance-grain strength. A value
of one is the unmodified per-channel model; zero is neutral grain.

No measured Verita push/pull curve family is currently present. The CLI control
therefore scales negative Status-M density around the calibrated middle-gray
anchor by `2^(0.2 * stops)`. Treat this as an explicit processing approximation,
not a newly calibrated stock property.

## Flash, printer timing and measured spatial response

Negative flash is modeled as uniform per-record exposure before the measured
negative characteristic curves, expressed as a percentage of the calibrated
middle-gray exposure. Print flash is uniform exposure before the print curves,
expressed against the neutral reference printer exposure. Both default to zero.
The master printer-light control is a linked offset to the three existing
printer-light settings, retaining the calibrated 0.025 LogE per point mapping.

The Kodak negative and 2383 modulation-transfer measurements are expressed in
cycles/mm. `SpatialResponseModel` maps that physical axis to pixels using the
selected active image width and cascades the negative and print responses into
a DC-preserving channel-dependent filter. This is a measured small-signal
system MTF applied to the rendered image. It does not invent interlayer
chemistry, density-dependent dye shapes, scanner response or a microscopic
spatial simulation. A strength of one selects the measured curve; zero is a
strict bypass. Film-format dimensions are mapping metadata rather than new
stock calibration values, and Custom is available where the actual aperture or
crop differs from a preset.
