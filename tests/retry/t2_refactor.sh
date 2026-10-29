#!/usr/bin/env bash
# Task 2: the save writer and loader work without the screens and write exactly what the screens wrote.
# Developer aid SR_DEBUG_SAVE=<in>:<out> loads HOTROD<in>.SAV and saves it as HOTROD<out>.SAV, then exits.
#  1. for every original slot n: load n, save 16; then load 16, save 14  ->  16 and 14 are identical except the
#     4 "elapsed" bytes (file offset size-6..size-3), so a save round-trips through the new code exactly;
#  2. 16 equals the original slot n except those bytes and the stale pointer words, which only mean
#     "zero / non-zero" (compared as such);
#  3. the demo still runs (t1 again).
. "$(dirname "$0")/lib.sh"
build_linux || fail "build"

# Byte comparison of two save files, ignoring the elapsed ticks (and with --flags also comparing every
# pointer word only as zero / non-zero; pointer offsets follow the layout of spec 5.1).
cmp_save() {
    python3 - "$@" <<'PY'
import sys, struct
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
flags = len(sys.argv) > 3
if len(a) != len(b): print("length %d != %d" % (len(a), len(b))); sys.exit(1)
ign = set(range(len(a) - 6, len(a) - 2))
ptr = set()
if flags:
    def w(o): return struct.unpack_from('<H', a, o)[0]
    for o in (0x16, 0x18, 0x1A, 0x1E, 0x20): ptr.add(o)
    pos = 0x24
    def chain(pos):
        while True:
            ptr.add(pos + 6); nxt = w(pos + 6); pos += 8
            if nxt == 0: return pos
    def cars(pos, one):
        while True:
            c = pos; pos += 0x28
            for f in (6, 8, 0xA, 0xC, 0xE, 0x10, 0x12):
                ptr.add(c + f)
                if w(c + f): pos = chain(pos)
            ptr.add(c + 0x26)
            if one or w(c + 0x26) == 0: return pos
    if w(0x16): pos = cars(pos, True)
    if w(0x18): pos = cars(pos, False)
    if w(0x1E): pos = chain(pos)
diff = []
for o in range(len(a)):
    if o in ign or a[o] == b[o]: continue
    if (o in ptr or o - 1 in ptr):
        base = o if o in ptr else o - 1
        if (a[base] | a[base+1]) and (b[base] | b[base+1]) or not (a[base] | a[base+1]) and not (b[base] | b[base+1]): continue
    diff.append(o)
if diff: print("differ at", [hex(x) for x in diff[:10]]); sys.exit(1)
PY
}

d="$TESTS/t2"
fresh_game "$d"
# The game opens files whatever their case and creates new ones in upper case (HOTROD16.SAV).
sav() { local f; f=$(ls "$1" | grep -ix "hotrod$2.sav" | head -1); echo "$1/${f:-HOTROD$2.SAV}"; }
before=$(cd "$d/Game" && for f in hotrod{1..15}.sav hall_dat; do sha256sum "$f"; done)
for n in $(seq 1 15); do
    SR_DEV_SEED=1 SR_DEBUG_SAVE="$n:16" run_headless "$d" 20 ""
    [ "$(cat "$d/exit.txt")" = 0 ] || fail "slot $n -> 16: exit $(cat "$d/exit.txt"): $(tail -2 "$d/stderr.txt")"
    SR_DEV_SEED=1 SR_DEBUG_SAVE="16:14" run_headless "$d" 20 ""
    [ "$(cat "$d/exit.txt")" = 0 ] || fail "slot 16 -> 14: exit $(cat "$d/exit.txt")"
    cmp_save "$(sav "$d/Game" 16)" "$(sav "$d/Game" 14)" || fail "slot $n: round trip 16 -> 14 differs"
    cmp_save "$(sav "$d/Game" 16)" "$(sav "$REPO/Game" "$n")" flags || fail "slot $n: differs from the original file"
    cp "$(sav "$REPO/Game" 14)" "$(sav "$d/Game" 14)"           # restore slot 14 for the next round
done
after=$(cd "$d/Game" && for f in hotrod{1..15}.sav hall_dat; do sha256sum "$f"; done)
[ "$before" = "$after" ] || fail "saved games 1..15 or hall_dat changed"
pass "15 slots load and save through the new code identically"

bash "$(dirname "$0")/t1_baseline.sh" | tail -1
