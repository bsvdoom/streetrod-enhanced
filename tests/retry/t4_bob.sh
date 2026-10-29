#!/usr/bin/env bash
# Task 4, the real path: Bob's Drive-In through the developer aid SR_DEBUG_RACE=bob (the cruise, then
# bob_drive_in, and after a Retry what game_loop does: load the slot, show the garage). Keys: Esc skips the
# cruise; C (challenge), D (drag race), 1 (just for kicks) cycle once a second while opponents come and go;
# Up held for the race (the car breaks down or loses); then R. The dialog must be visible: in the snapshot
# taken while it is up, the dialog box area is not black.
. "$(dirname "$0")/lib.sh"
build_linux || fail "build"
d="$TESTS/t4bob"
fresh_game "$d"
keys="4:01"
for t in $(seq 8 1 40); do case $((t % 3)) in 0) k=2e ;; 1) k=20 ;; 2) k=02 ;; esac; keys="$keys,$t:$k"; done
keys="$keys,41:e048p"
# R late (70 s), so the dialog stays up for several 250 ms snapshots
keys="$keys,70:13,72:13,74:13"
SR_SNAPSHOT_MS=250 SR_DEV_SEED=7 SR_DEBUG_RACE=bob run_headless "$d" 80 "$keys" --retry
cp "$d/stderr.txt" "$TESTS/t4bob.log"
grep -q 'retry: offer' "$d/stderr.txt" || fail "no offer: $(grep retry: "$d/stderr.txt" | tr '\n' ' ')"
grep -q 'retry: applied' "$d/stderr.txt" || fail "Retry not applied"
b=$(grep 'debug race: before' "$d/stderr.txt" | head -1 | sed 's/.*before //')
a=$(grep 'debug race: after' "$d/stderr.txt" | head -1 | sed 's/.*after //')
[ "$b" = "$a" ] || fail "state after Retry differs: $b / $a"

# The dialog box (64,80)-(256,120) in game pixels, 4x in the snapshot: left of the buttons, game x 68..117,
# y 85..115, is the box's plain light background with the start of the text. Only the visible dialog makes
# that area almost white (a black palette gives 0; the race, Bob's and the game's own bubbles stay < 0.5).
seen=0
for f in "$d"/snap/*.bmp; do
    m=$(convert "$f" -crop 200x120+270+340 -colorspace gray -format '%[fx:mean]' info:)
    if awk -v m="$m" 'BEGIN { exit !(m > 0.85) }'; then seen=1; break; fi
done
[ "$seen" = 1 ] || fail "the Retry dialog was never visible (black screen)"

# and the garage follows: the roof beams make the top-left corner orange-brown
last="$(ls "$d"/snap/*.bmp | tail -1)"
c=$(convert "$last" -crop 40x40+20+20 -format '%[fx:mean.r-mean.b]' info:)
awk -v c="$c" 'BEGIN { exit !(c > 0.25) }' || fail "no garage after the Retry"
# with the garage's own colours: the floor in front of the car is grey, not the opponent's paint (register
# 0Ch, which the race lends to the opponent and bob_drive_in gives back with opp_palette_set(-1))
f=$(convert "$last" -crop 120x30+560+690 -format '%[fx:mean.r-mean.g]' info:)
awk -v f="$f" 'BEGIN { exit !(f < 0.15) }' || fail "garage floor in the opponent's colour after the Retry (r-g $f)"
pass "real Bob's race: visible dialog, Retry restores the state, garage shown"
