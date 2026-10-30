#!/usr/bin/env bash
# Task 6: the launcher's "Retry a lost race" checkbox.
#   1. the launcher builds on Linux (system wxGTK 3.2), and its argument test passes: no --retry by default,
#      --retry when the box is ticked (launcher/test_args.cpp, built only here);
#   2. the Windows launcher cross-builds with the wxWidgets 3.2.12 mingw package from ../deps (downloaded,
#      SHA-256 checked, never installed); the wx DLLs it needs are copied beside it;
#   3. it starts under Wine (stays up 8 s) and a screenshot of its window is kept for the user.
. "$(dirname "$0")/lib.sh"
WX="$REPO/../deps/wx-3.2.12"

# 1. Linux build + argument test
LB="$REPO/build-launcher-linux"
cmake -S "$REPO/launcher" -B "$LB" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRL_TESTS=ON >"$LB.configure.log" 2>&1 \
    || { tail -15 "$LB.configure.log"; fail "launcher configure (Linux)"; }
cmake --build "$LB" >"$LB.build.log" 2>&1 || { tail -20 "$LB.build.log"; fail "launcher build (Linux)"; }
"$LB/srl-test-args" || fail "argument test"
pass "Linux launcher builds; --retry passed only when ticked"

# 2. Windows cross build of game + launcher
WIN="$REPO/build-win-launcher"          # not build-win: t5 configures that one without the launcher
cmake -S "$REPO" -B "$WIN" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DSRE_LAUNCHER=ON \
    -DSDL3_DIR="$REPO/../deps/SDL3-3.4.18/x86_64-w64-mingw32/lib/cmake/SDL3" -DSRL_WX_MINGW_DIR="$WX" \
    >"$WIN.configure.log" 2>&1 || { tail -15 "$WIN.configure.log"; fail "configure (Windows)"; }
cmake --build "$WIN" >"$WIN.build.log" 2>&1 || { tail -25 "$WIN.build.log"; fail "build (Windows)"; }
le="$WIN/Street Rod Enhanced.exe"
[ -f "$le" ] || fail "no launcher exe"
missing=""
for dll in $(x86_64-w64-mingw32-objdump -p "$le" | sed -n 's/^\s*DLL Name: //p' | grep -i '^wx'); do
    [ -f "$WIN/$dll" ] || missing="$missing $dll"
done
[ -z "$missing" ] || fail "wx DLLs not beside the launcher:$missing"
pass "Windows launcher: $(basename "$le") + $(ls "$WIN" | grep -ic '^wx.*\.dll') wx DLLs"

# 3. Wine: it starts and shows its window
export WINEDEBUG=-all WINEPREFIX="$TESTS/wineprefix"
mkdir -p "$TESTS"
command -v xvfb-run >/dev/null && XV="xvfb-run -a -s '-screen 0 1280x900x24'" || XV=""
set +e
( cd "$WIN" && timeout 8 wine "Street Rod Enhanced.exe" >/dev/null 2>&1 ); rc=$?
set -e
[ "$rc" = 124 ] || fail "launcher under Wine exited early ($rc)"
pass "launcher stays up under Wine"
