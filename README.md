# Street Rod Enhanced — SDL3

An enhanced native port of California Dreams' *Street Rod* (1989). Not an emulator. The original game files are
not included; the game is free from the copyright owner: https://www.streetrodonline.com/downloads/.

## Features

* Smooth road view in races and drives through town, at your display's refresh rate.
* High resolution (up to 2560 × 1600) with anti-aliasing.
* Separate music and sound effects volumes.
* The game is otherwise the original: garage, opponents, physics, police and screens.
* **Retry a lost race** (optional, off by default; not in the original): see [Retry](#retry).
* The Street Rod SE data disk's 25 cars.
* A launcher for cars, switches, picture and sound options.

## How to play (Windows)

1. Put your original game files in a folder named `Game` (for the data disk cars, Street Rod SE in `Game\datadisk`).
2. Place the program files next to it:

   ```text
   Street Rod Enhanced\
   ├── Game\                        <- your game files (SR.EXE, LIB1, LIB2, HOT_DATA, datadisk\ ...)
   ├── Street Rod Enhanced.exe      <- the launcher
   ├── streetrod-enhanced.exe       <- the game
   └── SDL3.dll and the other DLLs
   ```

3. Run `Street Rod Enhanced.exe`, pick your options and press **Play**.

* The folder must be writable (saved games and the hall of fame are saved in it).
* Alt+Enter: full screen.
* Launcher settings: `%APPDATA%\Street Rod Enhanced\settings.ini`.

## Retry

An option that is not in the original game. Switch it on in the launcher (**Start** box, *Retry a lost race*)
or with `--retry`.

* Before each race at Bob's Drive-In the game is saved silently in an extra slot, `HOTROD16.SAV`. The original's
  save and load screens list slots 1 to 15 only, so it never shows there, and your saved games are not touched.
  The file is a normal saved game (it loads in the original if renamed to `HOTROD1.SAV` ... `HOTROD15.SAV`).
* After a lost race (lost, crash, breakdown, police, a fine you cannot pay, the King) the game asks
  *Lost it ? Run that race again ?*: **Retry** (R or Enter) or **Go on** (G or Esc).
* **Retry** runs the same race again at once: the same opponent, kind of race and bet, with the money, cars and
  parts from before the race and the same random numbers (drive the same way and the same things happen).
  Each try costs the race's time on the calendar (three tries: three races' time). Lose again and you are
  asked again.
* **Go on** continues as the original does (the bet is lost, jail, the King's penalty, and so on).
* A jumped light and a won race are not offered a Retry. In the demo the option is off.
* `--retry-slot N` (1–16) uses another slot; 1–15 are your own saved games, and that slot is overwritten before
  every race (the game prints a warning when started so).
* The source of the change: `src/game/retry.c` and the lines marked `RETRY:` in the engine; design and plan in
  `docs/superpowers/`.

## Requirements

* Game files: `SR.EXE` (or `SRSE.EXE`), `LIB1`, `LIB2`, `HOT_DATA`.
* One CPU core draws the road view (about 8.5 ms a frame at the default); lower the resolution or anti-aliasing
  on slow machines.
* To build: CMake 3.24+, C11, SDL 3; the launcher needs C++17 and wxWidgets 3.2.

## Releases and checking them

The release zips are built by GitHub Actions from the tagged source (`.github/workflows/build.yml`), with the
same script you can run yourself: `tools/ci_build.sh all` builds the Linux and the Windows (mingw-w64 cross)
programs into `dist/` and zips them, with a `SHA256SUMS.txt` in each. Third-party files (the SDL3 mingw package,
the wxWidgets mingw DLLs) are downloaded from their official releases and checked against pinned SHA-256 sums
in that script. The build is reproducible: the same source gives byte-identical zips (pinned build image, fixed
file times, no link timestamp), so you can rebuild a release and compare its `SHA256SUMS.txt`. To rebuild in the
same environment as the release:

```bash
docker run --rm -v "$PWD:/src" -w /src ubuntu@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c bash -c '
  apt-get update && apt-get install -y --no-install-recommends git ca-certificates curl p7zip-full cmake \
    ninja-build gcc g++ libsdl3-dev libwxgtk3.2-dev g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix \
    binutils-mingw-w64-x86-64 && git config --global --add safe.directory /src && tools/ci_build.sh all'
```

## Build

In Git Bash or an MSYS2 MinGW64 shell:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

* `-DSRE_LAUNCHER=OFF` builds the game only.
* The build copies the DLLs both programs need beside them.

On Linux, the same for Linux (`libsdl3-dev`, `libwxgtk3.2-dev`), or for Windows with mingw-w64:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release                       # Linux
tools/ci_build.sh deps                                                          # SDL3 / wx mingw packages
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release \
      -DSDL3_DIR=$PWD/deps/SDL3-3.4.18/x86_64-w64-mingw32/lib/cmake/SDL3 -DSRL_WX_MINGW_DIR=$PWD/deps/wx-3.2.12
cmake --build build-win
```

Tests of the Retry option (headless, on a copy of `Game/`): `tests/retry/t*.sh`.

## Run

```bash
./build/streetrod-enhanced.exe --game-dir Game
```

| Option | Meaning |
|---|---|
| `--game-dir DIR` | Game files folder (default `Game`) |
| `--scale N` | Window size, × 320×240 (default 3) |
| `--fullscreen` | Start in full screen |
| `--res-scale N` | Resolution, × 320×200 (default 4, 2–8) |
| `--aa N` | Anti-aliasing, N×N samples (default 2, 1 = off) |
| `--motion-delay P` | Smooth view delay, % of a game frame (default 100) |
| `--town on\|off` | Enhanced drives through town (default on) |
| `--music-volume P` | Music volume, % (default 100) |
| `--sfx-volume P` | Sound effects volume, % (default 100) |
| `--classic` | Original picture only |
| `--retry` | Retry a lost race (see [Retry](#retry)) |
| `--retry-slot N` | The slot Retry saves to before a race (default 16) |
| `--check` | Check that `SR.EXE` loads, then exit |
| `demo`, `auto`, `nomouse` | The original's switches: self-playing demo, races drive themselves, no mouse |

## Controls

* Mouse, or arrows / keypad to move the pointer; Space, Ins or keypad 0 to click.
* Races: Up gas, Down brake, Left / Right steer, Space / Ins shift, A / Z gear up / down.
* Esc skips the drive through town and ends the demo. M music, Ctrl sound on / off.

## Layout

* `src/` — the game (`ENGINE.md`); `src/game/retry.c` — the Retry option.
* `src/enhanced/` — the enhanced road view (`ENHANCED.md`).
* `launcher/` — the launcher (`launcher/README.md`).
* `tools/ci_build.sh`, `.github/workflows/` — release builds; `cmake/` — the mingw-w64 toolchain.
* `tests/retry/` — tests of the Retry option.

## License

MIT (`LICENSE`). *Street Rod* is © California Dreams / Logical Design Works; its files are not included.

## Support

https://buymeacoffee.com/krzysztofkania
