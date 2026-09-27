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

The Kodak Vision3 50D sensitivity resource was re-derived from the retained
SVG trace after an overlay against Kodak publication H-1-5203 exposed a
compressed vertical digitization. The SVG's 1.0, 2.0 and 3.0 label baselines
define a linear `-34.56 SVG units / log-sensitivity unit`; the enlarged
annotation band above 3.0 is not part of that scale. Sampling the corrected
trace at 5 nm gives yellow/magenta/cyan-forming peaks of 2.677431 at 465 nm,
2.456505 at 545 nm and 2.410890 at 645 nm respectively. The curve supports and
wavelength positions are unchanged.

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

## Empirical colour-density response

Reference comparisons show a useful distinction between moderate colour and
extreme chroma: skin and ordinary object colours can benefit from slightly
greater density separation, while highly saturated blue/cyan trajectories need
a calmer outer response. The available measurements do not identify this as a
specific interimage-effect mechanism and do not provide coefficients for such
chemistry.

FilmViz therefore exposes the experiment as `FilmColorResponse`, an optional
creative layer after Status-M calibration and before negative spectral-density
synthesis. It preserves exact stock neutrals and progressively compresses
dye-coordinate differences as chroma grows. A chroma-weighted reduction of the
common negative-density coordinate increases print exposure, adding viewed
density so strong colours become deeper rather than simply greyer. The
general compression is radial in normalized dye-coordinate space; only the
narrowly gated warm guidance described below changes direction. Neither stage
is a display-space saturation or general hue correction.

Reference review selected the earlier amount 1.5 as the standard creative
response. Color Separation retains the signed -4..+4 mapping: zero maps to
that standard, -4 maps to the strict calibrated bypass, and +4 maps to twice
the standard response. Color Depth independently scales the common-coordinate
term from -1 through 2: one reproduces the accepted response, zero leaves only
the separation shaping and negative values lift chromatic regions. This split
avoids coupling desired outer-chroma compression to unwanted colour darkening.
The standard response reduces compression in a broad red-plus-green/low-blue
dye-coordinate lobe through ordinary midscale densities. Its protection fades
around neutral, toward highlights and shadows, and at extreme chroma so
saturated reds remain controlled; the common density-depth term remains active.
A small directional blend within the same smooth lobe guides near-warm
trajectories toward the yellow/orange dye-coordinate axis. It is deliberately
too narrow and too density-limited to convert true magenta objects into
skin-like hues.

The regression set records the standard baseline, explicit calibrated bypass
and a stronger shaped setting. Focused unit coverage also verifies that zero
depth preserves the common coordinate while separation remains active. These remain empirical
rendering decisions rather than new measured stock calibration or a claim that
the warm lobe reconstructs interimage chemistry.

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

## Experimental Color Response tuning

The Python app exposes a tuning panel for the existing empirical dye-coordinate
look layer. This adds no measured film-chemistry claim and changes no calibration
anchors. Defaults retain the accepted response: compression 0.22, chroma knee
0.50, color depth 1, density center 1.25/width 1, warm protection 0.50, warm hue
center 0 degrees/width 1 and additional hue shift 0 degrees.

Response amount (0..1, default 1) blends the complete shaped result, including
hue guidance, with calibrated coordinates; zero is an exact bypass. The legacy
Color Separation trim remains supported by C++/CLI/OFX, while the Python panel
starts it at its standard zero setting and exposes compression directly.
Density center and width remap normalized negative density before the existing
smooth general/warm envelopes. Hue center rotates the selected direction;
hue width scales angular distance. Signed hue shift adds a gated rotation in
the plane perpendicular to the neutral axis, positive from yellow toward red.
Rotation preserves chroma magnitude and common density before the existing
minimum-density clamp. Near-neutral and extreme-chroma warm gating remains.

The C++ model supplies the diagnostic plot. Metal and generated OpenCL mirror
the same equations and parameter ranges. Regression source assertions cover
full bypass, blending, neutral preservation and local rotation invariants;
the expanded tuning has not yet been built, tested or visually accepted.

## Experimental correlated grain texture (superseded by the revision below)

Reference review identified broad colored patches in the old independently
colored smoothstep lattice noise, particularly at enlarged grain scales. The
image-domain synthesizer now combines independent fine/coarse fields at 84/16
percent variance, with linear rather than smoothstep interpolation. Each band
is normalized by its interpolation-weight energy. RGB fields share 81 percent
of their variance, retaining a smaller independent component. Negative and print
stages still use independent seeds and the existing measured density-to-sigma
curves; neither the spectral pipeline nor color response is modified.

These texture weights and covariance are empirical rendering choices, not
newly measured stock properties. Per-channel variance is retained for resolved
sizes; luminance variance can change with channel correlation. Grain Chroma
still scales the channel differences while preserving weighted density-noise
luminance for a given sample. This does not guarantee constant image luminance
after exponentiation and clipping.

The user grain-size range starts at a 0.25 multiplier. Effective pixel size is
`scale * image_width_pixels / 2048 * 24.89 / active_image_width_mm`.
The 2048px / Super 35 anchor is an empirical rendering convention, not measured
crystal size. Both grain stages share this format mapping; it does not model a
separate print aperture. Measured sigma and strength defaults remain unchanged.
Below one pixel, RMS scales with size as approximate unresolved pixel-area
averaging. Python Metal preview uses its working width in the same mapping,
without an additional preview multiplier. This approximates reduced appearance
but is not pixel-identical to downsampling a full-resolution render.
Existing output-domain application and MTF ordering remain unchanged for this
first texture revision; they need separate assessment with matched image crops.
Regression assertions were added for determinism, channel covariance, marginal
variance and subpixel attenuation. This revision has not been built or tested.


## Aperture-integrated grain and downstream density response

The 1920px Super 35/Super 8 llama exports and eight supplied film-frame JPEGs
were reviewed at native sampling. Four sky patches per image gave mean local
high-pass RMS values of 1.62/1.52 equivalent 8-bit code values for FilmViz
Super 35/Super 8, compared with 2.30 for the river reference and 1.63 for the
bright-sky reference. Adjacent-pixel correlations were approximately 0.01/0.31
versus 0.34/0.43. These are delivered-image texture observations, contaminated
by scene structure and JPEG processing, not isolated stock measurements.

Code inspection identified the dominant 84-percent band being held at a
one-pixel minimum: at 1920px and scale 1 it changed only from 1.00px to 1.21px
between Super 35 and Super 8. All grain also passed through the combined MTF,
and negative noise did not travel through the downstream print response.
The user authorized the following empirical rendering revision on that basis.

The continuous random field now uses linear triangular basis functions at
0.85 and 1.80 times the size parameter. Each pixel integrates the basis over
its square area, with centers at x+0.5/y+0.5. There is no one-pixel size floor.
When a pixel spans more than eight lattice cells, an uncorrelated area-average
limit bounds computational cost; that thumbnail regime does not retain exact
spatial correspondence. Otherwise aligned area downsampling commutes with the
field integration. RGB covariance remains 0.81. Negative/print seeds remain
independent. No stock curves, calibration anchors or default strength values
are changed.

A 48um circular reference aperture is approximated by an equal-area square
(side 0.048*sqrt(pi)/2 mm). The exact phase-averaged squared integrated-basis
weight sum provides normalization for that square. Band weights are 75/25
percent variance at the reference aperture, not at individual pixels. The
48um convention follows Kodak's published diffuse-RMS measurement practice
(e.g. https://www.kodak.com/content/pdfs/KODAK-VISION3-5219-7219-technical-information.pdf).
It is an explicit common rendering assumption pending a profile-by-profile
aperture audit; the square replacement, spatial spectrum and shared negative/
print format mapping are not independently measured calibration. Strength 1
must not be described as a validated scan match. No scanner sharpening, print
aperture, temporal grain persistence or microscopic emulsion model is inferred.

A new density boundary adds negative noise in measured Status-M coordinates
before the existing nonlinear calibration. Print noise is added after print
development, before spectral synthesis/viewing. CPU LUTs cache two 3x3 local
log-linear-output Jacobians, multiplied by the measured sigmas, using +/-0.002D
central differences. GPU direct paths evaluate the baseline, negative-only and
print-only perturbations. This avoids twelve extra GPU spectral solves per
pixel but differs from CPU linearization for larger perturbations. Both paths
combine independent stage residuals and omit their nonlinear interaction.
Log-relative changes are limited to +/-2 to bound extreme black/gamut-boundary
extrapolation. This is a numerical guard, not measured film behavior.

Only the baseline receives the combined negative/print MTF. Negative residuals
receive print MTF only; print residuals are added afterward. The output-domain
filtering of those propagated residuals remains a small-signal approximation,
not a full spatial print exposure solve. Signed residuals are never clamped
before compositing. Grain chroma acts on relative RGB residuals after filtering;
zero preserves working-RGB color ratios until clipping/output conversion.
The original grain-free spectral path and cube generation remain unchanged.

OFX prebaked cache version 13 stores the new response matrices. Existing caches
are rejected and regenerated. `test_granularity` records aperture-energy,
area-downsample, covariance and determinism invariants; `test_grain_response`
records zero perturbation, downstream print propagation, neutral compositing
and signed-filter behavior. These sources have been added but not built or run,
at the user's request. Visual acceptance and CPU/GPU comparisons remain pending.


## Density-dependent grain diagnostic follow-up

The matched 4448x3096 Helen/John exports isolate the added grain with MTF and
halation off. Across five neutral patches, encoded-luma relative noise rises
from 2.30 percent on white to 9.81 percent on shadow gray. Absolute standard
deviation is 4.52, 5.30, 6.21, 6.36 and 5.44 equivalent 8-bit code values.
Neighbor correlation remains 0.727..0.732; the variance fraction retained by
2x2 averaging remains 0.746..0.753. These observations distinguish rising
relative amplitude from a change in spatial structure. They do not establish
physical crystal size or a density-dependent spectrum for either stock.

The measurements, coordinates and reproduction utility are retained in
`working/analysis/grain_density/`. A new API example,
`example_grain_density_diagnostic`, reports negative-only, print-only and
combined noise through a neutral exposure ramp, including each film density,
measured sigma, color covariance proxy, linear mean shift and spatial metrics.
The example is source-only pending the user's build; no stage-isolation
results are claimed yet. This follow-up changes no production grain settings,
curves or rendering behavior.

## CPU grain mean normalization

The user-built `build.debug/grain_density_super35.csv` isolates the two stages.
At zero stops their encoded-luma standard deviations are 6.04 (negative),
3.02 (print), and 6.77 (combined), in equivalent 8-bit codes. Print RGB standard
deviations are 1.89 / 2.82 / 10.22; combined blue mean rises by 0.72 codes.
Within each stage, neighbor correlation changes little across the ramp.
The print CSV declares R/G/B columns, the loader uses that order, and the
retained source SVG labels the upper granularity trace B. No channel swap is
established. This is a mapping audit, not an independent redigitization or an
audit of the measurement-to-print-dye coordinate interpretation. Measured
values remain unchanged.

CPU rendering previously used exp(clamp(log_gain,-2,2))-1, whose expectation
is positive for nonzero symmetric noise. The multiplier now divides by its
Gaussian ensemble expectation, including the bounded tails. Variance uses
the actual pixel integration weights at each lattice phase, the 75/25 band
mixture, and the record covariance (0.81 shared / 0.19 independent). This is
a rendering mean convention, not a new physical stock hypothesis. Neither
measured sigmas nor the density response matrices change.

The correction covers Python/CLI and CPU LUT consumers. Metal/OpenCL direct
spectral perturbation paths do not use this log-Jacobian approximation and
remain unchanged; their nonlinear mean response needs a separate diagnostic.
Linear ensemble mean preservation does not imply gamma-encoded mean
preservation, exact zero mean in a finite correlated patch, or preservation
after output clipping. Combined independent stage residuals can still exceed
the output range. Density-dependent texture redistribution is deferred until
the corrected Python output is reviewed. Regression source covers independent
seed means at ordinary and bounded gains; no builds or tests were run.

## Separate negative and print texture shapes

The user's 32-seed neutral-ramp diagnostic places all combined pre-clipping
RGB means within approximate pointwise 95% intervals around zero. Two
negative-only blue results are marginal (about 2.1 standard errors); shared
seeds and multiple comparisons limit their interpretation. This supports
retaining the CPU mean normalization for this configuration, not a claim of
universal validation.

Flat scan appearance references show differing spatial spectra: the supplied
2383 sample has lower neighbor correlation and more fine-frequency energy
than the supplied 250D sample. Their processing and physical scale are unknown.
They are used with production frames to guide an explicitly empirical stage
distinction, not to redigitize stock data or infer crystal sizes.

Negative texture retains lattice scales 0.85/1.80 times grain size with 75/25
reference-aperture variance. Print now uses 0.50/1.10 with 90/10 variance.
These are provisional rendering choices, shared across stock selections within
each stage. Each band is independently aperture-normalized. Measured sigmas,
format mapping, RGB covariance, and chroma control are unchanged. Individual
pixel variance can change when the spatial spectrum changes; strength one is
still a reference-aperture convention, not fixed pixel contrast.

CPU mean normalization uses the appropriate stage's pixel-phase variance.
Metal and generated OpenCL use the same stage shapes and aperture dimensions;
their existing direct spectral perturbation behavior remains otherwise intact.
Regression source extends aperture normalization, area averaging and CPU mean
checks to both stages. No builds or tests were run. Visual assessment and
CPU/GPU agreement remain pending. No density-dependent size law is introduced.

## Empirical grain visibility comparison

User review of the matched-scene 250D appearance reference requests gentler
grain, particularly in bright regions. The next comparison applies an explicit
output look trim after signed residual filtering and chroma mixing. Residual
amplitude is 0.80 in low working-RGB values, fading with smoothstep between
maximum linear RGB 0.12 and 0.65 to 0.35 in highlights. The thresholds and
amounts are provisional artistic choices, not recovered film measurements.
The maximum-channel convention is working-space dependent and also protects
bright saturated colors; it is not a physical luminance or density estimate.

The trim depends only on the grain-free baseline and is common to all RGB
residual channels. It therefore preserves zero-mean residuals before clipping
without face/edge detection. Measured RMS curves, texture spectra, format
mapping, and noise generation remain unchanged. Reference-aperture
normalization still applies to generated density noise; final displayed grain
is now deliberately attenuated by this look trim. Python/CLI CPU and Metal /
generated OpenCL composites share the rule. The ramp CSV includes
`grain_visibility`. Source checks were added, but no builds or tests were run.

The Python application now exposes that empirical trim with an explicit
bypass and shadow/midtone/highlight multipliers (0..2, default 1). Default
settings preserve the accepted trim. Shadow weight fades smoothly from one
to zero over maximum linear RGB 0..0.12; highlight weight rises over
0.12..0.65; midtone weight completes their sum to one. Bypass returns unity
visibility regardless of the multipliers. These output-space weights are
not measurements of density or stock-specific exposure regions. Measured
resources, spectral calibration, and grain-free processing are unchanged.
CPU conversion and GPU preview share the controls. New regression assertions
cover bypass and zero gains; source only, not built or run.

## Halation source weighting comparison

User review at strength 1, radius 74px and threshold zero found excessive
dominance of the brightest chart patch. Source inspection identified an extra
scene-luminance factor multiplying negative exposure, making neutral source
intensity approximately quadratic. The comparison now uses negative exposure
times a dimensionless smooth threshold mask only. Threshold zero gives a unit
mask; positive thresholds retain smoothstep from half-threshold to threshold.
CPU and Metal (also used to generate OpenCL) share this change.

Scatter coefficients, near/far blur, source suppression and controls remain
unchanged. This is an empirical halation adjustment, not new measured stock
data. It increases relative participation of lower-exposure regions and reduces
the extra weighting formerly given to scene luminance above one. It does not
guarantee a stronger halo everywhere. Visual comparison and strength retuning
remain pending; no builds or tests were run.
