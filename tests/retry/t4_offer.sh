#!/usr/bin/env bash
# Task 4b: Retry restarts the same race.
# Forced outcomes go through the drag-race developer aid with --retry (SR_DEBUG_RACE=drag): the save and the
# race record before the race (as challenge() does), then the offer and the restart loop as bob_drive_in.
#   SR_RETRY_FORCE=a,b,..  forces the result of race 1, 2, ... (hex player_result, W = a win; races past the
#                          list are not forced)
#   SR_RETRY_ANSWERS=RRG   answers the offers without the dialog (developer aid), for the multi-offer cases;
#                          the other cases answer through the dialog with keys (13 = R, 22 = G)
# With SR_RETRY_LOG=1 each race start logs "retry: start opp N type T bet K/A/B rnd s1 s2 s3 money M clock C",
# and the aid prints "debug race: before/after money .. drag .. road .. car .. model .. races ..".
. "$(dirname "$0")/lib.sh"
build_linux || fail "build"
d="$TESTS/t4"
# Up held from 3 s (a race needs the gas to end), then the answer key every 2 s until 116 s.
keys_for() { local k="$1" s="3:e048p" t; for t in $(seq 6 2 116); do s="$s,$t:$k"; done; echo "$s"; }
R_KEYS=$(keys_for 13)
G_KEYS=$(keys_for 22)

# run CASE FORCE KEYS [extra env...]: one session on a fresh game copy
run() {
    local name="$1" force="$2" keys="$3"; shift 3
    fresh_game "$d"
    env "$@" SR_DEV_SEED=7 SR_DEBUG_RACE=drag SR_RETRY_FORCE="$force" bash -c '
        . "'"$REPO"'/tests/retry/lib.sh"; run_headless "'"$d"'" 120 "'"$keys"'" --retry'
    [ "$(cat "$d/exit.txt")" = 0 ] || fail "$name: did not finish (exit $(cat "$d/exit.txt"))"
    cp "$d/stderr.txt" "$TESTS/t4-$name.log"
}
log() { cat "$TESTS/t4-$1.log"; }
starts() { log "$1" | grep 'retry: start' | sed 's/.*start //'; }
n_starts() { starts "$1" | wc -l; }
same_starts() { [ "$(starts "$1" | sort -u | wc -l)" = 1 ]; }
before_line() { log "$1" | grep 'debug race: before' | head -1 | sed 's/.*before //'; }
after_line()  { log "$1" | grep 'debug race: after'  | head -1 | sed 's/.*after //'; }

# one loss, Retry, the restarted race is won: two identical starts (opponent, type, bet, random state,
# money, clock before the race's cost), one restart
for c in "LOST 01" "CRASH 21" "BREAKDOWN 05" "POLICE 41"; do
    set -- $c
    run "$1" "$2,W" "$R_KEYS"
    log "$1" | grep -q "retry: outcome $1" || fail "$1: outcome: $(log "$1" | grep retry: | tr '\n' ' ')"
    [ "$(n_starts "$1")" = 2 ] || fail "$1: $(n_starts "$1") race starts, want 2"
    same_starts "$1" || fail "$1: the restarted race differs: $(starts "$1" | tr '\n' '|')"
    [ "$(log "$1" | grep -c 'retry: restarted')" = 1 ] || fail "$1: no restart line"
    pass "$1 -> Retry runs the same race again"
done

# jail (a fine with no money): restarted, no jail
run JAIL "41,W" "$R_KEYS" SR_DEBUG_MONEY=0
log JAIL | grep -q 'retry: outcome JAIL' || fail "jail: outcome: $(log JAIL | grep retry:)"
[ "$(n_starts JAIL)" = 2 ] && same_starts JAIL || fail "jail: starts: $(starts JAIL | tr '\n' '|')"
pass "JAIL -> the same race again"

# the King
run KING "01,W" "$R_KEYS" SR_DEBUG_OPP=21 SR_DEBUG_WINS=4
log KING | grep -q 'retry: outcome KING_LOST' || fail "king: outcome: $(log KING | grep retry:)"
[ "$(n_starts KING)" = 2 ] && same_starts KING || fail "king: starts: $(starts KING | tr '\n' '|')"
starts KING | head -1 | grep -q '^opp 21 ' || fail "king: not opponent 21"
pass "KING_LOST -> the same race against the King"

# pink slips: the car is back for the restart, and won again
run PINK "01,W" "$R_KEYS" SR_DEBUG_BET=5
[ "$(n_starts PINK)" = 2 ] && same_starts PINK || fail "pink: starts: $(starts PINK | tr '\n' '|')"
after_line PINK | grep -q 'car 1 ' || fail "pink: no car at the end"
pass "pink slip lost -> the same race, the car is back"

# Retry twice, then Go on: three identical starts, two restarts
run TWICE "01,01,01" "3:e048p" SR_RETRY_ANSWERS=RRG
[ "$(n_starts TWICE)" = 3 ] && same_starts TWICE || fail "twice: starts: $(starts TWICE | tr '\n' '|')"
[ "$(log TWICE | grep -c 'retry: restarted')" = 2 ] || fail "twice: restarts"
[ "$(log TWICE | grep -c 'retry: offer')" = 3 ] || fail "twice: offers"
pass "Retry twice, then Go on"

# no offer after a win or a jumped light
run JUMPED 81 "$R_KEYS"
log JUMPED | grep -q 'retry: outcome JUMPED_LIGHT' || fail "jumped light: outcome"
log JUMPED | grep -q 'retry: offer' && fail "jumped light: offered"
run WON W "$R_KEYS"
log WON | grep -q 'retry: outcome WON' || fail "won: outcome"
log WON | grep -q 'retry: offer' && fail "won: offered"
pass "no offer after a win or a jumped light"

# Go on: one race, the bet is lost
run GOON 01 "$G_KEYS" SR_DEBUG_BET=4
log GOON | grep -q 'retry: offer' || fail "go on: no offer"
[ "$(n_starts GOON)" = 1 ] || fail "go on: restarted"
[ "$(before_line GOON)" != "$(after_line GOON)" ] || fail "go on: nothing changed (bet not lost?)"
pass "Go on keeps the loss"

# a damaged slot: Retry failed (its OK box answered with Enter), no restart
BAD_KEYS="3:e048p"; for t in $(seq 6 2 60); do BAD_KEYS="$BAD_KEYS,$t:13,$((t + 1)):1c"; done
run BAD 01 "$BAD_KEYS" SR_RETRY_TRUNCATE=1
log BAD | grep -q 'retry: check failed' || fail "bad file: not refused: $(log BAD | grep retry:)"
[ "$(n_starts BAD)" = 1 ] || fail "bad file: restarted"
pass "damaged slot: Retry failed, game continues"

# without --retry: no save, no offer, slot 16 untouched (Game/ may already hold one from playing)
fresh_game "$d"; before=$(files_hash "$d")
s16() { ls "$d/Game" | grep -ix 'hotrod16.sav' | while read -r f; do sha256sum "$d/Game/$f"; done; }
s16_before=$(s16)
SR_DEV_SEED=7 SR_DEBUG_RACE=drag SR_RETRY_FORCE=01 run_headless "$d" 60 "$R_KEYS"
grep -q 'retry:' "$d/stderr.txt" && fail "retry active without --retry"
[ "$(s16)" = "$s16_before" ] || fail "slot 16 written without --retry"
[ "$(files_hash "$d")" = "$before" ] || fail "saved games changed without --retry"
pass "without --retry nothing happens"
