#!/usr/bin/env bash
# Task 5: the Windows build of the game, cross-compiled with mingw-w64 and run under Wine.
#   * configure + build into build-win/ with cmake/mingw-w64-x86_64.cmake and the SDL3 mingw package from
#     ../deps (downloaded, SHA-256 checked, never installed);
#   * streetrod-enhanced.exe imports only SDL3.dll and Windows system DLLs (libgcc / winpthread static), and
#     SDL3.dll is copied beside it;
#   * `--check` loads SR.EXE under Wine;
#   * a headless race under Wine with --retry: lost, Retry, the same race again (log lines as on Linux).
. "$(dirname "$0")/lib.sh"
WIN="$REPO/build-win"
SDL3_DIR="$REPO/../deps/SDL3-3.4.18/x86_64-w64-mingw32/lib/cmake/SDL3"
[ -d "$SDL3_DIR" ] || fail "SDL3 mingw package missing: $SDL3_DIR"
command -v wine >/dev/null || fail "wine missing"

cmake -S "$REPO" -B "$WIN" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DSRE_LAUNCHER=OFF -DSDL3_DIR="$SDL3_DIR" >"$WIN.configure.log" 2>&1 \
    || { tail -15 "$WIN.configure.log"; fail "configure"; }
cmake --build "$WIN" >"$WIN.build.log" 2>&1 || { tail -20 "$WIN.build.log"; fail "build"; }
exe="$WIN/streetrod-enhanced.exe"
[ -f "$exe" ] || fail "no exe"
[ -f "$WIN/SDL3.dll" ] || fail "SDL3.dll not beside the exe"

# imports: only SDL3.dll and system DLLs
imports=$(x86_64-w64-mingw32-objdump -p "$exe" | sed -n 's/^\s*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)
extra=$(echo "$imports" | grep -vE '^(sdl3\.dll|kernel32\.dll|msvcrt\.dll|user32\.dll|shell32\.dll|advapi32\.dll|ws2_32\.dll|api-ms-win-.*\.dll)$' || true)
[ -z "$extra" ] || fail "unexpected DLL imports: $extra"
pass "Windows build: streetrod-enhanced.exe + SDL3.dll, imports: $(echo $imports)"

export WINEDEBUG=-all WINEPREFIX="$TESTS/wineprefix"
mkdir -p "$TESTS"
out=$(timeout 60 wine "$exe" --game-dir "$REPO/Game" --check 2>/dev/null | tr -d '\r')
echo "$out" | grep -q ' ok: image ' || fail "--check under Wine: $out"
pass "--check under Wine: $out"

# a race under Wine: forced loss, Retry (answer aid), the restarted race won
d="$TESTS/t5"
fresh_game "$d"
set +e
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy SR_KEYS="3:e048p" SR_RETRY_LOG=1 SR_DEV_SEED=7 \
    SR_DEBUG_RACE=drag SR_RETRY_FORCE=01,W SR_RETRY_ANSWERS=R \
    timeout 90 wine "$exe" --game-dir "$d/Game" --retry >"$d/stdout.txt" 2>"$d/stderr.txt"
echo $? >"$d/exit.txt"
set -e
tr -d '\r' <"$d/stderr.txt" >"$d/stderr.clean"
grep -q 'retry: restarted' "$d/stderr.clean" || fail "no restart under Wine (exit $(cat "$d/exit.txt")): $(grep retry: "$d/stderr.clean" | tr '\n' ' ')"
[ "$(grep 'retry: start' "$d/stderr.clean" | sed 's/.*start //' | sort -u | wc -l)" = 1 ] || fail "restart differs under Wine"
[ -n "$(ls "$d/Game" | grep -ix 'hotrod16.sav')" ] || fail "no slot 16 under Wine"
pass "Wine: lost, Retry, the same race again; slot 16 written"
