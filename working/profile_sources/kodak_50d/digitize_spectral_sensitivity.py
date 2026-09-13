#!/usr/bin/env python3
"""Regenerate the Kodak 50D spectral-sensitivity CSV from its traced SVG."""

from __future__ import annotations

import argparse
import bisect
import csv
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


SVG_NAMESPACE = {"svg": "http://www.w3.org/2000/svg"}
CHANNEL_CLASSES = {
    "yellow_forming_layer": "cls-4",
    "magenta_forming_layer": "cls-3",
    "cyan_forming_layer": "cls-1",
}
X_LEFT = 13.95
X_RIGHT = 200.34
WAVELENGTH_MIN = 250.0
WAVELENGTH_MAX = 750.0

# Least-squares fit through the SVG label baselines for 1.0, 2.0 and 3.0.
# Kodak's enlarged annotation band above 3.0 makes the outer plot rectangle an
# invalid vertical calibration reference.
Y_INTERCEPT = 171.5866666666667
Y_PER_LOG_UNIT = -34.56


def path_points(data: str) -> list[tuple[float, float]]:
    tokens = re.findall(r"[A-Za-z]|[-+]?(?:\d*\.\d+|\d+)", data)
    points: list[tuple[float, float]] = []
    point = (0.0, 0.0)
    previous_control: tuple[float, float] | None = None
    command = ""
    index = 0

    while index < len(tokens):
        if tokens[index].isalpha():
            command = tokens[index]
            index += 1

        if command == "M":
            point = (float(tokens[index]), float(tokens[index + 1]))
            index += 2
            points.append(point)
            previous_control = None
            command = "L"
        elif command in ("L", "l"):
            x = float(tokens[index])
            y = float(tokens[index + 1])
            index += 2
            point = (x, y) if command == "L" else (point[0] + x, point[1] + y)
            points.append(point)
            previous_control = None
        elif command == "s":
            control_1 = (
                (2.0 * point[0] - previous_control[0], 2.0 * point[1] - previous_control[1])
                if previous_control
                else point
            )
            control_2 = (
                point[0] + float(tokens[index]),
                point[1] + float(tokens[index + 1]),
            )
            end = (
                point[0] + float(tokens[index + 2]),
                point[1] + float(tokens[index + 3]),
            )
            index += 4
            for step in range(1, 201):
                t = step / 200.0
                u = 1.0 - t
                points.append(
                    (
                        u**3 * point[0]
                        + 3.0 * u * u * t * control_1[0]
                        + 3.0 * u * t * t * control_2[0]
                        + t**3 * end[0],
                        u**3 * point[1]
                        + 3.0 * u * u * t * control_1[1]
                        + 3.0 * u * t * t * control_2[1]
                        + t**3 * end[1],
                    )
                )
            point = end
            previous_control = control_2
        else:
            raise ValueError(f"Unsupported SVG path command: {command}")

    return points


def wavelength_from_x(x: float) -> float:
    return WAVELENGTH_MIN + (x - X_LEFT) / (X_RIGHT - X_LEFT) * (
        WAVELENGTH_MAX - WAVELENGTH_MIN
    )


def sensitivity_from_y(y: float) -> float:
    return (y - Y_INTERCEPT) / Y_PER_LOG_UNIT


def sample_curve(points: list[tuple[float, float]], wavelength: float) -> float | None:
    samples = sorted((wavelength_from_x(x), sensitivity_from_y(y)) for x, y in points)
    wavelengths = [sample[0] for sample in samples]
    if wavelength < wavelengths[0] or wavelength > wavelengths[-1]:
        return None
    upper = bisect.bisect_left(wavelengths, wavelength)
    if upper == 0:
        return samples[0][1]
    low_wavelength, low_value = samples[upper - 1]
    high_wavelength, high_value = samples[upper]
    mix = (wavelength - low_wavelength) / (high_wavelength - low_wavelength)
    return low_value + mix * (high_value - low_value)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("svg", type=Path)
    args = parser.parse_args()

    root = ET.parse(args.svg).getroot()
    paths = {
        path.attrib.get("class", ""): path_points(path.attrib["d"])
        for path in root.findall(".//svg:path", SVG_NAMESPACE)
        if "d" in path.attrib
    }

    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(("wavelength_nm", *CHANNEL_CLASSES))
    for wavelength in range(250, 751, 5):
        row: list[str] = [f"{wavelength:.1f}"]
        for path_class in CHANNEL_CLASSES.values():
            value = sample_curve(paths[path_class], float(wavelength))
            row.append("" if value is None else f"{value:.6f}")
        writer.writerow(row)


if __name__ == "__main__":
    main()
