#!/usr/bin/env python3
"""Isolate private curl and its consumer without rewriting system dependencies."""
import argparse
from pathlib import Path
import subprocess


PREFIX = "fog_moonlight_"


def symbols(nm, archive, defined):
    output = subprocess.check_output(
        [nm, "-g", "--defined-only" if defined else "--undefined-only",
         "--format=posix", str(archive)], text=True)
    return {fields[0] for line in output.splitlines()
            if len(fields := line.split()) >= 2 and not line.endswith(":")}


def is_curl(name):
    return name.startswith(("curl_", "curlx_", "Curl_"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--create-map", action="store_true")
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--ranlib", required=True)
    args = parser.parse_args()
    if args.create_map:
        names = symbols(args.nm, args.archive, True)
        if "curl_easy_init" not in names or any(not is_curl(name) for name in names):
            raise SystemExit("Unexpected curl exports; review the namespace map before building")
        contents = "".join(f"{name} {PREFIX}{name}\n" for name in sorted(names))
        if not args.map.exists() or args.map.read_text() != contents:
            args.map.write_text(contents)
    mappings = dict(line.split() for line in args.map.read_text().splitlines())
    if not mappings or any(not is_curl(old) or new != PREFIX + old
                           for old, new in mappings.items()):
        raise SystemExit("Invalid private curl namespace map")
    references = symbols(args.nm, args.archive, False)
    unknown = {name for name in references if is_curl(name) and name not in mappings}
    if unknown:
        raise SystemExit(f"Unmapped curl references: {', '.join(sorted(unknown))}")
    output = args.output or args.archive
    temporary = output.with_name(output.name + ".namespace-tmp")
    try:
        subprocess.run([args.objcopy, "--redefine-syms", str(args.map),
                        str(args.archive), str(temporary)], check=True)
        subprocess.run([args.ranlib, "-D", str(temporary)], check=True)
        remaining = symbols(args.nm, temporary, True) | symbols(args.nm, temporary, False)
        if any(is_curl(name) for name in remaining):
            raise SystemExit("Unprefixed curl symbols remain after isolation")
        if args.create_map:
            expected = set(mappings.values())
            if symbols(args.nm, temporary, True) != expected:
                raise SystemExit("Private curl exports differ from its namespace map")
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
