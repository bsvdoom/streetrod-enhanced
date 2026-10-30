#!/usr/bin/env bash
# Task 3: the Retry core (src/game/retry.c) through the developer aid SR_DEBUG_RETRY=<case>:
#   roundtrip   new game + stock car, silent save to the slot, money += 1234, check + apply:
#               the money is back, and the saved games 1..15 and hall_dat are untouched
#   save        the silent save only (writes the slot, exit 0)
#   check       retry_check on the slot as it is; prints a DGROUP hash before and after
#   classify    retry_classify over a table of race results; prints one "ok"/"bad" line per row
# Exit 0 = the case passed in the game's own view; the script checks the details.
. "$(dirname "$0")/lib.sh"
build_linux || fail "build"
sav() { local f; f=$(ls "$1" | grep -ix "hotrod$2.sav" | head -1); echo "$1/${f:-HOTROD$2.SAV}"; }

d="$TESTS/t3"

# roundtrip, default slot 16
fresh_game "$d"; before=$(files_hash "$d")
SR_DEV_SEED=1 SR_DEBUG_RETRY=roundtrip run_headless "$d" 20 "" --retry
[ "$(cat "$d/exit.txt")" = 0 ] || fail "roundtrip: exit $(cat "$d/exit.txt"): $(tail -3 "$d/stderr.txt")"
grep -q 'retry: saved slot 16 ok' "$d/stderr.txt" || fail "roundtrip: no save log"
grep -q 'retry: check ok' "$d/stderr.txt" || fail "roundtrip: no check log"
grep -q 'retry: applied' "$d/stderr.txt" || fail "roundtrip: no apply log"
[ "$(files_hash "$d")" = "$before" ] || fail "roundtrip: saved games 1..15 or hall_dat changed"
pass "roundtrip (slot 16), saved games untouched"

# --retry-slot 3 writes slot 3; out-of-range slots are clamped to 1..16
fresh_game "$d"
SR_DEV_SEED=1 SR_DEBUG_RETRY=save run_headless "$d" 20 "" --retry --retry-slot 3
grep -q 'retry: saved slot 3 ok' "$d/stderr.txt" || fail "slot 3: $(tail -2 "$d/stderr.txt")"
grep -q 'warning: --retry-slot 3: saved game 3 is overwritten' "$d/stderr.txt" || fail "no warning for a visible slot"
SR_DEV_SEED=1 SR_DEBUG_RETRY=save run_headless "$d" 20 "" --retry --retry-slot 99
grep -q 'retry: saved slot 16 ok' "$d/stderr.txt" || fail "slot 99 not clamped to 16"
pass "--retry-slot"

# truncated file: check fails, DGROUP untouched
fresh_game "$d"
SR_DEV_SEED=1 SR_DEBUG_RETRY=save run_headless "$d" 20 "" --retry
f=$(sav "$d/Game" 16); truncate -s -1 "$f"
SR_DEV_SEED=1 SR_DEBUG_RETRY=check run_headless "$d" 20 "" --retry
grep -q 'retry: check failed' "$d/stderr.txt" || fail "truncated: not refused"
grep -q 'dgroup unchanged' "$d/stderr.txt" || fail "truncated: DGROUP changed"
# a longer file is refused too, and a missing one
printf 'xx' >>"$f"
SR_DEV_SEED=1 SR_DEBUG_RETRY=check run_headless "$d" 20 "" --retry
grep -q 'retry: check failed' "$d/stderr.txt" || fail "longer file: not refused"
rm -f "$f"
SR_DEV_SEED=1 SR_DEBUG_RETRY=check run_headless "$d" 20 "" --retry
grep -q 'retry: check failed' "$d/stderr.txt" || fail "missing file: not refused"
# a part chain longer than the pool (140 parts) is refused: header says spare parts, 141 linked records
python3 - "$f" <<'PY'
import struct, sys
hdr = bytearray(0x24); hdr[0:4] = b'TEST'; struct.pack_into('<H', hdr, 0x1E, 1)
parts = b''.join(struct.pack('<HHHH', 0, 0, 0, 1 if i < 140 else 0) for i in range(141))
tail = bytes(8 + 0x18C + 4 + 4 + 2)
open(sys.argv[1], 'wb').write(bytes(hdr) + parts + tail)
PY
SR_DEV_SEED=1 SR_DEBUG_RETRY=check run_headless "$d" 20 "" --retry
grep -q 'retry: check failed' "$d/stderr.txt" || fail "141 parts: not refused"
pass "bad slot files refused without touching the game"

# every original save passes the check (vanilla format accepted)
for n in $(seq 1 15); do
    cp "$(sav "$REPO/Game" "$n")" "$d/Game/HOTROD16.SAV"
    SR_DEV_SEED=1 SR_DEBUG_RETRY=check run_headless "$d" 20 "" --retry
    grep -q 'retry: check ok' "$d/stderr.txt" || fail "original slot $n refused"
done
pass "all 15 original saves pass the check"

# slot 16 in the vanilla format: loads through the normal loader when copied to slot 1
fresh_game "$d"
SR_DEV_SEED=1 SR_DEBUG_RETRY=save run_headless "$d" 20 "" --retry
cp "$(sav "$d/Game" 16)" "$(sav "$d/Game" 1)"
SR_DEV_SEED=1 SR_DEBUG_SAVE=1:2 run_headless "$d" 20 ""
[ "$(cat "$d/exit.txt")" = 0 ] || fail "vanilla: slot 16 copy does not load as slot 1"
pass "slot 16 is a vanilla save"

# outcome table
fresh_game "$d"
SR_DEV_SEED=1 SR_DEBUG_RETRY=classify run_headless "$d" 20 "" --retry
grep -q ' bad ' "$d/stderr.txt" && { grep ' bad ' "$d/stderr.txt"; fail "classify"; }
[ "$(grep -c 'classify .* ok' "$d/stderr.txt")" -ge 10 ] || fail "classify: too few rows"
pass "outcome classification"

# without --retry nothing is saved
fresh_game "$d"
s16() { ls "$d/Game" | grep -ix 'hotrod16.sav' | while read -r f; do sha256sum "$d/Game/$f"; done; }
s16_before=$(s16)
SR_DEV_SEED=1 SR_DEBUG_RETRY=save run_headless "$d" 20 ""
grep -q 'retry: saved' "$d/stderr.txt" && fail "saved without --retry"
[ "$(s16)" = "$s16_before" ] || fail "slot 16 written without --retry"
# demo forces it off
SR_DEV_SEED=1 SR_DEBUG_RETRY=save run_headless "$d" 20 "" --retry demo
grep -q 'retry: saved' "$d/stderr.txt" && fail "saved in demo mode"
pass "off without --retry and in demo mode"
