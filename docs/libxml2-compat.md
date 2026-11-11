# FogHub compatibility with the Switch libxml2 package

`bash tools/test/foghub-libxml-compat.sh` is an obligatory, offline-by-default
part of `tools/test/all.sh`. It compiles and runs the real catalog/PC-preview,
rental/history/pagination, repository, and review test programs twice: against
the pinned static 2.9.8 library and the current host library. Existing negative
fixtures run in both variants. Any dependency, verification, compilation, timeout,
or test failure exits nonzero; no system-library fallback or silent skip exists.
This is host parser compatibility evidence, not Switch runtime acceptance.

## Prepare once

Use Linux/WSL with Python >= 3.12 (safe tar extraction), gcc/g++, make,
pkg-config, zlib development files, and host libxml2/libcurl development files.
No package is installed by these scripts. An unavailable prerequisite produces
a failure; provision it using your normal development environment procedure.
Autotools is not required: use the release tarball, not the GitHub tag archive.

From the repository root:

```sh
# Explicit network access only in this setup command:
python3 tools/libxml2-compat/compat.py setup --download
# Or prepare entirely offline using the exact release archive:
python3 tools/libxml2-compat/compat.py setup --archive /path/libxml2-2.9.8.tar.gz
bash tools/test/foghub-libxml-compat.sh
```

The source SHA-256 is
`0b74e51595654f958148759cfef0993114ddccccbb6f31aee018f3558e8e2732`,
identical to the devkitPro recipe. Setup fetches a mirror of that exact release
from [Buildroot](https://sources.buildroot.net/libxml2/libxml2-2.9.8.tar.gz),
verifies it before extraction, and builds only inside
`work/libxml2-compat/2.9.8-4/`. The original upstream recipe URL uses HTTP;
the mirror uses HTTPS and the pinned digest is authoritative.

`--cache-dir /absolute/path` works with setup, verify, and the test wrapper.
A valid prepared cache is reused after integrity and feature checks.
An invalid or outdated cache is rejected: prepare a fresh directory and pass
that same path to subsequent runs. Setup never overwrites an unverified install.
The cache manifest pins setup inputs and every installed file hash. Test runs
recheck those hashes, public feature macros, header/runtime version, and all
33 `xmlHasFeature` values. Linking names the static archive by absolute path,
so pkg-config cannot silently select a newer libxml2 for the target variant.
`python3 tools/libxml2-compat/compat.py verify` checks preparation alone.

Every invocation creates a unique `work/logs/v2-libxml-{setup,verify,run}-*/`
directory for configure/build logs, feature results, and each parser's build/run
output. Setup also saves `config.h` and `config.log`; the manifest records the
compiler and actual configure options. The run records the current host version.
No setup or run reads console state, cookies, credentials, or saved pages by default.

## Exact recipe and the limits of equivalence

The vendored `tools/libxml2-compat/devkitpro-PKGBUILD` is the unmodified
[devkitPro recipe at commit 7b8af4560ea209bc603ed994caa37ad08969110b](https://github.com/devkitPro/pacman-packages/blob/7b8af4560ea209bc603ed994caa37ad08969110b/switch/libxml2/PKGBUILD),
SHA-256 `90c6b1bdf9658357f212538ff60bcec5e09123150fd554353c27feb39dffca8c`.
The installed target package checked on 2026-11-04 was `switch-libxml2-2.9.8-4`.
Its public header checksum and enabled feature macros are captured in
`target-profile.json`. No developer's /tmp build is accepted as this profile.

The recipe's only source adjustment is deleting `^bin_PROGRAMS` lines from
`Makefile.in` to omit xmllint/xmlcatalog. Setup reproduces this exact adjustment;
there are no parser patches. Both use static libraries, disabled maintainer mode,
no rebuilt docs/IPv6/ICU/Python/LZMA. The recipe enables default features; the
installed Switch profile has threads, HTML, XPath, FTP, HTTP, zlib, schemas,
legacy APIs and the other recorded defaults enabled, with iconv, modules and
ICU/LZMA disabled. Setup explicitly fixes host iconv/modules/zlib/thread/FTP/HTTP
choices to these actual target values, preventing autodetection drift.

Host differences are deliberate and do not establish binary equivalence:

- Native Linux architecture, compiler and glibc replace aarch64-none-elf,
  devkitA64 and newlib/libnx. `--host` and the Switch SDK's prefix, architecture,
  PIC/TLS flags, `__SWITCH__`, `__LINUX_ERRNO_EXTENSIONS__`, and `-lnx` are omitted.
- Native CFLAGS are `-O2 -std=gnu99`; the language standard keeps the historical
  source buildable with GCC 15. Switch uses SDK architecture flags, `-O2`,
  function/data sections and platform include/link paths.
- Host zlib/pthreads/libm and host libcurl replace portlibs dependencies. Host
  platform capability detection in private `config.h` differs from newlib.
  Public parser features and all runtime feature flags must still match.
- Installed documentation cleanup from the package recipe is irrelevant to
  parser behavior and is omitted. Installation remains isolated in the cache.

For historical syntax, the pinned release's `configure`/source take precedence
over [current upstream build documentation](https://gitlab.gnome.org/GNOME/libxml2/-/blob/master/README.md).
Do not call this full target-equivalent execution: parser source, recipe
adjustment, versions and exposed features match; OS/compiler/ABI/runtime do not.
Switch catalog visibility and a real rental/stop remain hardware gates.

## Optional sanitized saved pages

Only explicitly selected, already sanitized files may be supplied. Never point
this at a console dump, browser profile, auth store, or unsanitized HTTP capture.
The suite performs no HTTP requests. A TSV manifest has four fields per line:
`kind`, `expected-id`, `accept|reject`, `absolute-html-path`. Blank lines and `#`
comments are ignored. Supported kinds: `games`, `computers`, `preview`, `history`,
`connect`, `review`. Use `-` for IDs unused by games/history. Example (tabs):

```text
preview	1838	accept	/absolute/path/sanitized-preview.html
history	-	reject	/absolute/path/sanitized-malformed-history.html
```

```sh
bash tools/test/foghub-libxml-compat.sh --saved-pages /absolute/path/pages.tsv
```

Every page runs through the production parser on both libraries. The runner
reports only ordinal/kind and accept/reject, never page content, identity,
endpoints or exception messages. Missing files, unknown kinds, invalid or empty
manifests and unexpected accept/reject results fail. Relative page paths are
relative to the repository root; prefer absolute paths. The optional manifest
supplements the mandatory in-repository regression suites.

## Setup/verification regression tests

```sh
python3 -m unittest discover -s tools/libxml2-compat -p 'test_*.py'
```

These tests cover offline setup, archive/recipe checksum failure, missing tools,
and corrupted prepared dependencies. They do not download or build libxml2.