# Changelog

## 0.3.0-retry (fork)

- **Retry a lost race** (`--retry`, off by default; launcher: Start > *Retry a lost race*): before each race at
  Bob's the game is saved silently in slot 16 (`HOTROD16.SAV`, a normal saved game that the save and load lists
  never show); after a lost race *Retry* runs the same race again (same opponent, kind, bet and random numbers),
  each try costing the race's time; *Go on* continues as the original. `--retry-slot N` picks another slot.
- Builds on Linux (links libm) and cross-builds for Windows with mingw-w64 (`cmake/mingw-w64-x86_64.cmake`).
- Release zips for Linux and Windows built by GitHub Actions from the tag (`tools/ci_build.sh`), third-party
  downloads pinned by SHA-256.
- Developer aids (environment variables, off by default): `SR_DEV_SEED`, `SR_DEBUG_SAVE`, `SR_DEBUG_RETRY`,
  `SR_RETRY_LOG` and more for the headless tests in `tests/retry/`.

## 0.2.0

- Separate music and sound effects volumes (`--music-volume`, `--sfx-volume`).
- Launcher: menus (File > Preferences with Always on top, Game settings > Sounds, About), as in Aces of the Pacific Enhanced.
- Launcher: Game settings > Sounds, with sliders for the music and sound effects volumes.

## 0.1.0

- Project started from the faithful Street Rod port, with its launcher renamed "Street Rod Enhanced".
- Enhanced road view in races and drives through town, redrawn smoothly at the display's rate (`--motion-delay`).
- High resolution (`--res-scale`) with anti-aliasing (`--aa`).
- Redrawn: the road, kerbs, sidewalks, sky, horizon, roadside objects, signs, banners, town walls, the opponent,
  the police car and the rear-view mirror.
- `--classic` shows the original picture only; `--town off` leaves the drives through town original.
- Launcher: a Picture box; the Program field refuses a DOS program such as `SR.EXE`.
