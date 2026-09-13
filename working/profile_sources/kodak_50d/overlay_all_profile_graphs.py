#!/usr/bin/env python3
"""Overlay the runtime Kodak 50D CSV curves on the corresponding Kodak PDF plots."""

import argparse
import csv
import math
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[3]
PROFILE = ROOT / "resources/profiles/kodak_50d"
COLORS = {"B": "#1487ff", "G": "#16a34a", "R": "#ef2929", "N": "#f59e0b"}


def read_csv(name):
    with (PROFILE / name).open(newline="") as handle:
        return list(csv.DictReader(handle))


def number(row, key):
    value = row[key]
    return None if value == "" else float(value)


def map_linear(value, low, high, first, last):
    return first + (value - low) * (last - first) / (high - low)


def draw_curve(draw, rows, x_key, y_key, x_map, y_map, color, width=5):
    points = []
    for row in rows:
        x, y = number(row, x_key), number(row, y_key)
        if x is None or y is None or not math.isfinite(x) or not math.isfinite(y):
            if len(points) > 1:
                draw.line(points, fill=color, width=width, joint="curve")
            points = []
            continue
        points.append((x_map(x), y_map(y)))
    if len(points) > 1:
        draw.line(points, fill=color, width=width, joint="curve")


def render_pages(pdf):
    temporary = tempfile.TemporaryDirectory(prefix="filmviz_50d_pdf_")
    prefix = Path(temporary.name) / "page"
    subprocess.run(
        ["pdftoppm", "-f", "3", "-l", "4", "-r", "300", "-png", str(pdf), str(prefix)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return temporary, Image.open(f"{prefix}-3.png").convert("RGB"), Image.open(f"{prefix}-4.png").convert("RGB")


def annotate(image, xy, text, fill):
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 22)
    box = draw.textbbox(xy, text, font=font, stroke_width=3)
    draw.rectangle((box[0] - 7, box[1] - 4, box[2] + 7, box[3] + 4), fill="white")
    draw.text(xy, text, fill=fill, font=font, stroke_width=1, stroke_fill="white")


def build_overlays(page3, page4):
    # Coordinates are the plot-frame edges in Kodak's 300 dpi page render.
    plots = []

    image = page3.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_50d_modulation_transfer_function_curves.csv")
    xmap = lambda x: map_linear(math.log10(x), 0.0, math.log10(700.0), 354, 1157)
    ymap = lambda y: map_linear(math.log10(y), 0.0, math.log10(200.0), 1564, 957)
    for key, color in (("blue_mtf_percent", COLORS["B"]), ("green_mtf_percent", COLORS["G"]), ("red_mtf_percent", COLORS["R"])):
        draw_curve(draw, rows, "cycles_per_mm", key, xmap, ymap, color)
    annotate(image, (372, 914), "CSV overlay: B / G / R", COLORS["B"])
    plots.append(("MTF", image.crop((250, 855, 1240, 1700))))

    image = page3.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_50d_diffuse_rms_granularity_curves.csv")
    xmap = lambda x: map_linear(x, 0.0, 5.0, 1564, 2168)
    density_map = lambda y: map_linear(y, 0.0, 3.0, 1122, 518)
    grain_map = lambda y: 1122 - math.log10(y / 0.001) * ((1122 - 518) / 3.0)
    for channel, color in (("blue", COLORS["B"]), ("green", COLORS["G"]), ("red", COLORS["R"])):
        draw_curve(draw, rows, "log_relative_exposure", f"{channel}_density", xmap, density_map, color)
        draw_curve(draw, rows, "log_relative_exposure", f"{channel}_granularity_sigma_d", xmap, grain_map, color, 4)
    annotate(image, (1580, 475), "CSV overlay: density + grain B / G / R", COLORS["B"])
    plots.append(("Granularity", image.crop((1490, 430, 2350, 1240))))

    image = page3.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_50d_sensitometric_curves.csv")
    xmap = lambda x: map_linear(x, -8.0, 8.0, 1597, 2290)
    ymap = lambda y: map_linear(y, 0.0, 3.0, 2892, 2199)
    for key, color in (("curve_high_density", COLORS["B"]), ("curve_mid_density", COLORS["G"]), ("curve_low_density", COLORS["R"])):
        draw_curve(draw, rows, "camera_stops", key, xmap, ymap, color)
    annotate(image, (1615, 2152), "CSV overlay: B / G / R", COLORS["B"])
    plots.append(("Sensitometry", image.crop((1500, 2080, 2375, 3025))))

    image = page4.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_50d_spectral_sensitivity_curves.csv")
    xmap = lambda x: map_linear(x, 250.0, 750.0, 361, 1131)
    # The source plot has an enlarged annotation band above 3.0, so its outer
    # rectangle is not a linear 0..4 density axis. Use the same label-baseline
    # calibration as the digitizer and then map SVG coordinates to the PDF.
    ymap = lambda y: map_linear(171.5866666666667 - 34.56 * y, 2.78, 142.01, 575, 1157)
    for key, color in (("yellow_forming_layer", "#f2c500"), ("magenta_forming_layer", "#d927a8"), ("cyan_forming_layer", "#00a6d6")):
        draw_curve(draw, rows, "wavelength_nm", key, xmap, ymap, color)
    annotate(image, (380, 532), "CSV overlay: Y / M / C forming layers", "#d39e00")
    plots.append(("Spectral sensitivity", image.crop((250, 470, 1240, 1320))))

    image = page4.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_50d_spectral_dye_density_curves.csv")
    xmap = lambda x: map_linear(x, 400.0, 800.0, 1551, 2173)
    ymap = lambda y: map_linear(y, -0.2, 1.8, 1171, 549)
    for key, color in (("cyan_peak_normalized", "#00a6d6"), ("magenta_peak_normalized", "#d927a8"), ("yellow_peak_normalized", "#f2c500"), ("midscale_neutral_density", COLORS["N"]), ("minimum_density", "#7c3aed")):
        draw_curve(draw, rows, "wavelength_nm", key, xmap, ymap, color)
    annotate(image, (1570, 506), "CSV overlay: dyes + neutral + minimum", "#d97706")
    plots.append(("Spectral dye density", image.crop((1450, 430, 2260, 1310))))
    return plots


def compose(plots, output):
    cells = []
    for title, crop in plots:
        crop.thumbnail((940, 790), Image.Resampling.LANCZOS)
        cell = Image.new("RGB", (980, 850), "white")
        cell.paste(crop, ((980 - crop.width) // 2, 45))
        ImageDraw.Draw(cell).text((22, 12), title, fill="black", font=ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 26))
        cells.append(cell)
    canvas = Image.new("RGB", (2940, 1760), "white")
    draw = ImageDraw.Draw(canvas)
    draw.text((34, 18), "Kodak VISION3 50D — runtime CSV curves over Kodak PDF (black)", fill="black", font=ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 34))
    for index, cell in enumerate(cells):
        x = (index % 3) * 980
        y = 60 + (index // 3) * 850
        canvas.paste(cell, (x, y))
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pdf", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("kodak_50d_all_curves_pdf_overlay.png"))
    args = parser.parse_args()
    temporary, page3, page4 = render_pages(args.pdf)
    try:
        compose(build_overlays(page3, page4), args.output)
    finally:
        temporary.cleanup()
    print(args.output)


if __name__ == "__main__":
    main()
