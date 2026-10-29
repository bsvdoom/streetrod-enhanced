# Retry option for Street Rod Enhanced — design

Date: 2026-10-29. Branch `mod/retry` of `streetrod-enhanced` (based on 0.2.0, 9393e28).
Source of the request: `../mod_readme.md` (the workspace root, Hungarian).

## 1. Intent

The player wants a second chance after losing a race at Bob's Drive-In: when a race is lost, the game
offers a **Retry** button that restores the game as it was just before that race. The state is kept in a
save slot that the player can choose (default: an invisible 16th slot).

The change is made directly in the Enhanced source (C), as a small, readable patch on its own branch.
Another user reviews that diff and builds it locally. There is no scripting layer.

## 2. Goals, non-goals, anti-goals

Goals

* Retry after every kind of lost race (§4.3), restoring the pre-race state, back in the garage.
* Off by default. Switched on in the launcher (or `--retry` on the command line). Switched off, the game
  behaves exactly as the unmodified Enhanced.
* Windows build (cross-compiled here with mingw-w64), game and launcher; Linux build for development.
* A compact patch: the engine-side changes are a few call sites, the rest in new files, so the diff is easy
  to review and could later be carried to the faithful `street-rod-sdl3` port (the hooked engine files are
  identical in both).

Non-goals

* A mod system (Lua or other scripting) and the data-mod layer (text files replacing prices, parts,
  opponents, texts). The call sites added here can host them later.
* Retrying the very same race (same opponent, same bet) directly. Retry returns to the garage.
* The faithful port in this round.

Anti-goals (a failure even if it works)

* Saved games 1–15 or `hall_dat` change in any way with the default slot.
* A slot file that the original DOS game or the port cannot read in the normal format.
* A failed Retry that loses the current game.
* Any change of behaviour when Retry is off (the faithful rules of `ENGINE.md` still hold).

## 3. Constraints

* Engine rules of `ENGINE.md`: the game state stays in `mem[]` at the original DGROUP addresses. The Retry
  code keeps only its own request flag and option values in C statics (never game state). Engine-side
  changes are marked `RETRY:`, the way the `ENH:` hooks are marked.
* Nothing is installed on the system. Third-party code lives in `../deps/` (SDL3 3.4.18 mingw, wxWidgets
  3.2.12 gcc13.2 x64, both SHA-256 checked, downloaded only).
* Cross toolchain: `x86_64-w64-mingw32-gcc/g++` 13 (win32 threads). Wine 10 is available for test runs.

## 4. Behaviour

### 4.1 Options

* Game: `--retry` switches it on; `--retry-slot N` (1–16, default 16) picks the slot. Ignored in demo mode.
* Launcher: a checkbox "Retry after a lost race" and a slot choice (1–16, default 16, with a note that
  slots 1–15 are the visible save slots and get overwritten before every race). Kept in `settings.ini`,
  passed as the options above.

### 4.2 Save point

Right before each `race_start()` in `challenge()` (`race_bob.c`, both sites: the King and an ordinary
opponent), after the bet is accepted: a silent save into the slot. At that point the bet is not yet
taken, the clock has not been charged for the race, and the fuel has not been used.

The silent save writes the same bytes as `save_game_screen` for the current state with the current player
name, shows nothing, and changes no game state. The writing part of `save_game_screen` is factored into a
function both use; the visible save keeps its exact behaviour.

### 4.3 Loss detection and the offer

After `race_results()` returns in `bob_drive_in()`, before the bookkeeping that follows it (stats, King
penalty, jail, drive home), the outcome is classified. If it is a loss and the save of §4.2 succeeded, a
two-button dialog in the game's own style asks "Retry" / "Go on" (mouse and keys).

| Outcome | Condition (check against `race_results.c`) | Loss? |
|---|---|---|
| won | `player_won` | no |
| jumped the light | `player_result == 0x81` | no |
| police, fined | `player_result & 0x40`, `r == 3` | yes |
| jail | `r == 4` | yes |
| lost, car intact | `player_result == 1` | yes |
| crash | the crash branch (`!breakdown`) | yes |
| breakdown | engine / transmission blown | yes |
| King: lost | `vs_king && !player_won` | yes |

Races "just for kicks" (no bet) are included: a crash or breakdown still costs. The King's road race won
leads to the ending: no offer.

* Go on: everything continues as the original does.
* Retry: `bob_drive_in` returns at once (no jail, no stats, no King penalty, no drive home), with a C-side
  retry request set. The game loop sees it after `bob_drive_in()`, loads the slot and shows the garage
  (redraw 1), without the `clock_hour()` that follows Bob's. `DS_game_end_flags` stays 0, so a jail
  sentence never becomes a game over.

### 4.4 Safe load

The load reads the whole slot file into a buffer and checks its structure first (header, chains end within
the file, exact tail length). Only then are the pools reset and the state replaced, through the same code
path as `load_game_screen` uses after a successful read. If the file is missing or bad, nothing is touched,
a "Retry failed" message is shown, and the game goes on as with "Go on". It never calls `new_game()`.

### 4.5 Result after Retry

The game is in the garage with the money, cars, parts, wins, opponents and date of the save point. Bob's
Drive-In has to be visited again (the cruise costs fuel and time as usual). The calendar headline is
re-picked, as on any load.

## 5. Layout of the change

```
src/game/retry.c, retry.h   options, silent save, safe load, outcome classification, dialog
src/game/flow_save.c        the writing part of save_game_screen factored out (no behaviour change)
src/game/race_bob.c         RETRY: save before race_start(), the offer after race_results()
src/game/flow_main.c        RETRY: the load after bob_drive_in()
src/main.c                  --retry, --retry-slot
launcher/                   checkbox, slot choice, settings, arguments
cmake/mingw-w64-x86_64.cmake, launcher/copy_dlls.cmake   cross build
```

## 6. Build

* Linux: `cmake -S . -B build -DSRE_LAUNCHER=OFF` with the system SDL3 (development and headless tests).
* Windows: the toolchain file, `SDL3_DIR` → `../deps/SDL3-3.4.18/x86_64-w64-mingw32/lib/cmake/SDL3`,
  wxWidgets from `../deps/wx-3.2.12`. The DLL copy step works from the mingw sysroot and those folders (no
  MSYS2 paths).

## 7. Verification

* Headless runs (`SDL_VIDEO_DRIVER=dummy`, `SR_KEYS` scripts, snapshots) that reach a race at Bob's and
  lose it in each way of §4.3 that can be scripted; the rest by forcing `player_result` in a test build.
* `HOTROD1..15.SAV` and `hall_dat` byte-identical before and after a session with Retry on.
* `HOTROD16.SAV` loads through the normal load code when copied to `HOTROD1.SAV`.
* A damaged slot file: "Retry failed", the game continues unchanged.
* Without `--retry`: snapshots identical to the unmodified build for the same script.
* The Windows game and launcher start under Wine; final check by the user on Windows.

## 8. Left to the builder

* How the dialog is shown: reuse an existing two-button screen with temporarily replaced labels (restored
  afterwards) or new screen records in an unused part of the record table.
* Names inside `retry.c`.

## 9. Calls made by the designer (not discussed with the user)

* The save point is after the bet is accepted, before `race_start()`.
* Retry also skips the race's bookkeeping (stats, King penalty), since the state is reloaded anyway.
* No Retry offer when the pre-race save failed.
* Retry is off in demo mode.
* A failed load keeps the current game and continues as "Go on".
* Races "just for kicks" are offered Retry too.
