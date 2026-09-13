#!/usr/bin/env python3
"""Compare the FilmViz DCI-2K ColorChecker fixture with a reference render."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import OpenImageIO as oiio
from PIL import Image, ImageDraw, ImageFont


PATCHES = (
    ("Dark skin", 152, 113), ("Light skin", 428, 112),
    ("Blue sky", 687, 119), ("Foliage", 959, 118),
    ("Blue flower", 1223, 121), ("Bluish green", 1493, 113),
    ("Orange", 159, 370), ("Purplish blue", 412, 368),
    ("Moderate red", 686, 365), ("Purple", 952, 365),
    ("Yellow green", 1209, 354), ("Orange yellow", 1474, 365),
    ("Blue", 161, 595), ("Green", 424, 600),
    ("Red", 683, 611), ("Yellow", 955, 608),
    ("Magenta", 1202, 603), ("Cyan", 1489, 610),
    ("White", 146, 846), ("Neutral 8", 418, 867),
    ("Neutral 6.5", 714, 869), ("Neutral 5", 936, 866),
    ("Neutral 3.5", 1174, 857), ("Black", 1488, 840),
)


def rgb8(rgb: tuple[float, float, float]) -> tuple[int, int, int]:
    return tuple(round(max(0.0, min(1.0, value)) * 255.0) for value in rgb)


def sample(image: oiio.ImageBuf, x: int, y: int) -> tuple[float, float, float]:
    pixel = image.getpixel(x, y)
    return float(pixel[0]), float(pixel[1]), float(pixel[2])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("converted", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    converted_image = oiio.ImageBuf(str(args.converted))
    reference_image = oiio.ImageBuf(str(args.reference))
    converted = [sample(converted_image, x, y) for _, x, y in PATCHES]
    reference = [sample(reference_image, x, y) for _, x, y in PATCHES]
    errors = [
        tuple(output - target for output, target in zip(converted_rgb, reference_rgb))
        for converted_rgb, reference_rgb in zip(converted, reference)
    ]
    mae = sum(abs(value) for error in errors for value in error) / (len(errors) * 3)
    rmse = math.sqrt(sum(value * value for error in errors for value in error) / (len(errors) * 3))
    channel_bias = tuple(sum(error[channel] for error in errors) / len(errors) for channel in range(3))

    width, height = 1660, 1040
    image = Image.new("RGB", (width, height), (20, 20, 22))
    draw = ImageDraw.Draw(image)
    title_font = ImageFont.load_default(size=28)
    font = ImageFont.load_default(size=18)
    small_font = ImageFont.load_default(size=15)
    draw.text((28, 22), "Kodak 50D corrected curves - empirical colour response bypassed", fill="white", font=title_font)
    draw.text(
        (28, 64),
        f"24-patch encoded RGB: MAE {mae:.4f}   RMSE {rmse:.4f}   bias R/G/B {channel_bias[0]:+.4f} / {channel_bias[1]:+.4f} / {channel_bias[2]:+.4f}",
        fill=(205, 205, 210),
        font=font,
    )
    draw.text((28, 94), "Each swatch: target on top, FilmViz on bottom", fill=(160, 160, 165), font=small_font)

    card_width, card_height = 210, 190
    x_gap, y_gap = 16, 16
    x_origin, y_origin = 28, 130
    for index, ((name, _, _), output_rgb, target_rgb, error) in enumerate(zip(PATCHES, converted, reference, errors)):
        column = index % 6
        row = index // 6
        x = x_origin + column * (card_width + x_gap)
        y = y_origin + row * (card_height + y_gap)
        draw.rounded_rectangle((x, y, x + card_width, y + card_height), radius=8, fill=(35, 35, 38))
        draw.rectangle((x + 10, y + 10, x + card_width - 10, y + 66), fill=rgb8(target_rgb))
        draw.rectangle((x + 10, y + 66, x + card_width - 10, y + 122), fill=rgb8(output_rgb))
        draw.text((x + 10, y + 130), f"{index + 1}. {name}", fill="white", font=font)
        draw.text(
            (x + 10, y + 157),
            f"dRGB {error[0]:+.3f} {error[1]:+.3f} {error[2]:+.3f}",
            fill=(185, 185, 190),
            font=small_font,
        )

    bar_left, bar_top, bar_width = 1410, 138, 215
    draw.text((bar_left, 106), "Patch RGB RMSE", fill="white", font=font)
    for index, error in enumerate(errors):
        value = math.sqrt(sum(component * component for component in error) / 3.0)
        y = bar_top + index * 35
        draw.text((bar_left, y), f"{index + 1:2d}", fill=(190, 190, 195), font=small_font)
        draw.rectangle((bar_left + 30, y + 3, bar_left + 30 + min(value / 0.15, 1.0) * bar_width, y + 18), fill=(225, 91, 102))
        draw.text((bar_left + 34 + min(value / 0.15, 1.0) * bar_width, y), f"{value:.3f}", fill="white", font=small_font)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(f"MAE={mae:.8f} RMSE={rmse:.8f}")
    print(f"bias={channel_bias[0]:+.8f},{channel_bias[1]:+.8f},{channel_bias[2]:+.8f}")


if __name__ == "__main__":
    main()
