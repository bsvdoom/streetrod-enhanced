#!/usr/bin/env bash
# Task 4b, the real path: Bob's Drive-In through the developer aid SR_DEBUG_RACE=bob (the cruise, then
# bob_drive_in). Keys: Esc skips the cruise; C (challenge), D (drag race), 1 (just for kicks) cycle once a
# second while opponents come and go; Up held for the race (seed 7: the car breaks down); R late, so the
# dialog stays up for several 250 ms snapshots. Checks:
#   * the Retry dialog is visible (not drawn on a blacked-out palette);
#   * Retry runs the same race again: two "retry: start" lines with equal fields (opponent, kind, bet,
#     King state, random-number state, money, clock, and palette register 0Ch given back);
#   * the restarted race is shown: snapshots after the dialog show the cockpit again, not the garage.
# The run ends by the timeout (a second loss leaves the second offer waiting).
. "$(dirname "$0")/lib.sh"
build_linux || fail "build"
d="$TESTS/t4bob"
fresh_game "$d"
keys="4:01"
for t in $(seq 8 1 40); do case $((t % 3)) in 0) k=2e ;; 1) k=20 ;; 2) k=02 ;; esac; keys="$keys,$t:$k"; done
keys="$keys,41:e048p,70:13,72:13,74:13"
SR_SNAPSHOT_MS=250 SR_DEV_SEED=7 SR_DEBUG_RACE=bob run_headless "$d" 110 "$keys" --retry
cp "$d/stderr.txt" "$TESTS/t4bob.log"
grep -q 'retry: offer' "$d/stderr.txt" || fail "no offer: $(grep retry: "$d/stderr.txt" | tr '\n' ' ')"
grep -q 'retry: restarted' "$d/stderr.txt" || fail "no restart: $(grep retry: "$d/stderr.txt" | tr '\n' ' ')"
starts=$(grep 'retry: start' "$d/stderr.txt" | sed 's/.*start //')
[ "$(echo "$starts" | wc -l)" -ge 2 ] || fail "only one race start"
[ "$(echo "$starts" | head -2 | sort -u | wc -l)" = 1 ] || fail "the restarted race differs: $(echo "$starts" | tr '\n' '|')"

# The dialog box (64,80)-(256,120) in game pixels, 4x in the snapshot: left of the buttons it is the box's
# plain light background. Only the visible dialog makes that area almost white. Find the end of the first
# dialog episode (after Retry the same race runs and may end with a second offer).
frames=("$d"/snap/*.bmp)
dlg=-1
for i in "${!frames[@]}"; do
    m=$(convert "${frames[$i]}" -crop 200x120+270+340 -colorspace gray -format '%[fx:mean]' info:)
    if awk -v m="$m" 'BEGIN { exit !(m > 0.85) }'; then dlg=$i
    elif [ "$dlg" -ge 0 ]; then break; fi
done
[ "$dlg" -ge 0 ] || fail "the Retry dialog was never visible (black screen)"

# After the first dialog: the cockpit again (the race), never the garage (roof beams make the top-left
# corner orange-brown). In the cockpit the status line at the bottom left shows "Gear:" in red.
race=0
for i in $(seq $((dlg + 1)) $((${#frames[@]} - 1))); do
    g=$(convert "${frames[$i]}" -crop 40x40+20+20 -format '%[fx:mean.r-mean.b]' info:)
    awk -v g="$g" 'BEGIN { exit !(g > 0.25) }' && fail "the garage after Retry (frame $i)"
    s=$(convert "${frames[$i]}" -crop 320x120+480+600 -colorspace gray -format '%[fx:mean]' info:)
    awk -v s="$s" 'BEGIN { exit !(s > 0.3) }' && race=1
done
[ "$race" = 1 ] || fail "no race shown after Retry"
pass "real Bob's race: visible dialog, Retry runs the same race again"
