# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 - present Mikael Sundell.
"""Analyze the supplied 4448x3096 Helen/John pair; requires numpy and Pillow.

This is an image-analysis utility, not a renderer or regression test.
Regions are explicit coordinates for this frame, not automatically detected.
"""
import argparse
import csv
from pathlib import Path
import zlib

import numpy as np
from PIL import Image

REGIONS = [
    ("White patch", 168, 1882), ("Light gray", 354, 1882),
    ("Mid gray", 520, 1882), ("Dark gray", 690, 1882),
    ("Shadow gray", 880, 1882), ("Gray card", 671, 2203),
    ("Forehead", 1767, 659),
]


def read_export(path):
    """Retain 16-bit values; Pillow's RGB conversion otherwise reduces precision."""
    with Image.open(path) as image:
        tags = dict(image.tag_v2)
        width, height = image.size
    if ((width, height) != (4448, 3096)
            or tuple(tags.get(258, ())) != (16, 16, 16)
            or tags.get(277) != 3 or tags.get(284, 1) != 1
            or tags.get(274, 1) != 1 or tags.get(262) != 2
            or tags.get(259) not in (1, 8, 32946)
            or tags.get(317, 1) not in (1, 2)
            or 273 not in tags or 279 not in tags):
        raise ValueError("Expected original 4448x3096 RGB16 strip-based FilmViz TIFF exports")
    raw = path.read_bytes()
    dtype = "<u2" if raw[:2] == b"II" else ">u2"
    strips = []
    for offset, count in zip(tags[273], tags[279]):
        payload = raw[offset:offset+count]
        if tags[259] != 1:
            payload = zlib.decompress(payload)
        values = np.frombuffer(payload, dtype=dtype).reshape(-1, width, 3)
        if tags.get(317, 1) == 2:
            values = np.cumsum(values.astype(np.uint32), axis=1).astype(np.uint16)
        strips.append(values)
    return np.concatenate(strips)[:height].astype(np.float64) / 65535.0


def analyze(base, grain):
    weights = np.array([0.2126, 0.7152, 0.0722])
    size = 128
    frequency = np.fft.fftfreq(size)
    radius = np.sqrt(frequency[:, None]**2 + frequency[None, :]**2)
    window = np.hanning(size)[:, None] * np.hanning(size)[None, :]
    for name, x, y in REGIONS:
        clean = base[y:y+size, x:x+size]
        difference = grain[y:y+size, x:x+size] - clean
        luminance = difference @ weights
        mean = float((clean @ weights).mean())
        deviation = float(luminance.std())
        blocks = luminance.reshape(64, 2, 64, 2).mean((1, 3))
        power = abs(np.fft.fft2((luminance-luminance.mean()) * window))**2
        total = power.sum()
        yield dict(
            region=name, x=x, y=y, size=size,
            mean_code255=mean*255,
            grain_sd_code255=deviation*255,
            grain_relative_percent=100*deviation/mean if mean else 0.0,
            lag1=float(np.corrcoef(luminance[:, :-1].ravel(), luminance[:, 1:].ravel())[0, 1]) if deviation else 0.0,
            block2_variance_fraction=float(blocks.var()/luminance.var()) if deviation else 0.0,
            low_frequency_fraction=float(power[radius < .125].sum()/total) if total else 0.0,
            mid_frequency_fraction=float(power[(radius >= .125) & (radius < .25)].sum()/total) if total else 0.0,
            high_frequency_fraction=float(power[radius >= .25].sum()/total) if total else 0.0,
            blue_yellow_sd_code255=float(np.std(difference[:, :, 2]-.5*(difference[:, :, 0]+difference[:, :, 1]))*255),
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("no_grain", type=Path)
    parser.add_argument("grain", type=Path)
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    rows = list(analyze(read_export(args.no_grain), read_export(args.grain)))
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
