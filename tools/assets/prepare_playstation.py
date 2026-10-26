#!/usr/bin/env python3
"""Convert the supplied transparent PlayStation reference to tintable SVG masks.

Run: python3 tools/assets/prepare_playstation.py --source /path/to/reference.webp
Uses the same alpha-mask trace and validation as prepare_branding.py.
"""
import argparse
import json
import xml.etree.ElementTree as ET
from pathlib import Path

from prepare_branding import trace_icon


def remove_registered_mark(path):
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    tree = ET.parse(path)
    removed = 0
    for group in tree.getroot().iter():
        for child in list(group):
            if child.tag.endswith("path") and child.get("d", "").startswith(("M47685 4190", "M47052 3368")):
                group.remove(child)
                removed += 1
    if removed != 2:
        raise ValueError("Unexpected PlayStation reference contours; review trademark removal")
    tree.write(path, encoding="utf-8", xml_declaration=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[2])
    arguments = parser.parse_args()
    editable = arguments.project / "assets" / "branding" / "svg" / "playstation.svg"
    runtime = arguments.project / "romfs" / "ui" / "playstation.svg"
    metrics, _, _ = trace_icon(arguments.source, editable, runtime)
    if metrics["mask_intersection_over_union"] < 0.99:
        raise ValueError("PlayStation: traced silhouette differs by more than 1%")
    remove_registered_mark(editable)
    remove_registered_mark(runtime)
    metrics["registered_mark_removed_paths"] = 2
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()

