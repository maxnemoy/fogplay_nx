#!/usr/bin/env python3
"""Trace supplied monochrome PNG alpha masks and prepare FogPlay branding.

Dependencies (Ubuntu): apt install potrace python3-pil python3-cairosvg
Run: python3 tools/assets/prepare_branding.py --source /path/to/static \
    --svg-export /path/to/static/svg
The source directory is read only. Editable SVGs use currentColor; SDL_image
receives explicit white SVG paths so SDL_SetTextureColorMod can tint them.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import cairosvg
from PIL import Image, ImageChops, ImageDraw, ImageOps, ImageStat

ICONS = ("magnifying-glass", "writing", "setting", "game-controller")
SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)


def trace_icon(source, editable, runtime):
    image = Image.open(source).convert("RGBA")
    width, height = image.size
    alpha = image.getchannel("A")
    if alpha.getextrema() == (255, 255):
        raise ValueError(f"{source}: expected a transparent monochrome icon")
    enlarged = alpha.resize((width * 4, height * 4), Image.Resampling.LANCZOS)
    mask = enlarged.point(lambda value: 0 if value >= 128 else 255, mode="1")
    with tempfile.TemporaryDirectory(prefix="fogplay-trace-") as temporary:
        bitmap = Path(temporary) / "mask.pbm"
        traced = Path(temporary) / "trace.svg"
        mask.save(bitmap)
        subprocess.run(
            ["potrace", str(bitmap), "--svg", "--output", str(traced),
             "--turdsize", "0", "--alphamax", "1", "--opttolerance", "0.15"],
            check=True,
        )
        root = ET.parse(traced).getroot()
    root.set("width", str(width))
    root.set("height", str(height))
    root.set("viewBox", f"0 0 {width} {height}")
    for metadata in root.findall(f"{{{SVG_NS}}}metadata"):
        root.remove(metadata)
    content = list(root)
    for element in content:
        root.remove(element)
    scaled = ET.SubElement(root, f"{{{SVG_NS}}}g", {"transform": "scale(0.25)"})
    scaled.extend(content)
    for element in root.iter():
        if element.get("fill") is not None:
            element.set("fill", "currentColor")
    ET.ElementTree(root).write(editable, encoding="utf-8", xml_declaration=True)
    for element in root.iter():
        if element.get("fill") == "currentColor":
            element.set("fill", "#ffffff")
    ET.ElementTree(root).write(runtime, encoding="utf-8", xml_declaration=True)
    rendered = Image.open(io.BytesIO(cairosvg.svg2png(url=str(runtime)))).convert("RGBA")
    source_mask = alpha.point(lambda value: 255 if value >= 128 else 0, mode="1")
    result_mask = rendered.getchannel("A").point(
        lambda value: 255 if value >= 128 else 0, mode="1"
    )
    intersection = ImageChops.logical_and(source_mask, result_mask).histogram()[255]
    union = ImageChops.logical_or(source_mask, result_mask).histogram()[255]
    error = ImageStat.Stat(ImageChops.difference(alpha, rendered.getchannel("A"))).mean[0]
    return {
        "source": source.name,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "size": [width, height],
        "alpha_range": list(alpha.getextrema()),
        "mask_intersection_over_union": round(intersection / union, 6),
        "alpha_mean_absolute_error_0_255": round(error, 4),
        "editable_bytes": editable.stat().st_size,
        "runtime_bytes": runtime.stat().st_size,
    }, alpha, rendered.getchannel("A")


def create_preview(rows, destination):
    preview = Image.new("RGB", (840, 230 * len(rows)), "#171c28")
    draw = ImageDraw.Draw(preview)
    for index, (name, source, traced) in enumerate(rows):
        top = index * 230
        draw.text((16, top + 10), name + " / source PNG alpha / traced SVG", fill="white")
        for mask, left in ((source, 30), (traced, 260)):
            mask = mask.resize((180, 180), Image.Resampling.LANCZOS)
            preview.paste("white", (left, top + 36, left + 180, top + 216), mask)
        for mask, left in ((source, 510), (traced, 610)):
            mask = mask.resize((48, 48), Image.Resampling.LANCZOS)
            preview.paste("white", (left, top + 102, left + 48, top + 150), mask)
        draw.text((500, top + 176), "48 px: PNG / SVG", fill="#aeb8cd")
    preview.save(destination)


def prepare(source, project, svg_export):
    branding = project / "assets" / "branding"
    originals = branding / "original"
    editable_dir = branding / "svg"
    runtime_dir = project / "romfs" / "ui"
    for directory in (originals, editable_dir, runtime_dir):
        directory.mkdir(parents=True, exist_ok=True)
    if svg_export:
        svg_export.mkdir(parents=True, exist_ok=True)
    report = {"icons": [], "method": "potrace on 4x Lanczos alpha >= 128; no silhouette redesign"}
    rows = []
    for name in ICONS:
        original = source / f"{name}.png"
        shutil.copy2(original, originals / original.name)
        editable = editable_dir / f"{name}.svg"
        runtime = runtime_dir / f"{name}.svg"
        metrics, alpha, traced_alpha = trace_icon(original, editable, runtime)
        report["icons"].append(metrics)
        rows.append((name, alpha, traced_alpha))
        if metrics["mask_intersection_over_union"] < 0.99:
            raise ValueError(f"{name}: traced silhouette differs by more than 1%")
        if svg_export:
            shutil.copy2(editable, svg_export / editable.name)
    for name in ("cover.png", "logo.png"):
        shutil.copy2(source / name, originals / name)
    shutil.copy2(source / "logo.png", runtime_dir / "logo.png")
    cover = Image.open(source / "cover.png").convert("RGBA")
    contained = ImageOps.contain(cover, (256, 256), Image.Resampling.LANCZOS)
    icon = Image.new("RGB", (256, 256), "#000000")
    position = ((256 - contained.width) // 2, (256 - contained.height) // 2)
    icon.paste(contained, position, contained.getchannel("A"))
    icon.save(project / "assets" / "icon.jpg", "JPEG", quality=95, subsampling=0)
    report["cover"] = {
        "original_size": list(cover.size),
        "jpeg_size": [256, 256],
        "contained_size": list(contained.size),
        "offset": list(position),
        "background": "#000000",
        "jpeg_bytes": (project / "assets" / "icon.jpg").stat().st_size,
        "source_sha256": hashlib.sha256((source / "cover.png").read_bytes()).hexdigest(),
    }
    report["logo"] = {
        "size": list(Image.open(runtime_dir / "logo.png").size),
        "copied_without_modification": True,
        "source_sha256": hashlib.sha256((source / "logo.png").read_bytes()).hexdigest(),
    }
    create_preview(rows, branding / "comparison.png")
    (branding / "conversion-report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--svg-export", type=Path)
    arguments = parser.parse_args()
    prepare(arguments.source, arguments.project, arguments.svg_export)


if __name__ == "__main__":
    main()
