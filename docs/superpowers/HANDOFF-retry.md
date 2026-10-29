# Retry mod — handoff (continue from here in a new conversation)

Last update: 2026-10-29 03:27 UTC. Branch `mod/retry` of `streetrod-enhanced`, HEAD `a21663a` (WIP commit).

## How to resume

1. Read, in this order:
   * this file;
   * `docs/superpowers/specs/2026-10-29-retry-mod-design.md` (the approved design);
   * `docs/superpowers/plans/2026-10-29-retry.md` (the plan, tasks 1–7);
   * `.superpowers/sdd/2026-10-29-retry/progress.md` (ledger: completed tasks and every ruling; git-ignored).
2. Skills: superpowers lives in `~/.jcode/superpowers/skills/` (the symlinks in `~/.jcode/skills/` point
   to themselves and are broken; read the SKILL.md files from the superpowers folder directly). The run uses
   `executing-plans` + `test-driven-development`; scripts `executing-plans/scripts/task-start|task-done`.
3. Working agreements with the user (Hungarian speaker; answer in Hungarian):
   * **stop and report after every task**; re-estimate the remaining steps from measured times when asked;
   * no `find` command; nothing is installed on the system (downloads only, into the project);
   * no em dashes in replies; short replies.

## What it is

After a lost race at Bob's Drive-In, the game offers **Retry** / **Go on**. Retry reloads the state saved
silently right before that race (slot 16, invisible to the game's own save / load screens; `--retry-slot N`
for 1–16) and returns to the garage. Off by default: `--retry` (the launcher checkbox comes in Task 6).
A direct C change in the Enhanced source, no Lua (the user dropped the mod system).

User decisions: Enhanced as the base; this repo, branch `mod/retry`; Retry after every kind of loss
(police and jail included), jumping the light is **not** a loss; default slot 16; saves stay vanilla;
Windows target, cross-built here with mingw; enable / disable in the launcher; data-mod layer deferred.

## Status

| Task | State | Commit |
|---|---|---|
| 0 Prep (branch, `Game/` copy of the user's files) | done | 26dc702 |
| 1 Design + builder check, plan | done | 203d17e, 8bd54ce |
| T1 Linux build + headless test harness | done | c9ea3fa |
| T2 save writer / loader without the screens | done | a35bf27 |
| T3 retry core (options, silent save, checked load, classify) | done | 02b1cae |
| **T4 offer dialog + engine call sites** | **WIP, one bug** | a21663a |
| T5 Windows game build (mingw) | todo | |
| T6 Launcher: Retry option, cross-built | todo | |
| T7 Docs + Windows zip | todo | |

### Task 4: what is left

1. **Bug (found by the user playing on Linux):** at Bob's the Retry dialog is invisible (black screen);
   keys work (Enter = Retry reloads, G = Go on), and JUMPED_LIGHT is logged correctly.
   Cause: in `src/game/race_bob.c` (`bob_drive_in`, after `s16 r = (s16)race_results();`) the call
   `retry_offer(r)` comes **after** `drv_pal_black(); opp_palette_set(-1);`, so the dialog is drawn with a
   black palette. Fix: move the `if (retry_offer(r)) { ui_pop(1); return; }` block to right after
   `race_results()`, **before** `drv_pal_black()`. The forced tests did not see it because the
   `SR_DEBUG_RACE=drag` path (hooks_race.c) has no `drv_pal_black()`.
2. Add a test that goes through the real Bob's path and checks the dialog is visible (snapshot not black
   in the dialog area). A working key script (fresh game, seed 7, `SR_DEBUG_RACE=bob`):
   `4:01` (Esc skips the cruise), then from 8 s to 40 s once a second cycling `2e` (C, challenge),
   `20` (D, drag race), `02` (1, just for kicks); `41:e048p` (hold Up); then `13` (R) every 2 s.
   That run reached `outcome BREAKDOWN`, `offer`, `check ok`. The bob debug path now applies the Retry
   and calls `garage_screen(1)` (hooks_race.c), so a garage snapshot should follow; not yet verified
   (that last run was interrupted).
3. Remove the temporary `dumpui` case from `retry_debug_main` in `src/game/retry.c`.
4. Run `tests/retry/t4_offer.sh` (was 11/11 PASS before the bug report), `t3_core.sh`, `t2_refactor.sh`;
   then commit ("Retry: the offer after a lost race and the engine call sites") and run
   `task-done ... 4 02b1cae -- bash tests/retry/t4_offer.sh`.

## Key facts for the code

* Engine changes are marked `RETRY:`. Module: `src/game/retry.c/.h`. Save split: `flow_save.c`
  (`flow_save_write`, `SaveReader`, `flow_load_apply`), `flow.h`.
* Call sites: `race_bob.c` `challenge()` both `race_start()` sites (save before); `bob_drive_in()` after
  `race_results()` (offer); `flow_main.c` `game_loop` case 2 after `bob_drive_in()` (apply, clear
  `DS_game_end_flags`, `music_mute(0)`, redraw 1, skip the following `clock_hour()`); `main.c` options.
* Dialog: reuses screen 0x1E (records 270 "Go ahead" -20, 271 "Forget it" -21, 272 text) with its label
  words and key map temporarily replaced (R / Enter = Retry, G / Esc = Go on), restored after; texts at
  DS:94C0 (unused DGROUP stack area). "Retry failed !" uses `dialog_msg`.
* Pools: 16 cars, 140 parts (`retry_check` enforces them).
* Developer aids (env vars, off by default): `SR_DEV_SEED` (fixed rnd seed), `SR_DEBUG_SAVE=in:out`,
  `SR_DEBUG_RETRY=save|check|roundtrip|classify`, `SR_DEBUG_RACE=drag|road|bob` with `SR_RETRY_FORCE`,
  `SR_DEBUG_MONEY`, `SR_DEBUG_BET`, `SR_DEBUG_WINS`, `SR_DEBUG_OPP`, `SR_RETRY_TRUNCATE`; `SR_RETRY_LOG=1`
  prints `retry: ...` lines.
* Tests: `tests/retry/lib.sh`, `t1_baseline.sh`, `t2_refactor.sh`, `t3_core.sh`, `t4_offer.sh`. They run
  headless on copies under `build-linux/tests/`. New files are created upper case (`HOTROD16.SAV`).
  Snapshot pixels are not reproducible between runs (wall-clock snapshots), so tests compare save bytes
  and log lines. Beware `set -e` in `lib.sh` with a trailing `grep` that finds nothing.
* Try it on Linux: `cmake --build build-linux && SR_RETRY_LOG=1 ./build-linux/streetrod-enhanced --game-dir Game --retry`
  (`Game/` is a copy; the user's originals are in the parent folder).

## Remaining tasks and estimates

Measured so far (estimate → actual): T1 10–15 → 3.5 min; T2 15–25 → 6 min; T3 20–35 → 3.5 min;
T4 25–45 → 20+ min (real-flow scripting and the palette bug made it the first task over its low end).
Not yet re-estimated as the user asked; do it at the end of Task 4. Plan's numbers for the rest:
T5 Windows game build 15–30 min, T6 launcher 20–40 min (riskiest: wx cross build), T7 docs + zip 10–15 min.

## Third-party downloads (outside the repo, in the workspace root `../deps/`)

* `deps/SDL3-3.4.18/` (SDL3-devel-3.4.18-mingw, SHA-256 049b9dd7…771cf checked)
* `deps/wx-3.2.12/` (wxMSW 3.2.12 gcc13.2 x64 Dev + ReleaseDLL + headers, SHA-256 checked; the `.7z`
  archives and `deps/wx_assets.txt` are still there, deletion was not approved)
* Tools present: mingw-w64 gcc 13 (win32), wine 10, cmake 4.2, ninja, libsdl3-dev, libwxgtk3.2-dev, 7z.
