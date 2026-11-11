#!/usr/bin/env python3
"""Offline-by-default, pinned devkitPro libxml2 parser compatibility gate."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SOURCE_SHA256 = "0b74e51595654f958148759cfef0993114ddccccbb6f31aee018f3558e8e2732"
RECIPE_SHA256 = "90c6b1bdf9658357f212538ff60bcec5e09123150fd554353c27feb39dffca8c"
SOURCE_URL = "https://sources.buildroot.net/libxml2/libxml2-2.9.8.tar.gz"
OPTIONS = ["--disable-shared", "--enable-static", "--disable-maintainer-mode",
           "--enable-rebuild-docs=no", "--enable-ipv6=no", "--without-icu",
           "--without-python", "--without-lzma", "--without-iconv", "--without-modules",
           "--with-zlib", "--with-threads", "--with-http", "--with-ftp"]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(*names):
    for name in names:
        if shutil.which(name) is None:
            raise RuntimeError(f"Missing required tool: {name}; see docs/libxml2-compat.md")


def run(command, log=None, **kwargs):
    if log:
        with open(log, "w") as output:
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, **kwargs)
        if result.returncode:
            raise RuntimeError(f"Command failed ({result.returncode}): {command[0]}; see {log}")
    else:
        subprocess.run(command, check=True, **kwargs)


def pkg_flags(*options):
    # pkg-config quotes/escapes flags according to shell syntax.
    import shlex
    return shlex.split(subprocess.check_output(["pkg-config", *options], text=True))


def profile_macros(include):
    result = subprocess.check_output(["g++", "-pthread", "-E", "-dM", "-x", "c++",
                                      f"-I{include}", "-include", "libxml/xmlversion.h", "/dev/null"], text=True)
    return sorted(line.split()[1] for line in result.splitlines()
                  if line.startswith("#define LIBXML_") and line.split()[1].endswith("_ENABLED"))


def recipe_check():
    if digest(HERE / "devkitpro-PKGBUILD") != RECIPE_SHA256:
        raise RuntimeError("Pinned devkitPro recipe checksum mismatch")


def install_hashes(prefix):
    return {str(p.relative_to(prefix)): digest(p) for p in sorted(prefix.rglob("*")) if p.is_file()}


def fingerprint():
    return {p.name: digest(p) for p in (HERE / "compat.py", HERE / "features.cpp", HERE / "target-profile.json")}


def verify(cache, output):
    recipe_check()
    manifest_path = cache / "manifest.json"
    if not manifest_path.is_file():
        raise RuntimeError("Prepared dependency missing. Run: python3 tools/libxml2-compat/compat.py setup --download\n"
                           "Or use setup --archive /path/libxml2-2.9.8.tar.gz (offline). See docs/libxml2-compat.md")
    manifest = json.loads(manifest_path.read_text())
    if manifest["inputs"] != fingerprint() or manifest["source_sha256"] != SOURCE_SHA256:
        raise RuntimeError("Prepared dependency recipe changed; prepare a fresh --cache-dir")
    prefix = cache / "install"
    if not (prefix / "lib/libxml2.a").is_file() or install_hashes(prefix) != manifest["installed_sha256"]:
        raise RuntimeError("Prepared dependency integrity check failed; prepare a fresh --cache-dir")
    profile = json.loads((HERE / "target-profile.json").read_text())
    if profile_macros(prefix / "include/libxml2") != profile["enabled_macros"]:
        raise RuntimeError("Prepared dependency features differ from captured Switch header")
    run(["g++", "-std=c++17", "-pthread", f"-I{prefix}/include/libxml2", str(HERE / "features.cpp"),
         str(prefix / "lib/libxml2.a"), "-lz", "-lm", "-o", str(output / "features")], output / "features.build.log")
    result = subprocess.check_output([str(output / "features")], text=True)
    (output / "features.log").write_text(result)
    expected = "version=2.9.8 runtime=20908\n" + "".join(f"feature.{i}={int(i not in (19,27,29,30,32,33))}\n" for i in range(1,34))
    if result != expected:
        raise RuntimeError(f"Runtime libxml2 version/features mismatch; see {output / 'features.log'}")
    print("PASS dependency: 2.9.8, 33 runtime features and Switch public feature macros verified", flush=True)
    return prefix


def setup(args, cache, output):
    recipe_check()
    if (cache / "manifest.json").exists():
        verify(cache, output)
        return
    require("gcc", "g++", "make", "pkg-config", "sed")
    run(["pkg-config", "--exists", "zlib"])
    archive = args.archive
    if archive is None:
        if not args.download:
            raise RuntimeError("Setup needs --archive PATH or explicit --download; no automatic network access")
        archive = cache / "libxml2-2.9.8.tar.gz"
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as source, open(archive, "wb") as dest:
            shutil.copyfileobj(source, dest)
    if digest(archive) != SOURCE_SHA256:
        raise RuntimeError("Source archive checksum mismatch (requires exact devkitPro release tar.gz)")
    prefix = cache / "install"
    if prefix.exists():
        raise RuntimeError("Unverified install already exists; prepare a fresh --cache-dir")
    build = Path(tempfile.mkdtemp(prefix="build-", dir=cache))
    with tarfile.open(archive) as source:
        source.extractall(build, filter="data")
    source_dir = build / "libxml2-2.9.8"
    # Exact source adjustment from devkitPro's recipe; no parser source patches.
    makefile = source_dir / "Makefile.in"
    makefile.write_text("".join(line for line in makefile.read_text().splitlines(keepends=True)
                              if not line.startswith("bin_PROGRAMS")))
    env = os.environ.copy()
    # Do not inherit Switch/toolchain or user library selection flags.
    for key in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBS", "CONFIG_SITE", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR"):
        env.pop(key, None)
    env.update(CC="gcc", CXX="g++", CFLAGS="-O2 -std=gnu99", CONFIG_SITE="/dev/null")
    run(["./configure", f"--prefix={prefix}", *OPTIONS], output / "configure.log", cwd=source_dir, env=env)
    run(["make", f"-j{args.jobs}"], output / "make.log", cwd=source_dir, env=env)
    run(["make", "install"], output / "install.log", cwd=source_dir, env=env)
    shutil.copy2(source_dir / "config.h", output / "config.h")
    shutil.copy2(source_dir / "config.log", output / "config.log")
    (cache / "manifest.json").write_text(json.dumps({"source_sha256": SOURCE_SHA256, "recipe_sha256": RECIPE_SHA256,
        "inputs": fingerprint(), "configure": OPTIONS, "host_cflags": env["CFLAGS"],
        "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
        "installed_sha256": install_hashes(prefix)}, indent=2) + "\n")
    verify(cache, output)


def suites(prefix, output, saved_pages):
    require("pkg-config", "g++", "timeout")
    run(["pkg-config", "--exists", "libcurl", "libxml-2.0"])
    curl_cflags, curl_libs = pkg_flags("--cflags", "libcurl"), pkg_flags("--libs", "libcurl")
    host_flags = pkg_flags("--cflags", "libxml-2.0")
    host_libs = pkg_flags("--libs", "libxml-2.0")
    production = "src/features/providers/infrastructure/"
    html = production + "foghub_html.cpp"
    review = production + "foghub_review_html.cpp"
    repo = [production + "foghub_" + name + ".cpp" for name in ("repositories", "transport")]
    repo += ["src/features/auth/infrastructure/foghub_cookie_storage.cpp", "src/core/storage/storage.cpp"]
    tests = {"catalog": [html, "tests/foghub_html_tests.cpp"],
             "rental-html": [html, "tests/foghub_rental_html_tests.cpp"],
             "repository": [*repo, html, "tests/foghub_repository_tests.cpp"],
             "review": [*repo, html, review, production + "foghub_rental_repository.cpp", "tests/foghub_review_tests.cpp"]}
    if saved_pages:
        if not saved_pages.is_file():
            raise RuntimeError("Explicit sanitized saved-page manifest does not exist")
        tests["saved-pages"] = [html, review, str(HERE / "saved_pages.cpp")]
    for variant, xml_flags, xml_libs in (
            ("target-2.9.8", [f"-I{prefix}/include/libxml2"], [str(prefix / "lib/libxml2.a"), "-lz", "-lm"]),
            ("host", host_flags, host_libs)):
        for name, sources in tests.items():
            binary = output / f"{variant}-{name}"
            run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-Isrc", "-Ithird_party",
                 *xml_flags, *curl_cflags, *sources, *xml_libs, *curl_libs, "-o", str(binary)], str(binary) + ".build.log", cwd=REPO)
            command = ["timeout", "60", str(binary)]
            if name == "saved-pages":
                command.append(str(saved_pages))
            run(command, str(binary) + ".log", cwd=REPO)
            print(f"PASS {variant} {name}; {binary}.log", flush=True)
    (output / "host-version.log").write_text(subprocess.check_output(["pkg-config", "--modversion", "libxml-2.0"], text=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("setup", "verify", "run"))
    parser.add_argument("--cache-dir", type=Path, default=REPO / "work/libxml2-compat/2.9.8-4")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--saved-pages", type=Path, help="Explicit sanitized TSV manifest; see docs")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    cache = args.cache_dir.resolve()
    logs = REPO / "work/logs"
    logs.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="v2-libxml-" + args.command + "-", dir=logs))
    print(f"libxml2 compatibility logs: {output}", flush=True)
    try:
        require("g++")
        if args.command == "setup":
            cache.mkdir(parents=True, exist_ok=True)
            setup(args, cache, output)
        else:
            prefix = verify(cache, output)
            if args.command == "run":
                suites(prefix, output, args.saved_pages.resolve() if args.saved_pages else None)
    except (OSError, RuntimeError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"FAIL libxml2 compatibility: {error}", file=sys.stderr)
        return 1
    print("PASS libxml2 compatibility " + args.command, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())