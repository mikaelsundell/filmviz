# ACES Academy Printing Density scanner responses

`aces_apd_scanner_responsivities.csv` contains the calibrated red, green and
blue scanner responsivities associated with ACES Academy Printing Density
(APD), SMPTE ST 2065-2.

This is densitometry data. It describes three spectral measurement responses
for deriving APD values from film transmittance or spectral optical density. It
is not an SPD-to-ACES2065-1/AP0 colour conversion, and the curves are not AP0
colour-matching functions.

## Data layout

```text
wavelength_nm,red,green,blue
```

- 181 samples from 368 through 728 nm, inclusive;
- uniform 2 nm spacing;
- peak wavelengths: red 692 nm, green 550 nm, blue 466 nm;
- calibrated source amplitudes, intentionally not channel-normalized.

The source grid is retained exactly. A future APD measurement implementation
should interpolate these responsivities onto the spectrum being measured rather
than resampling this canonical file in advance.

## Provenance

The resource supplied to FilmViz was exported from `colorSpec::scanner.ACES`.
The colorSpec metadata names `SMPTE-ST-2065-2.txt` as its upstream source. Its
181 data rows exactly match the `BEGIN_SCANNER_ACES` block retained in
`working/rgb2spec/colorSpec.txt`.

Imported CSV SHA-256:

```text
812afaeb043d44468de2231dfb9080e3c8c25283770db7214e9f0079bd65c499
```

The package provenance statement has not been independently certified against
the published SMPTE standard.

## Relationship to FilmViz

FilmViz currently uses ISO Status-M to interpret Kodak Verita characteristic
curves. APD is a separate measurement system and this resource is not loaded by
the production `FilmPipeline`. Adding APD support in the future must not replace
or silently alter `StatusMDensitometer` or `FilmDensityCalibration`.
