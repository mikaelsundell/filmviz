# FilmViz development tools

The rgb2spec generation scripts rebuild the ACES2065-1 spectral reconstruction
tables using `build/Debug/rgb2spec_opt`:

```bash
./tools/generate_aces2065_1_rgb2spec_64.sh
./tools/generate_aces2065_1_rgb2spec_128.sh
```

They write directly to `resources/spectral/reconstruction/`. The 64-level table
is the current production resource; the 128-level table is retained for
comparison and future validation.
