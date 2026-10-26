#!/usr/bin/env bash
# Private HTTP(S) curl: the devkitPro curl uses libnx TLS, which accepts P12 only.
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
# Share the application's exact bundled mbedTLS ABI and configuration. Portlibs'
# mbedTLS 2.28 cannot coexist under the same symbols as RTC's mbedTLS 3.6.
VERSION=8.15.0
SHA256=6cd0a8a5b126ddfda61c94dc2c3fc53481ba7a35461cf7c5ab66aa9d6775b609
URL="https://curl.se/download/curl-$VERSION.tar.xz"
WORK="$ROOT/work/moonlight"
ARCHIVE="$WORK/downloads/curl-$VERSION.tar.xz"
SOURCE="$WORK/curl-$VERSION"
BUILD="$WORK/build-curl-switch"
PREFIX="$ROOT/third_party/moonlight/switch"
PORTLIBS="$DEVKITPRO/portlibs/switch"
TLS_PREFIX="$ROOT/third_party/rtc/switch"
test -f "$TLS_PREFIX/lib/libmbedcrypto.a"
mkdir -p "$WORK/downloads" "$PREFIX/lib" "$PREFIX/include/curl"
if [ ! -f "$ARCHIVE" ]; then
  curl --fail --location --proto '=https' --tlsv1.2 "$URL" -o "$ARCHIVE.tmp"
  mv "$ARCHIVE.tmp" "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum --check --status
# Extract verified sources afresh so local edits cannot enter the dependency.
if [ -d "$SOURCE" ]; then
  test "$(realpath "$SOURCE")" = "$WORK/curl-$VERSION"
  rm -rf -- "$SOURCE"
fi
tar -xJf "$ARCHIVE" -C "$WORK"
# newlib declares basename/socketpair without usable Switch implementations.
# Keep curl's basename fallback and disable its unused socketpair wakeup path.
cmake -S "$SOURCE" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-fPIE -DMBEDTLS_USER_CONFIG_FILE=\\\"$ROOT/tools/rtc/mbedtls-user-config.h\\\"" \
  '-DCMAKE_C_ARCHIVE_CREATE=<CMAKE_AR> qcD <TARGET> <LINK_FLAGS> <OBJECTS>' \
  '-DCMAKE_C_ARCHIVE_APPEND=<CMAKE_AR> qD <TARGET> <LINK_FLAGS> <OBJECTS>' \
  '-DCMAKE_C_ARCHIVE_FINISH=<CMAKE_RANLIB> -D <TARGET>' \
  -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
  -DENABLE_CURL_MANUAL=OFF -DCURL_DISABLE_INSTALL=ON \
  -DCURL_ENABLE_EXPORT_TARGET=OFF -DCURL_USE_PKGCONFIG=OFF \
  -DHTTP_ONLY=ON -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
  -DMBEDTLS_INCLUDE_DIR="$TLS_PREFIX/include" \
  -DMBEDTLS_LIBRARY="$TLS_PREFIX/lib/libmbedtls.a" \
  -DMBEDX509_LIBRARY="$TLS_PREFIX/lib/libmbedx509.a" \
  -DMBEDCRYPTO_LIBRARY="$TLS_PREFIX/lib/libmbedcrypto.a" \
  -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
  -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
  -DENABLE_THREADED_RESOLVER=OFF -DENABLE_ARES=OFF -DENABLE_UNIX_SOCKETS=OFF \
  -DCURL_DISABLE_SOCKETPAIR=ON -DHAVE_SOCKETPAIR=OFF -DCURL_DISABLE_PROXY=ON \
  -DCURL_DISABLE_NETRC=ON -DCURL_DISABLE_COOKIES=ON \
  -DHAVE_BASENAME=OFF \
  -DCURL_DISABLE_ALTSVC=ON -DCURL_DISABLE_HSTS=ON \
  -DCURL_DISABLE_DOH=ON -DCURL_DISABLE_WEBSOCKETS=ON \
  -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none
cmake --build "$BUILD" --target libcurl_static -j "${JOBS:-8}"
python3 "$ROOT/tools/moonlight/namespace-curl.py" "$BUILD/lib/libcurl.a" \
  --create-map --map "$PREFIX/curl-symbols.map" \
  --output "$PREFIX/lib/libmoonlight-curl.a" \
  --nm "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" \
  --objcopy "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-objcopy" \
  --ranlib "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ranlib"
cp "$SOURCE/include/curl/"*.h "$PREFIX/include/curl/"
cp "$SOURCE/COPYING" "$ROOT/licenses/moonlight/curl-COPYING"
python3 - "$PREFIX" "$URL" "$SHA256" "$BUILD/lib/curl_config.h" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

prefix, url, source_sha, config = sys.argv[1:]
path = Path(prefix)
config_text = Path(config).read_text()
if "#define USE_MBEDTLS 1" not in config_text:
    raise SystemExit("Private curl is missing the required mbedTLS backend")
if any(f"#define {backend} 1" in config_text for backend in (
    "USE_OPENSSL", "USE_SCHANNEL", "USE_GNUTLS", "USE_WOLFSSL", "USE_RUSTLS"
)):
    raise SystemExit("Private curl unexpectedly contains another TLS backend")
metadata = {
    "url": url,
    "archive_sha256": source_sha,
    "tls_backend": "mbedTLS",
    "protocols": ["http", "https"],
    "symbol_prefix": "fog_moonlight_",
    "symbol_map_sha256": hashlib.sha256((path / "curl-symbols.map").read_bytes()).hexdigest(),
    "config_sha256": hashlib.sha256(Path(config).read_bytes()).hexdigest(),
    "library_sha256": hashlib.sha256((path / "lib/libmoonlight-curl.a").read_bytes()).hexdigest(),
}
(path / "curl-build.json").write_text(json.dumps(metadata, indent=2) + "\n")
PY
printf 'Private curl %s (mbedTLS, HTTP/HTTPS) built: %s/lib/libmoonlight-curl.a\n' "$VERSION" "$PREFIX"
