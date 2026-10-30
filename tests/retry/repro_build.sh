#!/usr/bin/env bash
# One clean container build of the current working tree's git files into ./build-repro/<name>/ (new folder).
# usage: tests/retry/repro_build.sh <name>     (name: a new folder name, e.g. r1)
set -euo pipefail
cd "$(dirname "$0")/../.."
name="$1"
out="build-repro/$name"
[ ! -e "$out" ] || { echo "$out exists; pick a new name" >&2; exit 1; }
mkdir -p "$out"
git ls-files -co --exclude-standard | tar cf - -T - | tar xf - -C "$out"
cp -r .git "$out/.git"
docker run --rm -v "$PWD/$out:/src" -w /src \
    ubuntu@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c bash -c '
set -e
apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends git ca-certificates curl \
  p7zip-full cmake ninja-build gcc g++ libsdl3-dev libwxgtk3.2-dev g++-mingw-w64-x86-64-posix \
  gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 >/dev/null 2>&1
git config --global --add safe.directory /src
tools/ci_build.sh all > /src/ci.log 2>&1 || { tail -30 /src/ci.log; exit 1; }
grep "gcc runtime DLLs" /src/ci.log; echo "warnings: $(grep -c "warning:" /src/ci.log || true)"
chown -R '"$(id -u):$(id -g)"' /src'
sha256sum "$out"/dist/*.zip
