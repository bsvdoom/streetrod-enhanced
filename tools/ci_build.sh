#!/usr/bin/env bash
# Builds everything from source, as the GitHub workflow does (.github/workflows/build.yml), so the release
# can be reproduced locally (e.g. in `docker run -v $PWD:/src -w /src ubuntu:26.04 tools/ci_build.sh all`).
#
#   tools/ci_build.sh deps      third-party downloads into ./deps (SDL3 mingw, wxMSW gcc13.2), SHA-256 checked
#   tools/ci_build.sh linux     game + launcher for Linux             -> dist/linux/
#   tools/ci_build.sh windows   game + launcher for Windows (mingw)   -> dist/windows/
#   tools/ci_build.sh all       deps, linux, windows; each also zipped: dist/streetrod-enhanced-<tag>-<os>.zip
#
# Needs (Debian / Ubuntu 26.04): cmake ninja-build gcc g++ libsdl3-dev libwxgtk3.2-dev
#   g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix curl p7zip-full ca-certificates
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
DEPS=${DEPS:-$ROOT/deps}

# name  sha256  url  -- pinned; the files are checked before use, nothing is installed
SDL3_VER=3.4.18
WX_VER=3.2.12
PINS="
SDL3-devel-$SDL3_VER-mingw.tar.gz 049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VER/SDL3-devel-$SDL3_VER-mingw.tar.gz
wxMSW-${WX_VER}_gcc1320_x64_Dev.7z c4c0fcf38928a58066bb30952d80e8a6eb54cb608b8eaa63b083415e7b7e52a1 https://github.com/wxWidgets/wxWidgets/releases/download/v$WX_VER/wxMSW-${WX_VER}_gcc1320_x64_Dev.7z
wxMSW-${WX_VER}_gcc1320_x64_ReleaseDLL.7z 8aa27f981007097bff4ce010e36ffc661273042a75ec642d9e3acdd277ce37b9 https://github.com/wxWidgets/wxWidgets/releases/download/v$WX_VER/wxMSW-${WX_VER}_gcc1320_x64_ReleaseDLL.7z
wxWidgets-$WX_VER-headers.7z a6fc9aa41e90ca6abd679cd6cb7c42c0f14f8013e47ebff09f15ba3765d181be https://github.com/wxWidgets/wxWidgets/releases/download/v$WX_VER/wxWidgets-$WX_VER-headers.7z
"
SDL3_DIR="$DEPS/SDL3-$SDL3_VER/x86_64-w64-mingw32/lib/cmake/SDL3"
WX_DIR="$DEPS/wx-$WX_VER"

deps() {
    mkdir -p "$DEPS/downloads" "$WX_DIR"
    echo "$PINS" | while read -r name sum url; do
        [ -n "$name" ] || continue
        f="$DEPS/downloads/$name"
        [ -f "$f" ] || curl -fsSL -o "$f" "$url"
        echo "$sum  $f" | sha256sum -c --quiet
    done
    [ -d "$DEPS/SDL3-$SDL3_VER" ] || tar xzf "$DEPS/downloads/SDL3-devel-$SDL3_VER-mingw.tar.gz" -C "$DEPS"
    if [ ! -d "$WX_DIR/lib" ]; then
        for f in "$DEPS"/downloads/wx*.7z; do 7z x -y -bso0 -o"$WX_DIR" "$f"; done
    fi
}

linux() {
    cmake -S . -B build-ci-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRE_LAUNCHER=ON
    cmake --build build-ci-linux
    rm -rf dist/linux && mkdir -p dist/linux
    cp build-ci-linux/streetrod-enhanced "build-ci-linux/Street Rod Enhanced" dist/linux/
}

windows() {
    cmake -S . -B build-ci-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
        -DCMAKE_BUILD_TYPE=Release -DSRE_LAUNCHER=ON -DSDL3_DIR="$SDL3_DIR" -DSRL_WX_MINGW_DIR="$WX_DIR"
    cmake --build build-ci-win
    rm -rf dist/windows && mkdir -p dist/windows
    cp build-ci-win/*.exe build-ci-win/*.dll dist/windows/
    # the licences of what is shipped beside our programs
    mkdir -p dist/windows/licenses
    cp "$DEPS/SDL3-$SDL3_VER/LICENSE.txt" dist/windows/licenses/SDL3-LICENSE.txt
    cp "$WX_DIR/docs/licence.txt" dist/windows/licenses/wxWidgets-licence.txt
    cp /usr/share/doc/gcc-mingw-w64-base/copyright dist/windows/licenses/gcc-runtime-copyright.txt 2>/dev/null || true
}

# package NAME DIR: README, LICENSE, SHA256SUMS.txt, then dist/streetrod-enhanced-<VERSION>-<NAME>.zip
# (VERSION = the git tag, e.g. v0.3.0-retry, or the commit when untagged)
VERSION=${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
package() {
    local name="$1" dir="$2"
    cp README.md LICENSE "$dir/"
    (cd "$dir" && find . -type f ! -name SHA256SUMS.txt -printf '%P\n' | sort | xargs -d '\n' sha256sum > SHA256SUMS.txt)
    # the same timestamp on every file (the commit's time), so the same source gives the same zip
    local stamp="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || echo 315532800)}"
    find "$dir" -exec touch -h -d "@$stamp" {} +
    local zip="dist/streetrod-enhanced-$VERSION-$name.zip"
    rm -f "$zip"
    (cd "$dir" && find . -type f -printf '%P\n' | LC_ALL=C sort > "$ROOT/dist/.list-$name" &&
        7z a -tzip -bso0 -mtc=off "$ROOT/$zip" @"$ROOT/dist/.list-$name")
    rm -f "dist/.list-$name"
    echo "$zip"
}

case "${1:-all}" in
    deps) deps ;;
    linux) linux; package linux dist/linux ;;
    windows) deps; windows; package windows dist/windows ;;
    all) deps; linux; package linux dist/linux; windows; package windows dist/windows ;;
    *) echo "usage: $0 deps|linux|windows|all" >&2; exit 2 ;;
esac
