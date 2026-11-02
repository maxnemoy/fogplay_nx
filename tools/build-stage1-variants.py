#!/usr/bin/env python3
"""Build isolated Stage 1 Switch experiments from one unchanged source snapshot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PROFILES = {"control": 0, "c1": 1, "c2": 2, "s5": 4, "p9": 8,
            "p10": 16, "s14": 32, "s10": 64, "p4_legacy": 128, "p4": 128, "s16": 256, "full": 255}

def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()

def source_digest():
    paths = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT).split(b"\0")
    result = hashlib.sha256()
    for name in sorted(set(paths) - {b""}):
        path = ROOT / os.fsdecode(name)
        if path.is_file() and (name.startswith((b"src/", b"tools/", b"tests/", b"romfs/", b"assets/", b"licenses/")) or name in (b"Makefile", b"build.sh", b"package.py", b"README.md", b"LICENSE", b"docs/THIRD-PARTY.md")):
            result.update(name + b"\0" + digest(path).encode() + b"\0")
    return result.hexdigest()

def dependency_inputs():
    sdk = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
    compiler = Path(os.environ.get("DEVKITA64", str(sdk / "devkitA64")))
    roots = [compiler, sdk / "libnx", sdk / "portlibs/switch", sdk / "tools",
             ROOT / "third_party/rtc/switch", ROOT / "third_party/moonlight/switch",
             ROOT / "third_party/ffmpeg-diag/switch"]
    fingerprints = {}
    for directory in roots:
        if not directory.is_dir():
            raise RuntimeError(f"Missing build dependency: {directory}")
        for path in sorted(directory.rglob("*")):
            if path.is_file():
                fingerprints[str(path)] = digest(path)
    compiler_id = subprocess.check_output([str(compiler / "bin/aarch64-none-elf-g++"), "--version"], text=True).strip()
    return {"compiler": compiler_id, "files": fingerprints}

def input_digest(inputs):
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profiles", nargs="+", choices=PROFILES, default=list(PROFILES))
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--mts-media-workers", type=int, choices=(0, 1), default=0,
                        help="Keep 0 for Stage 1 P4 comparisons; 1 selects the Stage 3 media path")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    profiles = list(dict.fromkeys(args.profiles))
    # Leave dist with full Stage 1 features and the explicitly selected media
    # path. The default synchronous matrix isolates P4, not ordinary Stage 3.
    if "full" in profiles:
        profiles.remove("full")
    profiles.append("full")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Do not replace a completed variant from another source snapshot.
    for profile in profiles:
        if (output / profile).exists():
            parser.error(f"output already exists: {output / profile}")
    source = source_digest()
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    # Resolve/rebuild dependencies BEFORE capturing the fixed experiment inputs.
    warmup = os.environ.copy()
    warmup.update(STAGE1_PROFILE="full", BOUNDED_SYNC_AUDIO="0", JOBS=str(args.jobs),
                  MTS_MEDIA_WORKERS=str(args.mts_media_workers))
    with (output / "prepare.log").open("w") as log:
        subprocess.run(["bash", "build.sh"], cwd=ROOT, env=warmup, stdout=log, stderr=subprocess.STDOUT, check=True)
    if source_digest() != source:
        raise RuntimeError("Source changed during dependency preparation")
    dependencies = dependency_inputs()
    dependency_hash = input_digest(dependencies)
    (output / "build-inputs.json").write_text(json.dumps(dependencies, indent=2) + "\n")
    results = []
    for profile in profiles:
        if source_digest() != source:
            raise RuntimeError("Source changed during variant build; comparisons would be invalid")
        if input_digest(dependency_inputs()) != dependency_hash:
            raise RuntimeError("Toolchain/dependency inputs changed between variants")
        target = output / profile
        target.mkdir()
        env = os.environ.copy()
        env["STAGE1_PROFILE"] = profile
        env["BOUNDED_SYNC_AUDIO"] = "0"  # S16 is selected only by the profile mask.
        env["JOBS"] = str(args.jobs)
        env["MTS_MEDIA_WORKERS"] = str(args.mts_media_workers)
        print(f"Building {profile} (mask={PROFILES[profile]})", flush=True)
        with (target / "build.log").open("w") as log:
            subprocess.run(["bash", "build.sh"], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run(["python3", "package.py"], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        if source_digest() != source:
            raise RuntimeError("Source changed while building; refusing to label this artifact")
        if input_digest(dependency_inputs()) != dependency_hash:
            raise RuntimeError("Toolchain/dependency inputs changed while building")
        for filename in ("fogplay_nx.nro", "fogplay_nx-sd.zip", "verification.json"):
            shutil.copyfile(ROOT / "dist" / filename, target / filename)
        verification = json.loads((target / "verification.json").read_text())
        if digest(target / "fogplay_nx.nro") != verification["nro_sha256"] or digest(target / "fogplay_nx-sd.zip") != verification["archive_sha256"]:
            raise RuntimeError("Copied artifact digest mismatch")
        result = dict(verification, profile=profile, feature_mask=PROFILES[profile],
                      mts_media_workers=bool(args.mts_media_workers),
                      mts_catchup_age_guard=bool(PROFILES[profile] & 128) and profile != "p4_legacy",
                      source_sha256=source, dependency_sha256=dependency_hash, base_commit=head)
        (target / "experiment.json").write_text(json.dumps(result, indent=2) + "\n")
        results.append(result)
        (output / "matrix.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"Verified {profile}: {result['nro_sha256']}", flush=True)

if __name__ == "__main__":
    main()
