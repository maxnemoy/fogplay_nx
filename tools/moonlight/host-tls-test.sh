#!/usr/bin/env bash
set -Eeuo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
ROOT=$PWD
WORK="$ROOT/work/moonlight/host-tls"
SOURCE="$WORK/curl-8.15.0"
MBED="$ROOT/third_party/rtc/host"
mkdir -p "$WORK" work/logs
test -f "$MBED/lib/libmbedcrypto.a"
ARCHIVE="$ROOT/work/moonlight/downloads/curl-8.15.0.tar.xz"
printf '%s  %s\n' 6cd0a8a5b126ddfda61c94dc2c3fc53481ba7a35461cf7c5ab66aa9d6775b609 "$ARCHIVE" | sha256sum --check --status
if [[ ! -d "$SOURCE" ]]; then tar -xJf "$ARCHIVE" -C "$WORK"; fi
cmake -S "$SOURCE" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-DMBEDTLS_USER_CONFIG_FILE=\\\"$ROOT/tools/rtc/mbedtls-user-config.h\\\"" \
  -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
  -DENABLE_CURL_MANUAL=OFF -DCURL_DISABLE_INSTALL=ON -DCURL_USE_PKGCONFIG=OFF \
  -DHTTP_ONLY=ON -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
  -DMBEDTLS_INCLUDE_DIR="$MBED/include" -DMBEDTLS_LIBRARY="$MBED/lib/libmbedtls.a" \
  -DMBEDX509_LIBRARY="$MBED/lib/libmbedx509.a" -DMBEDCRYPTO_LIBRARY="$MBED/lib/libmbedcrypto.a" \
  -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF \
  -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
  -DCURL_DISABLE_PROXY=ON -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none \
  > "$WORK/configure.log" 2>&1
cmake --build "$WORK/build" --target libcurl_static -j 8 > "$WORK/build.log" 2>&1
g++ -std=c++17 -g -Wall -Wextra -Werror -Wno-sign-compare -Wno-unused-but-set-variable -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -DUSE_MBEDTLS_CRYPTO=1 \
  -DMBEDTLS_USER_CONFIG_FILE="\"$ROOT/tools/rtc/mbedtls-user-config.h\"" \
  -Itools/moonlight/tests/stubs -Itools/moonlight/compat \
  -Ithird_party/moonlight/prepared/libgamestream -Ithird_party/moonlight/switch/include \
  -Ithird_party/moonlight/prepared/crypto -I"$SOURCE/include" -I"$MBED/include" \
  third_party/moonlight/prepared/libgamestream/http.cpp third_party/moonlight/prepared/crypto/Data.cpp \
  third_party/moonlight/prepared/crypto/MbedTLSCryptoManager.cpp tools/moonlight/tests/tls_client.cpp \
  -Wl,--start-group "$WORK/build/lib/libcurl.a" "$MBED/lib/libmbedtls.a" \
  "$MBED/lib/libmbedx509.a" "$MBED/lib/libmbedcrypto.a" -Wl,--end-group \
  -o "$WORK/client"
python3 tools/moonlight/tests/tls_test.py "$WORK/client" 2>&1 | tee work/logs/moonlight-host-tls-test.log
