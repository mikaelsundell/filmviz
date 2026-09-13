#!/usr/bin/env python3
"""Overlay the runtime 50D sensitivity CSV on Kodak's 300 dpi page-4 plot.

Render page 4 first with:
  pdftoppm -f 4 -singlefile -r 300 -png INPUT.pdf kodak_50d_page4
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


PLOT_BOUNDS_300_DPI = (361, 575, 1131, 1157)
SVG_X_LEFT = 13.95
SVG_X_RIGHT = 200.34
SVG_Y_BOTTOM = 142.01
SVG_Y_INTERCEPT = 171.5866666666667
SVG_Y_PER_LOG_UNIT = -34.56
CHANNELS = (
    ("yellow_forming_layer", "Yellow-forming", (237, 62, 84, 220)),
    ("magenta_forming_layer", "Magenta-forming", (46, 166, 92, 220)),
    ("cyan_forming_layer", "Cyan-forming", (42, 103, 211, 220)),
)


def load_curves(path: Path) -> dict[str, list[tuple[float, float]]]:
    curves = {column: [] for column, _, _ in CHANNELS}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            wavelength = float(row["wavelength_nm"])
            for column in curves:
                if row[column]:
                    curves[column].append((wavelength, float(row[column])))
    return curves


def graph_point(
    wavelength: float, sensitivity: float, bounds: tuple[int, int, int, int]
) -> tuple[float, float]:
    left, top, right, bottom = bounds
    x = left + (wavelength - 250.0) / 500.0 * (right - left)
    pdf_pixels_per_svg_unit = (right - left) / (SVG_X_RIGHT - SVG_X_LEFT)
    svg_y = SVG_Y_INTERCEPT + SVG_Y_PER_LOG_UNIT * sensitivity
    y = bottom + (svg_y - SVG_Y_BOTTOM) * pdf_pixels_per_svg_unit
    return x, y


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_page", type=Path, help="300 dpi PNG of Kodak PDF page 4")
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--csv",
        type=Path,
        default=Path(__file__).resolve().parents[3]
        / "resources/profiles/kodak_50d/kodak_50d_spectral_sensitivity_curves.csv",
    )
    args = parser.parse_args()

    source = Image.open(args.source_page).convert("RGBA")
    if source.size != (2550, 3300):
        raise SystemExit(f"Expected a 2550x3300 300 dpi page, got {source.size}")

    curves = load_curves(args.csv)
    overlay = Image.new("RGBA", source.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    font = ImageFont.load_default(size=22)

    for column, label, color in CHANNELS:
        points = [graph_point(wavelength, value, PLOT_BOUNDS_300_DPI) for wavelength, value in curves[column]]
        draw.line(points, fill=color, width=7, joint="curve")
        peak_wavelength, peak_value = max(curves[column], key=lambda sample: sample[1])
        peak_x, peak_y = graph_point(peak_wavelength, peak_value, PLOT_BOUNDS_300_DPI)
        draw.ellipse((peak_x - 7, peak_y - 7, peak_x + 7, peak_y + 7), fill=color)
        draw.text(
            (peak_x + 11, peak_y - 27),
            f"{peak_value:.3f} @ {peak_wavelength:.0f} nm",
            fill=color,
            font=font,
            stroke_width=3,
            stroke_fill=(255, 255, 255, 235),
        )

    composited = Image.alpha_composite(source, overlay)
    crop = composited.crop((285, 500, 1215, 1280))
    header = Image.new("RGBA", (crop.width, 108), "white")
    header_draw = ImageDraw.Draw(header)
    header_draw.text((18, 14), "Kodak 50D paper curves (black) vs FilmViz runtime CSV (colour)", fill="black", font=font)
    x = 18
    for _, label, color in CHANNELS:
        header_draw.line((x, 72, x + 42, 72), fill=color, width=7)
        header_draw.text((x + 52, 59), label, fill="black", font=font)
        x += 285

    result = Image.new("RGBA", (crop.width, header.height + crop.height), "white")
    result.alpha_composite(header)
    result.alpha_composite(crop, (0, header.height))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result.convert("RGB").save(args.output)

    for column, label, _ in CHANNELS:
        wavelength, value = max(curves[column], key=lambda sample: sample[1])
        print(f"{label}: {value:.6f} at {wavelength:.0f} nm")


if __name__ == "__main__":
    main()
