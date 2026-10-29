# Retry mod and mod system for Street Rod Enhanced — design

Date: 2026-10-29. Branch `mod/retry` of `streetrod-enhanced` (based on 0.2.0, 9393e28).
Source of the request: `../mod_readme.md` (the workspace root, Hungarian).

## 1. Intent

The player wants a second chance after losing a race at Bob's Drive-In: when a race is lost, the game
offers a **Retry** button that restores the game as it was just before that race. The state is kept in a
save slot that the player can choose (default: an invisible 16th slot).

The work also lays the ground for **other mods**: a small, sandboxed Lua 5.4 layer that a mod uses through a
narrow API, so that another user can read a mod's source before playing and builds everything locally from
source.

## 2. Goals, non-goals, anti-goals

Goals

* Retry after every kind of lost race (§5.3), restoring the pre-race state, back in the garage.
* Mods are switched on and off in the launcher, per mod. With no mod enabled the game behaves exactly as now.
* Windows build (cross-compiled here with mingw-w64), game and launcher; Linux build for development.
* Mod code is plain Lua text in the repository, readable before use; Lua itself is vendored source, built
  with the game.

Non-goals (this round)

* The data-mod layer (text files that replace prices, parts, opponents, texts). Deferred; the mod system
  must not block adding it later.
* Retrying the very same race (same opponent, same bet) directly. Retry returns to the garage.
* Mods for the faithful `street-rod-sdl3` port.

Anti-goals (a failure even if it works)

* Saved games 1–15 or `hall_dat` change in any way when the mod is enabled with its defaults.
* Slot-16 files that the original DOS game or the port cannot read in the normal format.
* A mod that can reach files, the network, the OS, or native code.
* A broken mod that crashes or hangs the game.
* Any change of behaviour when no mod is enabled (the engine's faithful rules, `ENGINE.md`, still hold).

## 3. Constraints

* Engine rules of `ENGINE.md`: the game state stays in `mem[]` at the original DGROUP addresses. Mod hooks
  are marked `MOD:` in the engine, the same way the `ENH:` hooks are marked.
* The hooked engine files (`race_bob.c`, `race_results.c`, `flow_main.c`, `flow_save.c`, `ui_*.c`) are
  identical to the faithful port's. Keep the engine-side changes small and confined to a few call sites,
  so the same patch could later be carried to the faithful port.
* Nothing is installed on the system. Third-party code lives in the repository (`third_party/lua`) or in
  `../deps/` (SDL3 3.4.18 mingw, wxWidgets 3.2.12 gcc13.2 x64, both SHA-256 checked, downloaded only).
* Cross toolchain: `x86_64-w64-mingw32-gcc/g++` 13 (win32 threads). Wine 10 is available for test runs.

## 4. Decided by the user

| # | Decision |
|---|---|
| 1 | Base: Street Rod Enhanced, branch `mod/retry` in this repo |
| 2 | Mod system: data mods + sandboxed Lua (A+B); data mods deferred (§2) |
| 3 | Lua API: **narrow** (§6.3) |
| 4 | Retry = load the pre-race state, back in the garage; saves stay compatible |
| 5 | Retry after every kind of loss, including police and jail; jumping the light is **not** a loss |
| 6 | Default slot 16 (invisible); configurable 1–16 |
| 7 | Enable / disable in the launcher |
| 8 | Windows target, built with mingw here |

## 5. Retry behaviour

### 5.1 Save point

Right before each `race_start()` in `challenge()` (`race_bob.c`, both sites: the King and an ordinary
opponent), after the bet is accepted, the engine raises `race_start`. The Retry mod saves the game into its
slot then. At that point the bet is not yet taken, the clock has not been charged for the race, and the
fuel has not been used. This is "the game just before the race".

The save writes the same bytes as `save_game_screen` would for the current state with the current player
name (header DS:7E9A…, car chains, parts, DS:4970, opponents, clock, elapsed ticks, DS:5E0C). It does not
show any screen and does not change `DS_player_name`. Factor the writing part of `save_game_screen` into a
function both use; the visible save keeps its exact behaviour.

### 5.2 Loss detection and the offer

After `race_results()` returns in `bob_drive_in()`, and **before** the bookkeeping that follows it (stats,
King penalty, jail, drive home), the engine raises `race_end` with the outcome. If the outcome is a loss and
the save of §5.1 succeeded, the mod asks the player "Retry" / "Go on".

* Go on: everything continues as the original does.
* Retry: `bob_drive_in` returns at once (no jail, no stats, no King penalty, no drive home); the game loop
  loads the slot and shows the garage (redraw 1), without the extra `clock_hour()` that follows Bob's.
  `DS_game_end_flags` is cleared, so a jail sentence never becomes a game over.

### 5.3 Outcomes

Classified from `player_won`, `player_result` and the return value `r` of `race_results`:

| Outcome | Condition (to be checked against `race_results.c`) | Loss? |
|---|---|---|
| won | `player_won` | no |
| jumped the light | `player_result == 0x81` | no |
| police, fined | `player_result & 0x40`, `r == 3` | yes |
| jail | `r == 4` | yes |
| lost, car intact | `player_result == 1` | yes |
| crash | the crash branch (`!breakdown`) | yes |
| breakdown | engine / transmission blown | yes |
| King: lost | `vs_king && !player_won` | yes |

The King's road race won leads to the ending: no offer. Demo mode (`demo` switch): the mod system is not
started.

### 5.4 Result after Retry

The game is in the garage with the money, cars, parts, wins, opponents and the date of the save point. Bob's
Drive-In has to be visited again (the cruise costs fuel and time as usual). The calendar headline is
re-picked, as on any load.

## 6. Mod system

### 6.1 Layout

```
mods/<id>/mod.lua          the mod (Lua 5.4 text)
mods/<id>/README.md        what it does, its settings
src/mod/                   the host: Lua state, sandbox, API, hook entry points (mod.h)
third_party/lua/           Lua 5.4.9 source, pinned by SHA-256, unmodified, with its LICENSE
```

### 6.2 Enabling

* Game: `--mod <id>` (repeatable). Without it no Lua state is created and the hooks return at once.
* Launcher: a "Mods" box lists the folders under `mods/` beside the program, one checkbox each with the
  first line of its README. Choices are kept in `settings.ini` and passed as `--mod`.
* The release zip ships `mods/retry/`.

### 6.3 Narrow API (all a mod can reach)

```lua
-- events, defined by the mod
function on_race_start(race) end      -- race = { kind = "drag"|"road", bet = 0..5, amount = n, king = bool }
function on_race_end(race, outcome) end   -- outcome: "won", "jumped_light", "police", "jail", "lost",
                                          -- "crash", "breakdown"; return "retry" to request the reload
-- calls
game.save(slot)            -- 1..16, silent save; returns true / false
game.slot_exists(slot)     -- true when HOTROD<slot>.SAV has a readable header
game.ask(text, yes, no)    -- the game's own two-button dialog; returns true for `yes`
game.state()               -- read-only copy: money, drag_wins, road_wins, races, day, month
config                     -- the table returned by mods/<id>/config.lua, if present
log(text)                  -- to stderr / the log
```

The load itself is never called by the mod during the race-end event; the mod returns `"retry"` and the
engine performs it at the safe point of §5.2. Slot numbers outside 1–16 are refused.

### 6.4 Sandbox

* Libraries opened: base (without `dofile`, `loadfile`, `require`, `collectgarbage` except "count"),
  `string`, `table`, `math`, `utf8`. Not opened: `io`, `os`, `package`, `debug`, `coroutine` is allowed.
* `load` accepts text chunks only (mode "t"); precompiled bytecode is refused everywhere.
* Instruction limit per event (count hook) and a memory cap through the allocator (a few MB); exceeding
  either raises an error.
* Any error disables that mod for the session, logs the message, and the game goes on as if it were not
  enabled. The first error is also shown once in the game's own message box.

## 7. The Retry mod

`mods/retry/mod.lua` (sketch, the builder writes the final one):

```lua
local slot = (config and config.slot) or 16
local saved = false
function on_race_start(race) saved = game.save(slot) end
function on_race_end(race, outcome)
  if not saved or outcome == "won" or outcome == "jumped_light" then return end
  if game.ask("Retry this race?", "Retry", "Go on") then return "retry" end
end
```

`mods/retry/config.lua`: `return { slot = 16 }`. Slots 1–15 are allowed; the README warns that such a slot
is overwritten before every race.

## 8. Build

* Linux: `cmake -S . -B build -DSRE_LAUNCHER=OFF` with the system SDL3 (development and headless tests).
* Windows: a toolchain file `cmake/mingw-w64-x86_64.cmake`, `SDL3_DIR` → `../deps/SDL3-3.4.18/...`,
  wxWidgets from `../deps/wx-3.2.12`. The DLL copy step works from the mingw sysroot and those folders (no
  MSYS2 paths).
* Lua is a static library target from `third_party/lua` (no `lua.c`/`luac.c`).

## 9. Verification

* Headless runs (`SDL_VIDEO_DRIVER=dummy`, `SR_KEYS` scripts, snapshots) that reach a race at Bob's and lose
  it in each way of §5.3 that can be scripted; the rest by forcing `player_result` in a test build.
* `HOTROD1..15.SAV` and `hall_dat` byte-identical before and after a session with the mod.
* `HOTROD16.SAV` loads through the normal load code when copied to `HOTROD1.SAV`.
* Without `--mod`: snapshots identical to the unmodified build for the same script.
* Sandbox: test mods that try `io`, `os`, bytecode, an endless loop, a memory bomb; each is disabled with a
  logged error and the game continues.
* The Windows game and launcher start under Wine; final check by the user on Windows.

## 10. Left to the builder

* How `game.ask` shows its dialog: reuse an existing two-button screen with temporarily replaced labels
  (restored afterwards) or new screen records in an unused part of the record table. It must look like the
  game's own dialogs and work with mouse and keys.
* Exact limits of the sandbox (instructions, memory).
* Names and the split of `src/mod/` files.

## 11. Calls made by the designer (not discussed with the user)

* The save point is after the bet is accepted, before `race_start()`.
* Retry also skips the race's bookkeeping (stats, King penalty), since the state is reloaded anyway.
* No Retry offer when the pre-race save failed.
* The mod system does not run in demo mode.
* Lua 5.4.9 (latest 5.4), vendored.
* (builder check) **Failed Retry load:** the silent load first reads the whole file into a buffer and
  checks its structure; only then are the pools reset and the state replaced. If the file is missing or
  bad, nothing is touched, the message "Retry failed" is shown, and the game goes on as with "Go on".
  It never calls `new_game()`.
* (builder check) **Races "just for kicks"** (no bet) are offered Retry too: a crash or breakdown still
  costs money and parts, and the user asked for every kind of loss.
* (builder check) **Several mods:** events go to the enabled mods in launcher order; the first `"retry"`
  wins and the later mods' `on_race_end` is not called for that event. One `game.ask` per mod per event.
* (builder check) **The retry request** travels in a C-side static of `src/mod/` (mod state, not game
  state, never saved), read by `flow_main.c` after `bob_drive_in()` returns. No DGROUP address is used.
