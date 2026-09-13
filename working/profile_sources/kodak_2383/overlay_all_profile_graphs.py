#!/usr/bin/env python3
"""Overlay the runtime Kodak 2383 CSV curves on the Kodak PDF plots."""

import argparse
import csv
import math
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[3]
PROFILE = ROOT / "resources/profiles/kodak_2383"
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
    temporary = tempfile.TemporaryDirectory(prefix="filmviz_2383_pdf_")
    prefix = Path(temporary.name) / "page"
    subprocess.run(
        ["pdftoppm", "-f", "4", "-l", "5", "-r", "300", "-png", str(pdf), str(prefix)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return temporary, Image.open(f"{prefix}-4.png").convert("RGB"), Image.open(f"{prefix}-5.png").convert("RGB")


def annotate(image, xy, text, fill):
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 22)
    box = draw.textbbox(xy, text, font=font, stroke_width=3)
    draw.rectangle((box[0] - 7, box[1] - 4, box[2] + 7, box[3] + 4), fill="white")
    draw.text(xy, text, fill=fill, font=font, stroke_width=1, stroke_fill="white")


def build_overlays(page4, page5):
    # Coordinates are plot-frame edges in Kodak's 300 dpi page render.
    plots = []

    image = page4.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_2383_sensitometric_curves.csv")
    xmap = lambda x: map_linear(x, -3.0, 3.0, 290, 1058)
    ymap = lambda y: map_linear(y, 0.0, 6.0, 1094, 324)
    for key, color in (("blue_density", COLORS["B"]), ("green_density", COLORS["G"]), ("red_density", COLORS["R"])):
        draw_curve(draw, rows, "log_exposure", key, xmap, ymap, color)
    annotate(image, (310, 280), "Runtime CSV overlay: B / G / R", COLORS["B"])
    plots.append(("Sensitometry", image.crop((205, 245, 1145, 1190))))

    image = page4.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_2383_diffuse_rms_granularity_curves.csv")
    xmap = lambda x: map_linear(x, 0.0, 3.0, 1463, 2233)
    density_map = lambda y: map_linear(y, 0.0, 4.0, 1072, 305)
    grain_map = lambda y: map_linear(math.log10(y / 0.001), 0.0, 2.0, 1077, 710)
    for channel, color in (("blue", COLORS["B"]), ("green", COLORS["G"]), ("red", COLORS["R"])):
        draw_curve(draw, rows, "log_exposure_lux_seconds", f"{channel}_density", xmap, density_map, color)
        draw_curve(draw, rows, "log_exposure_lux_seconds", f"{channel}_diffuse_rms_granularity", xmap, grain_map, color, 4)
    annotate(image, (1485, 260), "Runtime CSV overlay: density + grain B / G / R", COLORS["B"])
    plots.append(("Diffuse RMS granularity", image.crop((1375, 225, 2360, 1185))))

    image = page4.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_2383_modulation_transfer_function_curves.csv")
    xmap = lambda x: map_linear(math.log10(x), 0.0, math.log10(600.0), 308, 1145)
    ymap = lambda y: map_linear(math.log10(y), 0.0, math.log10(200.0), 2222, 1587)
    for key, color in (("blue_response_percent", COLORS["B"]), ("green_response_percent", COLORS["G"]), ("red_response_percent", COLORS["R"])):
        draw_curve(draw, rows, "spatial_frequency_cycles_per_mm", key, xmap, ymap, color)
    annotate(image, (330, 1540), "Runtime CSV overlay: B / G / R", COLORS["B"])
    plots.append(("MTF", image.crop((220, 1490, 1230, 2350))))

    image = page5.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_2383_spectral_sensitivity_curves.csv")
    xmap = lambda x: map_linear(x, 250.0, 750.0, 318, 1136)
    ymap = lambda y: map_linear(y, -3.0, 1.0, 866, 248)
    for key, color in (("yellow_forming_log_sensitivity", "#f2c500"), ("magenta_forming_log_sensitivity", "#d927a8"), ("cyan_forming_log_sensitivity", "#00a6d6")):
        draw_curve(draw, rows, "wavelength_nm", key, xmap, ymap, color)
    annotate(image, (340, 205), "Runtime CSV overlay: Y / M / C forming layers", "#d39e00")
    plots.append(("Spectral sensitivity", image.crop((230, 175, 1170, 980))))

    image = page5.copy()
    draw = ImageDraw.Draw(image)
    rows = read_csv("kodak_2383_corrected_spectral_dye_density_curves.csv")
    xmap = lambda x: map_linear(x, 250.0, 750.0, 1510, 2253)
    ymap = lambda y: map_linear(y, 0.0, 1.4, 1023, 278)
    for key, color in (("cyan_density", "#00a6d6"), ("magenta_density", "#d927a8"), ("yellow_density", "#f2c500"), ("visual_neutral_density", COLORS["N"])):
        draw_curve(draw, rows, "wavelength_nm", key, xmap, ymap, color)
    annotate(image, (1530, 230), "Production corrected CSV: dyes + visual neutral", "#d97706")
    plots.append(("Spectral dye density", image.crop((1460, 195, 2340, 1125))))
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
    draw.text((34, 18), "Kodak VISION 2383/3383 - runtime CSV curves over Kodak PDF (black)", fill="black", font=ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 34))
    for index, cell in enumerate(cells):
        x = (index % 3) * 980
        y = 60 + (index // 3) * 850
        canvas.paste(cell, (x, y))
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pdf", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("kodak_2383_all_curves_pdf_overlay.png"))
    args = parser.parse_args()
    temporary, page4, page5 = render_pages(args.pdf)
    try:
        compose(build_overlays(page4, page5), args.output)
    finally:
        temporary.cleanup()
    print(args.output)


if __name__ == "__main__":
    main()
