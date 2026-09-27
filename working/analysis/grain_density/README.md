# Density-dependent grain diagnostic

## Observed pair

Inputs: `build/filmviz_output_no_grain.tif` and
`build/filmviz_output_grain.tif`, 4448x3096 RGB16, saved September 26 at
20:31:58 and 20:40:22. The corresponding run log changes only negative/print
grain from 0/0 to 1/1. Super 35, scale/chroma 1, seed 1, LUT 65;
MTF and halation are disabled. The original files are not modified.

`paired_regions.csv` records measurements of grain minus no-grain in explicit
128px square regions. The chart coordinates and forehead region are included
for reproduction. Rec.709 weights are applied to encoded values: these are
code-value contrast measurements, not linear-light luminance or film density.

| Patch | Mean code (0..255) | Grain standard deviation | Relative variation | Neighbor correlation | Variance retained after 2x2 averaging |
|---|---:|---:|---:|---:|---:|
| White | 195.97 | 4.52 | 2.30% | 0.729 | 0.748 |
| Light gray | 174.05 | 5.30 | 3.04% | 0.727 | 0.746 |
| Mid gray | 141.71 | 6.21 | 4.38% | 0.732 | 0.751 |
| Dark gray | 98.71 | 6.36 | 6.45% | 0.727 | 0.748 |
| Shadow gray | 55.44 | 5.44 | 9.81% | 0.730 | 0.753 |
| Forehead | 144.16 | 6.70 | 4.65% | 0.735 | 0.755 |

Absolute grain variation peaks around darker midtones in these samples and
falls again toward shadows. Relative variation keeps increasing toward darker
patches. Texture correlation and the 2x2 averaging ratio barely change: the
model varies amplitude much more than structure. Neither neighboring-pixel
correlation nor the box-average ratio establishes physical crystal size.

The forehead and mid-gray patch are similar in brightness. Their luminance
noise is also fairly similar; blue-minus-yellow variation is larger on the
forehead (12.75 vs 10.72 code-value standard deviation). This argues for
examining propagated color covariance alongside any change of grain coarseness.

The CSV also contains Hann-windowed spectral energy fractions below 0.125,
from 0.125 to 0.25, and above 0.25 cycles/native pixel. These describe the output
noise spectrum, not the generator's two independent fine/coarse bands. Their
small patch-to-patch differences are not a reliable monotonic density trend.

The paired TIFFs cannot identify which independent stage contributed the
observed residual. No negative-only or print-only result is inferred from them.
No spatial model or measured curve is changed by this diagnostic work.

## Reproduce the pair analysis

Requires numpy and Pillow. From the project root:

```sh
python3 working/analysis/grain_density/analyze_pair.py \
    build/filmviz_output_no_grain.tif build/filmviz_output_grain.tif \
    working/analysis/grain_density/paired_regions.csv
```

The reader is intentionally restricted to this original frame and FilmViz's
strip-based RGB16 TIFF format; it does not silently reinterpret resized images.

## Isolate the film stages

`examples/example_grain_density_diagnostic.cpp` is registered with the existing
`FILMVIZ_BUILD_EXAMPLES` option. It has been written but not built or run.
After the user's build, from the project root:

```sh
./build/bin/example_grain_density_diagnostic resources super-35 4448 1 \
    > build/grain_density_super35.csv
```

Optional fifth/sixth arguments select negative and print profiles. A `none`
print produces zero print residual. The diagnostic processes a neutral AP0
ramp from -6 through +6 stops. For each input it reports negative-only,
print-only and combined results, using the same seed and texture realization.
It uses the production CPU density-response approximation and compositing,
with strength/chroma 1 and no MTF/halation. It does not call the application or
write rendered images. The width controls film-to-pixel mapping; each sampled
uniform patch is 128x128 pixels.

CSV fields include both developed RGB densities, both measured sigma triplets,
RGB noise means and standard deviations, linear-light mean shifts, relative
encoded-luma variation, neighbor correlation, a 2x2 averaging ratio,
blue/yellow variation and the fraction of channels clipped. Compare actual
negative/print density columns, not just final display brightness. Strong
clipping can make noise appear quieter or change its apparent structure.
The diagnostic includes nonlinear output conversion, so isolated stage
statistics need not add numerically to the combined row.

Next decision: identify the dominant stage and color contribution before
choosing an empirical density-dependent coarse-band weight. Preserve the
reference-aperture variance if redistributing power between bands; account for
mean shift separately. The delivered film JPEGs remain appearance references,
not measurements of crystals or a quantitative calibration target.

## Multi-seed mean diagnostic

The example now defaults to 32 seeds (1 through 32), reusing that set across
all stops and stage combinations. The existing command still works. A final
optional positional argument after the negative and print identifiers sets
the seed count (2..256). No production rendering changes are made here.

Existing statistic columns now contain averages of per-seed patch statistics.
In particular, SD and correlation are averages within patches, not statistics
of concatenated images. New `preclip_linear_mean_*` fields measure the signed
linear residual before output clamping. Existing `linear_mean_*` fields retain
their after-clipping meaning. This distinguishes the normalization convention
from clipping and nonlinear gamma encoding.

The `*_se` columns report standard errors of mean shifts, using the sample
variance of the independent seed means divided by the seed count. Correlated
pixels are not treated as independent observations. At the default 32 seeds,
mean +/- 2.04 times SE gives an approximate pointwise 95% Student-t interval.
These are not simultaneous confidence intervals across all channels/stops;
the same seeds across rows intentionally make those rows dependent. A zero
SE for a disabled response indicates identical samples, not independent proof
of exactness. Additional seeds may be necessary for marginal results.

For the user's debug build:

```sh
./build.debug/bin/example_grain_density_diagnostic resources super-35 4448 1 \
    > build.debug/grain_density_super35_multiseed.csv
```

The extended source has not been built or run by the assistant.

## Retained scan appearance references

`Reference_50D.png`, `Reference_250D.png`, and `Reference_2383.png` under
`resources/references/images/` are likely photographed midgray, according to
the user. Processing history, scan scale, and neutrality adjustments are
unknown. Use them alongside real production frames, not as absolute stock
calibration or evidence for density-dependent grain size.

Nine detrended 512px patches in each original 3659x2051 PNG gave encoded-luma
SD / horizontal lag-one correlation / energy above 0.25 cycles per pixel:
50D 5.63 / 0.55 / 37%; 250D 6.28 / 0.66 / 22%; 2383 7.38 / 0.46 / 45%.
Full-image RGB correlations span 0.985..0.997. These describe the supplied
files, including any scan/postprocessing, not intrinsic emulsion covariance.
