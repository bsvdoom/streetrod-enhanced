#!/usr/bin/env bash
# Task 4: the Retry offer after a lost race.
# Forced outcomes go through the drag-race developer aid with --retry: SR_DEBUG_RACE=drag saves before the
# race (as challenge() does), SR_RETRY_FORCE=<hex> sets the race result after it, then race_results and the
# offer run as in bob_drive_in. Keys answer the dialog: 13 = R (Retry), 22 = G (Go on); they are pressed
# repeatedly so they land whenever the dialog is up. The aid prints "debug race: money N drag D road R car C
# slot16 S" before the race and after the offer / Retry.
. "$(dirname "$0")/lib.sh"
build_linux || fail "build"
d="$TESTS/t4"
# Up held from 3 s (the race needs the gas to end), then the answer key every 2 s until 56 s.
keys_for() { local k="$1" s="3:e048p" t; for t in $(seq 6 2 56); do s="$s,$t:$k"; done; echo "$s"; }
R_KEYS=$(keys_for 13)
G_KEYS=$(keys_for 22)

# run CASE FORCE KEYS [extra env...]: one forced race on a fresh game copy
run() {
    local name="$1" force="$2" keys="$3"; shift 3
    fresh_game "$d"
    env "$@" SR_DEV_SEED=7 SR_DEBUG_RACE=drag SR_RETRY_FORCE="$force" bash -c '
        . "'"$REPO"'/tests/retry/lib.sh"; run_headless "'"$d"'" 60 "'"$keys"'" --retry'
    cp "$d/stderr.txt" "$TESTS/t4-$name.log"
}
log() { cat "$TESTS/t4-$1.log"; }
before_line() { log "$1" | grep 'debug race: before' | head -1 | sed 's/.*before //'; }
after_line()  { log "$1" | grep 'debug race: after'  | head -1 | sed 's/.*after //'; }

for c in "LOST 01" "CRASH 21" "BREAKDOWN 05" "POLICE 41"; do
    set -- $c
    run "$1" "$2" "$R_KEYS"
    log "$1" | grep -q "retry: outcome $1" || fail "$1: outcome not $1: $(log "$1" | grep retry: | tr '\n' ' ')"
    log "$1" | grep -q 'retry: applied' || fail "$1: Retry not applied"
    [ "$(before_line "$1")" = "$(after_line "$1")" ] || fail "$1: state after Retry differs: $(before_line "$1") / $(after_line "$1")"
    pass "$1 -> Retry restores the pre-race state"
done

# a win and a jumped light: no offer
run JUMPED 81 "$R_KEYS"
log JUMPED | grep -q 'retry: outcome JUMPED_LIGHT' || fail "jumped light: outcome"
log JUMPED | grep -q 'retry: offer' && fail "jumped light: offered"
run WON W "$R_KEYS"
log WON | grep -q 'retry: outcome WON' || fail "won: outcome"
log WON | grep -q 'retry: offer' && fail "won: offered"
pass "no offer after a win or a jumped light"

# jail: a speeding fine with no money
run JAIL 41 "$R_KEYS" SR_DEBUG_MONEY=0
log JAIL | grep -q 'retry: outcome JAIL' || fail "jail: outcome: $(log JAIL | grep retry:)"
log JAIL | grep -q 'retry: applied' || fail "jail: not applied"
[ "$(before_line JAIL)" = "$(after_line JAIL)" ] || fail "jail: state differs"
pass "JAIL -> Retry"

# the King: wins are restored (no King penalty)
run KING 01 "$R_KEYS" SR_DEBUG_OPP=21 SR_DEBUG_WINS=4
log KING | grep -q 'retry: outcome KING_LOST' || fail "king: outcome: $(log KING | grep retry:)"
[ "$(before_line KING)" = "$(after_line KING)" ] || fail "king: state differs: $(before_line KING) / $(after_line KING)"
pass "KING_LOST -> Retry, wins kept"

# pink slips lost: the car is back
run PINK 01 "$R_KEYS" SR_DEBUG_BET=5
log PINK | grep -q 'retry: applied' || fail "pink: not applied"
after_line PINK | grep -q 'car 0 ' && fail "pink: no car after Retry"
[ "$(before_line PINK)" = "$(after_line PINK)" ] || fail "pink: state differs"
pass "pink slip lost -> Retry gives the car back"

# Go on: no load, the bet is lost
run GOON 01 "$G_KEYS" SR_DEBUG_BET=4
log GOON | grep -q 'retry: offer' || fail "go on: no offer"
log GOON | grep -q 'retry: applied' && fail "go on: applied"
[ "$(before_line GOON)" != "$(after_line GOON)" ] || fail "go on: nothing changed (bet not lost?)"
pass "Go on keeps the loss"

# a damaged slot: Retry failed, the game goes on
run BAD 01 "$R_KEYS" SR_RETRY_TRUNCATE=1
log BAD | grep -q 'retry: check failed' || fail "bad file: not refused: $(log BAD | grep retry:)"
log BAD | grep -q 'retry: applied' && fail "bad file: applied"
pass "damaged slot: Retry failed, game continues"

# without --retry: no save, no offer, no slot 16
fresh_game "$d"; before=$(files_hash "$d")
SR_DEV_SEED=7 SR_DEBUG_RACE=drag SR_RETRY_FORCE=01 run_headless "$d" 40 "$R_KEYS"
grep -q 'retry:' "$d/stderr.txt" && fail "retry active without --retry"
[ -z "$(ls "$d/Game" | grep -i hotrod16)" ] || fail "slot 16 written without --retry"
[ "$(files_hash "$d")" = "$before" ] || fail "saved games changed without --retry"
pass "without --retry nothing happens"
