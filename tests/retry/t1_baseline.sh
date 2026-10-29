#!/usr/bin/env bash
# Task 1: build, then check that the unmodified game runs headless: the self-playing demo, and a drag race
# through the developer aid SR_DEBUG_RACE (which exits by itself after race_results). Keeps the binary as
# build-linux/ref-streetrod-enhanced, the reference for Task 2's byte-for-byte save comparison.
#
# Snapshot hashes are NOT compared between runs: the game seeds its random numbers from the time of day
# (rnd(-1)) and snapshots are taken on wall-clock time, so two runs of the same build differ.
. "$(dirname "$0")/lib.sh"

build_linux || fail "build"
mkdir -p "$TESTS"

d="$TESTS/baseline-demo"
fresh_game "$d"
run_headless "$d" 15 "" demo
[ "$(cat "$d/exit.txt")" = 124 ] || fail "demo ended early (exit $(cat "$d/exit.txt"))"
n=$(snap_count "$d"); [ "$n" -ge 3 ] || fail "demo: only $n snapshots"

d="$TESTS/baseline-drag"
fresh_game "$d"
before=$(files_hash "$d")
SR_DEBUG_RACE=drag run_headless "$d" 30 "3:e048p"
[ "$(cat "$d/exit.txt")" = 0 ] || fail "drag race did not finish (exit $(cat "$d/exit.txt"))"
n=$(snap_count "$d"); [ "$n" -ge 3 ] || fail "drag: only $n snapshots"
[ "$(files_hash "$d")" = "$before" ] || fail "drag race changed the saved games"
grep -q 'retry:' "$d/stderr.txt" && fail "retry log lines without --retry"

[ -f "$BUILD/ref-streetrod-enhanced" ] || cp "$EXE" "$BUILD/ref-streetrod-enhanced"
pass "demo and drag race run headless; reference binary kept"
