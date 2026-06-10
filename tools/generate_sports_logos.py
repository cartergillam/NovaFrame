#!/usr/bin/env python3
"""
Generate 32x32 sports logo previews, color masks, and C++ bitmap arrays.

Input layout:
  sports_logos/source/{league}/{TEAM}.png

Output layout:
  sports_logos/generated/normalized/{league}/{TEAM}.png
  sports_logos/generated/quantized/{league}/{TEAM}.png
  sports_logos/generated/masks/{league}/{TEAM}/layer_*.png
  sports_logos/generated/contact_sheet.png
  sports_logos/generated/SportsLogosGenerated.h
  sports_logos/generated/SportsLogosGenerated.cpp
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from PIL import Image, ImageDraw, ImageFont


DEFAULT_SOURCE_DIR = Path("sports_logos/source")
DEFAULT_OUTPUT_DIR = Path("sports_logos/generated")
DEFAULT_REVIEW_MANIFEST = Path("sports_logos/review_status.json")
LEAGUE_ORDER = ("nhl", "nfl", "nba", "mlb")
VALID_REVIEW_STATUSES = {"pass", "tune", "manual"}


@dataclass(frozen=True)
class Layer:
    color: tuple[int, int, int]
    pixels: frozenset[tuple[int, int]]


@dataclass(frozen=True)
class Logo:
    league: str
    team: str
    source: Path
    status: str
    notes: str
    normalized: Image.Image
    quantized: Image.Image
    layers: tuple[Layer, ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate NovaFrame sports logo assets.")
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--cpp-dir", type=Path, default=Path("."))
    parser.add_argument("--size", type=int, default=32)
    parser.add_argument("--padding", type=int, default=1)
    parser.add_argument("--max-colors", type=int, default=5)
    parser.add_argument("--alpha-threshold", type=int, default=48)
    parser.add_argument("--min-layer-pixels", type=int, default=2)
    parser.add_argument("--review-manifest", type=Path, default=DEFAULT_REVIEW_MANIFEST)
    parser.add_argument("--no-cpp", action="store_true", help="Skip generated C++ output.")
    return parser.parse_args()


def load_review_manifest(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"logos": {}}
    with path.open("r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    if not isinstance(manifest, dict):
        raise ValueError(f"{path} must contain a JSON object")
    if not isinstance(manifest.get("logos", {}), dict):
        raise ValueError(f"{path} field 'logos' must be an object")
    return manifest


def review_entry(manifest: dict[str, Any], league: str, team: str) -> dict[str, Any]:
    logos = manifest.get("logos", {})
    league_entries = logos.get(league, {})
    if not isinstance(league_entries, dict):
        return {}
    entry = league_entries.get(team, {})
    if not isinstance(entry, dict):
        return {}
    return entry


def review_status(entry: dict[str, Any]) -> str:
    status = str(entry.get("status", "pass")).lower()
    if status not in VALID_REVIEW_STATUSES:
        raise ValueError(f"Invalid review status '{status}'")
    return status


def review_notes(entry: dict[str, Any]) -> str:
    notes = entry.get("notes", "")
    return notes if isinstance(notes, str) else ""


def override_int(entry: dict[str, Any], key: str, default: int) -> int:
    overrides = entry.get("overrides", {})
    if not isinstance(overrides, dict) or key not in overrides:
        return default
    value = overrides[key]
    if not isinstance(value, int):
        raise ValueError(f"Override '{key}' must be an integer")
    return value


def override_source(entry: dict[str, Any], root: Path, default: Path) -> Path:
    overrides = entry.get("overrides", {})
    if not isinstance(overrides, dict) or "sourceOverride" not in overrides:
        return default
    raw = overrides["sourceOverride"]
    if not isinstance(raw, str) or not raw:
        raise ValueError("Override 'sourceOverride' must be a non-empty string")
    path = Path(raw)
    if not path.is_absolute():
        path = root / path
    if not path.exists():
        raise FileNotFoundError(f"sourceOverride does not exist: {path}")
    return path


def review_coverage(
    manifest: dict[str, Any],
    source_paths: list[Path],
) -> tuple[list[str], list[str], list[str]]:
    source_keys = {(path.parent.name.lower(), path.stem.upper()) for path in source_paths}
    manifest_keys: set[tuple[str, str]] = set()
    unresolved_tune: list[str] = []

    logos = manifest.get("logos", {})
    if isinstance(logos, dict):
        for league, teams in logos.items():
            if not isinstance(teams, dict):
                continue
            for team, entry in teams.items():
                team_key = (str(league).lower(), str(team).upper())
                manifest_keys.add(team_key)
                if isinstance(entry, dict) and review_status(entry) == "tune":
                    unresolved_tune.append(f"{team_key[0].upper()} {team_key[1]}")

    missing = [f"{league.upper()} {team}" for league, team in sorted(source_keys - manifest_keys)]
    extra = [f"{league.upper()} {team}" for league, team in sorted(manifest_keys - source_keys)]
    return missing, extra, sorted(unresolved_tune)


def iter_sources(source_dir: Path) -> Iterable[Path]:
    for league_dir in sorted(source_dir.iterdir(), key=lambda p: league_sort_key(p.name)):
        if not league_dir.is_dir():
            continue
        for path in sorted(league_dir.glob("*.png")):
            if path.name.startswith("."):
                continue
            yield path


def league_sort_key(name: str) -> tuple[int, str]:
    lowered = name.lower()
    try:
        return (LEAGUE_ORDER.index(lowered), lowered)
    except ValueError:
        return (len(LEAGUE_ORDER), lowered)


def normalize_image(path: Path, size: int, padding: int, alpha_threshold: int) -> Image.Image:
    image = Image.open(path).convert("RGBA")
    alpha = image.getchannel("A")
    hard_alpha = alpha.point(lambda value: 255 if value >= alpha_threshold else 0)
    bbox = hard_alpha.getbbox() or alpha.getbbox()
    if bbox:
        image = image.crop(bbox)

    target_inner = max(1, size - padding * 2)
    image.thumbnail((target_inner, target_inner), Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    x = (size - image.width) // 2
    y = (size - image.height) // 2
    canvas.alpha_composite(image, (x, y))
    return canvas


def visible_pixels(image: Image.Image, alpha_threshold: int) -> list[tuple[int, int, int, int, int, int]]:
    pixels = image.load()
    out: list[tuple[int, int, int, int, int, int]] = []
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = pixels[x, y]
            if a >= alpha_threshold:
                out.append((x, y, r, g, b, a))
    return out


def initial_centers(samples: list[tuple[int, int, int, int, int, int]], max_colors: int) -> list[tuple[float, float, float]]:
    bins: dict[tuple[int, int, int], int] = {}
    for _, _, r, g, b, a in samples:
        key = (snap_channel(r, 24), snap_channel(g, 24), snap_channel(b, 24))
        bins[key] = bins.get(key, 0) + a

    centers: list[tuple[float, float, float]] = []
    for color, _ in sorted(bins.items(), key=lambda item: item[1], reverse=True):
        if all(color_distance(color, center) >= 26 for center in centers):
            centers.append(tuple(float(c) for c in color))
        if len(centers) >= max_colors:
            break

    if not centers and samples:
        _, _, r, g, b, _ = samples[0]
        centers.append((float(r), float(g), float(b)))
    return centers


def snap_channel(value: int, step: int) -> int:
    return max(0, min(255, int(round(value / step) * step)))


def color_distance(left: tuple[int, int, int] | tuple[float, float, float], right: tuple[int, int, int] | tuple[float, float, float]) -> float:
    return math.sqrt(sum((float(a) - float(b)) ** 2 for a, b in zip(left, right)))


def nearest_center(color: tuple[int, int, int], centers: list[tuple[float, float, float]]) -> int:
    return min(range(len(centers)), key=lambda index: color_distance(color, centers[index]))


def cluster_layers(
    image: Image.Image,
    max_colors: int,
    alpha_threshold: int,
    min_layer_pixels: int,
) -> tuple[Image.Image, tuple[Layer, ...]]:
    samples = visible_pixels(image, alpha_threshold)
    if not samples:
        return Image.new("RGBA", image.size, (0, 0, 0, 0)), tuple()

    centers = initial_centers(samples, max_colors)
    for _ in range(10):
        buckets: list[list[tuple[int, int, int, int, int, int]]] = [[] for _ in centers]
        for sample in samples:
            _, _, r, g, b, _ = sample
            buckets[nearest_center((r, g, b), centers)].append(sample)

        next_centers: list[tuple[float, float, float]] = []
        for index, bucket in enumerate(buckets):
            if not bucket:
                next_centers.append(centers[index])
                continue
            total_weight = sum(sample[5] for sample in bucket)
            next_centers.append(
                (
                    sum(sample[2] * sample[5] for sample in bucket) / total_weight,
                    sum(sample[3] * sample[5] for sample in bucket) / total_weight,
                    sum(sample[4] * sample[5] for sample in bucket) / total_weight,
                )
            )
        if max(color_distance(a, b) for a, b in zip(centers, next_centers)) < 0.25:
            centers = next_centers
            break
        centers = next_centers

    grouped: list[dict[str, object]] = [
        {"color": normalize_palette_color(center), "pixels": set()} for center in centers
    ]
    for x, y, r, g, b, _ in samples:
        index = nearest_center((r, g, b), centers)
        grouped[index]["pixels"].add((x, y))  # type: ignore[index, union-attr]

    grouped = [item for item in grouped if len(item["pixels"]) >= min_layer_pixels]  # type: ignore[arg-type]
    grouped.sort(key=lambda item: len(item["pixels"]), reverse=True)  # type: ignore[arg-type]

    quantized = Image.new("RGBA", image.size, (0, 0, 0, 0))
    quantized_pixels = quantized.load()
    layers: list[Layer] = []
    for item in grouped:
        color = item["color"]  # type: ignore[assignment]
        pixels = frozenset(item["pixels"])  # type: ignore[arg-type]
        for x, y in pixels:
            quantized_pixels[x, y] = (*color, 255)
        layers.append(Layer(color=color, pixels=pixels))

    return quantized, tuple(layers)


def normalize_palette_color(center: tuple[float, float, float]) -> tuple[int, int, int]:
    r, g, b = (int(round(c)) for c in center)
    if r > 232 and g > 232 and b > 232:
        return (255, 255, 255)
    if r < 24 and g < 24 and b < 24:
        return (0, 0, 0)
    return (r, g, b)


def write_pngs(logo: Logo, output_dir: Path) -> None:
    normalized_dir = output_dir / "normalized" / logo.league
    quantized_dir = output_dir / "quantized" / logo.league
    masks_dir = output_dir / "masks" / logo.league / logo.team
    normalized_dir.mkdir(parents=True, exist_ok=True)
    quantized_dir.mkdir(parents=True, exist_ok=True)
    masks_dir.mkdir(parents=True, exist_ok=True)

    logo.normalized.save(normalized_dir / f"{logo.team}.png")
    logo.quantized.save(quantized_dir / f"{logo.team}.png")

    for stale in masks_dir.glob("*.png"):
        stale.unlink()

    for index, layer in enumerate(logo.layers):
        mask = Image.new("L", logo.normalized.size, 0)
        draw = ImageDraw.Draw(mask)
        for x, y in layer.pixels:
            draw.point((x, y), fill=255)
        hex_color = color_hex(layer.color)
        mask.convert("RGBA").save(masks_dir / f"layer_{index:02d}_{hex_color}.png")


def clean_output_dir(output_dir: Path) -> None:
    for name in ("normalized", "quantized", "masks"):
        path = output_dir / name
        if path.exists():
            shutil.rmtree(path)
    for name in ("contact_sheet.png", "SportsLogosGenerated.h", "SportsLogosGenerated.cpp"):
        path = output_dir / name
        if path.exists():
            path.unlink()
    for path in output_dir.glob("SportsLogosGenerated*.h"):
        path.unlink()
    for path in output_dir.glob("SportsLogosGenerated*.cpp"):
        path.unlink()


def color_hex(color: tuple[int, int, int]) -> str:
    return "".join(f"{channel:02X}" for channel in color)


def identifier(*parts: str) -> str:
    raw = "_".join(parts).lower()
    return re.sub(r"[^a-z0-9_]", "_", raw)


def pack_bitmap(layer: Layer, size: int) -> list[int]:
    data: list[int] = []
    for y in range(size):
        for byte_x in range(0, size, 8):
            value = 0
            for bit in range(8):
                x = byte_x + bit
                if (x, y) in layer.pixels:
                    value |= 1 << (7 - bit)
            data.append(value)
    return data


def write_cpp(logos: list[Logo], output_dir: Path, size: int) -> None:
    header = output_dir / "SportsLogosGenerated.h"
    source = output_dir / "SportsLogosGenerated.cpp"
    header.write_text(
        """#pragma once

#include <Arduino.h>

struct SportsLogoLayerDef {
  const uint8_t* bitmap;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct SportsLogoDef {
  const char* league;
  const char* team;
  uint8_t width;
  uint8_t height;
  uint8_t layerCount;
  const SportsLogoLayerDef* layers;
};

extern const SportsLogoDef SPORTS_LOGOS[];
extern const size_t SPORTS_LOGO_COUNT;
""",
        encoding="utf-8",
    )

    lines: list[str] = [
        '#include "SportsLogosGenerated.h"',
        "",
        "namespace {",
        "",
    ]
    for logo in logos:
        base = identifier("bitmap", logo.league, logo.team)
        for index, layer in enumerate(logo.layers):
            bitmap_name = f"{base}_layer_{index}"
            bytes_out = pack_bitmap(layer, size)
            lines.append(f"const uint8_t {bitmap_name}[] PROGMEM = {{")
            for offset in range(0, len(bytes_out), 12):
                chunk = ", ".join(f"0x{value:02X}" for value in bytes_out[offset : offset + 12])
                lines.append(f"  {chunk},")
            lines.append("};")
            lines.append("")

        layer_name = identifier("layers", logo.league, logo.team)
        lines.append(f"const SportsLogoLayerDef {layer_name}[] = {{")
        for index, layer in enumerate(logo.layers):
            r, g, b = layer.color
            bitmap_name = f"{base}_layer_{index}"
            lines.append(f"  {{ {bitmap_name}, {r}, {g}, {b} }},")
        lines.append("};")
        lines.append("")

    lines.extend(
        [
            "}  // namespace",
            "",
            "const SportsLogoDef SPORTS_LOGOS[] = {",
        ]
    )
    for logo in logos:
        layer_name = identifier("layers", logo.league, logo.team)
        lines.append(
            f'  {{ "{logo.league}", "{logo.team}", {size}, {size}, {len(logo.layers)}, {layer_name} }},'
        )
    lines.extend(
        [
            "};",
            "",
            "const size_t SPORTS_LOGO_COUNT = sizeof(SPORTS_LOGOS) / sizeof(SPORTS_LOGOS[0]);",
            "",
        ]
    )
    source.write_text("\n".join(lines), encoding="utf-8")


def write_contact_sheet(logos: list[Logo], output_dir: Path, size: int) -> None:
    if not logos:
        return
    scale = 4
    tile_w = size * scale
    tile_h = size * scale + 14
    cols = 10
    rows = math.ceil(len(logos) / cols)
    sheet = Image.new("RGB", (cols * tile_w, rows * tile_h), (18, 18, 18))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()

    for index, logo in enumerate(logos):
        col = index % cols
        row = index // cols
        x = col * tile_w
        y = row * tile_h
        preview = logo.quantized.resize((tile_w, tile_w), Image.Resampling.NEAREST)
        checker = Image.new("RGB", preview.size, (0, 0, 0))
        checker.paste(preview, mask=preview.getchannel("A"))
        sheet.paste(checker, (x, y))
        label = f"{logo.league.upper()} {logo.team} {status_badge(logo.status)}"
        draw.text((x + 2, y + tile_w + 2), label, fill=status_color(logo.status), font=font)

    sheet.save(output_dir / "contact_sheet.png")


def status_badge(status: str) -> str:
    if status == "manual":
        return "M"
    if status == "tune":
        return "T"
    return "P"


def status_color(status: str) -> tuple[int, int, int]:
    if status == "manual":
        return (255, 110, 110)
    if status == "tune":
        return (255, 190, 70)
    return (230, 230, 230)


def main() -> int:
    args = parse_args()
    output_dir: Path = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    clean_output_dir(output_dir)
    manifest = load_review_manifest(args.review_manifest)
    source_paths = list(iter_sources(args.source_dir))

    logos: list[Logo] = []
    for path in source_paths:
        league = path.parent.name.lower()
        team = path.stem.upper()
        entry = review_entry(manifest, league, team)
        status = review_status(entry)
        notes = review_notes(entry)
        source_path = override_source(entry, args.review_manifest.parent, path)
        padding = override_int(entry, "padding", args.padding)
        max_colors = override_int(entry, "maxColors", args.max_colors)
        alpha_threshold = override_int(entry, "alphaThreshold", args.alpha_threshold)

        normalized = normalize_image(source_path, args.size, padding, alpha_threshold)
        quantized, layers = cluster_layers(
            normalized,
            max_colors,
            alpha_threshold,
            args.min_layer_pixels,
        )
        logo = Logo(league, team, source_path, status, notes, normalized, quantized, layers)
        write_pngs(logo, output_dir)
        logos.append(logo)

    write_contact_sheet(logos, output_dir, args.size)
    if not args.no_cpp:
        args.cpp_dir.mkdir(parents=True, exist_ok=True)
        write_cpp(logos, args.cpp_dir, args.size)

    layer_count = sum(len(logo.layers) for logo in logos)
    missing, extra, unresolved_tune = review_coverage(manifest, source_paths)
    print(f"Generated {len(logos)} logos and {layer_count} color layers in {output_dir}")
    print(
        "Review statuses: "
        f"{sum(1 for logo in logos if logo.status == 'pass')} pass, "
        f"{sum(1 for logo in logos if logo.status == 'manual')} manual, "
        f"{sum(1 for logo in logos if logo.status == 'tune')} tune"
    )
    if missing:
        print(f"Review manifest missing {len(missing)} source logos: {', '.join(missing)}")
    if extra:
        print(f"Review manifest has {len(extra)} extra entries: {', '.join(extra)}")
    if unresolved_tune:
        print(f"Unresolved tune items: {', '.join(unresolved_tune)}")
    print(f"Review {output_dir / 'contact_sheet.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
