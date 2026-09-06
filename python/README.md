# FilmViz Python application

The Python application uses PySide6 for its interface and the `filmviz_python`
pybind11 module for direct access to the same C++ spectral pipeline as the
`filmviz` command-line tool. Both LUT sampling and image rows use FilmViz's
process-wide worker-thread setting.

Build the project normally with `FILMVIZ_BUILD_PYTHON_APP=ON` (the default).
The binding is built when pybind11 and Python development files are available.

```bash
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH=/Volumes/Projects/github/3rdparty/build/macosx/arm64.debug
cmake --build build -j
```

For a release build, configure with the release dependency tree instead:

```bash
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH=/Volumes/Projects/github/3rdparty/build/macosx/arm64.release
cmake --build build --config Release -j
```

Use exactly one dependency prefix per build directory. Reconfigure or use a
separate build directory when switching between debug and release dependencies.

Build the application target and run its generated launcher:

```bash
cmake --build build --config Debug --target python_filmviz_app
./build/Debug/python_filmviz_app.sh
```

For Release, use `--config Release` and run
`./build/Release/python_filmviz_app.sh`. The launcher embeds the Python
interpreter, dependency prefix, extension-module directory and project root
selected by CMake. It also applies the matching macOS Qt framework suffix, so
no shell environment setup is required.

Running `python3 python/filmviz_app.py` directly remains supported when that
Python environment can already import both PySide6 and `filmviz_python`.

The image interface exposes negative and print flash, linked master printer
timing, Color Density, density-dependent negative/print grain, and measured
negative/print MTF. Color Density is a neutral-preserving transform in
calibrated negative dye-coordinate space that progressively calms chroma and
adds chroma-weighted depth through print exposure. Zero is the accepted
standard response, -4 restores calibrated bypass, and +4 applies twice the
standard response. Film-format presets supply the physical active-image width
used to convert the MTF curves from cycles/mm to pixels; Custom enables direct
width entry. These controls call the shared C++ models rather than duplicating
film logic in Python.

If PySide6 is installed in the same non-system dependency prefix, the app reads
`build/CMakeCache.txt` and adds that prefix's Python site-packages directory.
You can also set `FILMVIZ_DEPENDENCY_PREFIX` explicitly.

On macOS the selected dependency prefix also controls Qt framework selection.
For a prefix ending in `.debug`, the app re-launches with the CMake-selected
Python interpreter and `DYLD_IMAGE_SUFFIX=_debug`; a `.release` prefix runs
without that suffix. This prevents debug and release Qt frameworks from being
loaded into the same process.

For an automated startup check that constructs the complete window without
entering the event loop, set `FILMVIZ_APP_SMOKE_TEST=1`.
