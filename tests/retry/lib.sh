#!/usr/bin/env bash
# Shared helpers for the Retry tests (docs/superpowers/plans/2026-10-29-retry.md).
# Source from a test:  . "$(dirname "$0")/lib.sh"
# Every run is headless (SDL dummy drivers) on a copy of Game/, so the real saved games are never touched.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$REPO/build-linux"
TESTS="$BUILD/tests"
EXE="$BUILD/streetrod-enhanced"

build_linux() {
    cmake -S "$REPO" -B "$BUILD" -G Ninja -DSRE_LAUNCHER=OFF -DCMAKE_BUILD_TYPE=Release >"$BUILD.configure.log" 2>&1 \
        || { tail -20 "$BUILD.configure.log"; return 1; }
    cmake --build "$BUILD" >"$BUILD.build.log" 2>&1 || { tail -30 "$BUILD.build.log"; return 1; }
}

# fresh_game DIR: DIR/Game = a copy of the repository's Game/ (DIR is emptied first).
fresh_game() {
    local dir="$1"
    case "$dir" in "$TESTS"/*) ;; *) echo "fresh_game: $dir is outside $TESTS" >&2; return 1 ;; esac
    rm -rf -- "$dir"
    mkdir -p "$dir/snap"
    cp -a "$REPO/Game" "$dir/Game"
}

# run_headless DIR SECONDS KEYS [ARGS...]: runs the game on DIR/Game for SECONDS, then stops it.
# Extra environment for the game is passed through (e.g. SR_DEBUG_RACE=drag run_headless ...).
# Exit code of the game in DIR/exit.txt (124 = stopped by the timeout), stderr in DIR/stderr.txt.
run_headless() {
    local dir="$1" secs="$2" keys="$3"; shift 3
    local exe="${RUN_EXE:-$EXE}"
    set +e
    SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy SR_SNAPSHOT_DIR="$dir/snap" SR_KEYS="$keys" SR_RETRY_LOG=1 \
        timeout "$secs" "$exe" --game-dir "$dir/Game" "$@" >"$dir/stdout.txt" 2>"$dir/stderr.txt"
    echo $? >"$dir/exit.txt"
    set -e
}

# snap_hash DIR: one sha256 over all snapshots, in order.
snap_hash() {
    local dir="$1"
    (cd "$dir/snap" && ls | sort | xargs -r cat) | sha256sum | cut -c1-64
}

snap_count() { ls "$1/snap" | wc -l; }

# files_hash DIR: sha256 of the saved games 1..15 and hall_dat in DIR/Game.
files_hash() {
    local dir="$1"
    (cd "$dir/Game" && for f in hotrod{1..15}.sav hall_dat; do [ -f "$f" ] && sha256sum "$f"; done) | sha256sum | cut -c1-64
}

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }
