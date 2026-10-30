# HD pictures, test 1: the cockpit, the gauge needles and the sky — design and plan

Status (2026-10-30): planned. Done: the picture dump (`SR_ENH_DUMP`) and its converter (steps 1–2). Branch `hd/cockpit`, to be
rebased onto `mod/retry` once Retry 0.3.0 is committed.

## 1. Goal

Replace the original's 320×200, 16-colour pictures with high-resolution, AI-generated ones on the host, the way
the enhanced road view already replaces the road. This first test is limited to what is on screen during every
race and drive through town:

* the cockpit: dashboard, steering wheel, A-pillar, hood, rear-view mirror frame;
* the gauge needles (speedometer, tachometer), drawn as smooth vector lines;
* the sky and the horizon (race and town).

If the test looks good, Bob's Drive-In, the opponents' cars and the garage come next (§9).

## 2. What the game draws (findings)

| Picture | Id | Where | Notes |
|---|---|---|---|
| Cockpit | 1014 (LIB2 #14) | `race_run`, `race_drive.c` | **One picture for every car.** Rows 0–64h go to pages 1/2 (above the split); the dashboard rows go to page 0, shown below the split |
| Clock face | 1240 | `race_run` | Blitted onto the cockpit for cars with a clock (automatic) |
| A-pillar, hood | 1006, 1007 | `road_draw_frame` | Masked, foreground layer over the road view |
| Mirror frame, mirror sky | 1009, 1243 | mirror drawing | |
| Sky | 1005 | `road_load_graphics` | **Shared** by races and the town |
| Horizon panorama | 1267 | `road_load_graphics` | Mountains; shared |
| Town skyline | 1008 | `road_load_graphics` | Pasted onto the mountains (masked, at 2Ch,0Ch) in town; shown fixed, not scrolled (`enh_road.c` `draw_sky`) |

* **Needles** are lines (`drv_line`) from the end-point tables (`needle_points`, `race_dash.c`); the old
  needle is erased by copying back a clean copy of the area (page 0 rows ≥ 100). A smooth vector needle is
  therefore cheap and needs no art.
* The steering wheel's knob, the shifter knob and the gear label move or change: they stay original in this
  test (§8).
* **Paint is a palette register.** The cars' colours (`set_paint_palette`, `garage_main.c`; the opponent's
  in `race_bob.c`) and the fades (`drv_pal_black` / `drv_pal_normal`) change palette registers, not pixels. An
  HD picture must follow the palette (§5.3). The cockpit is grey and does not use the paint register; the
  fades still apply.
* The opponent's car has only **4 real pictures per car class** (side, rear 3/4, rear, front:
  `race_road.c` 584–605); the other sizes are shrunk at run time. Not part of this test.

## 3. Resolution

* The picture is shown at 4:3. On a 4K screen (3840×2160) the game's area is 2880×2160: an HD picture for the
  whole screen width needs about **2880 px across**. 8K brings nothing.
* Store the HD pictures at **res-scale 8** (2560 wide for the full width, i.e. 8 times the original's pixels);
  draw them scaled to the output with bilinear filtering. Generate larger and scale down.
* The output today is at most 2560×1600 (`ENH_MAX_RES_SCALE 8`). Raising it to 9 (2880×1800) is a separate
  step and needs the worker-thread road renderer first (ENHANCED.md: about 8.5 ms a frame at 1280×800 on one
  core).

## 4. Art pipeline (offline, not shipped)

1. **Dump** (done): `SR_ENH_DUMP=dir` with `SR_DEBUG_RACE=drag` and `=cruise` writes `cockpit.idx`,
   `clock_face.idx` (cars with a clock), `pillar.idx`, `hood.idx`, `mirror.idx`, `sheets_race.idx`,
   `sheets_town.idx` (page 1 rows 66h–B7h: sky and horizon sheets) and the palettes `palette_race.txt` /
   `palette_town.txt` (taken 60 frames after the fade-in). Format: a line `IDX w h`, then w×h indices,
   FFh = transparent.
2. **Convert**: `tools/hd/idx2png.py DUMP OUT`: RGBA PNG, a 4× nearest copy (the img2img input), the mask.
3. **Generate**: img2img from the 4× copy with a low denoise (0.3–0.5) so the layout stays; a tile / canny
   ControlNet if available. A few variants per picture (denoise, style: photographic or painted).
   **Open:** the local dispatcher's MCP tool (`comfyui_generate_image`) is text-to-image only (FLUX.1-schnell,
   no input picture), which cannot keep the layout. Either call ComfyUI's HTTP API (127.0.0.1:8188) directly
   with an img2img workflow, or generate elsewhere. User decision.
4. **Mask**: the original mask scaled up and smoothed (the AI does not make alpha). The cockpit picture
   (320×190) has no mask: above the split the road view is drawn over it every frame, and the window edges
   come from the masked pillar (88×82) and hood (232×5), whose masks must match exactly at 1×.
5. **Index map**: for each HD pixel, the original palette index it comes from (the 4× nearest copy,
   scaled). Used for the palette at run time (§5.3).
6. **Pack**: `Game/hd/<id>.png` (RGBA) and `Game/hd/<id>.idx.png` (index map). Not part of the repository
   or the release (generated from the game's own pictures); users make or download their own.

## 5. Run-time design

### 5.1 Where the HD picture goes

The enhanced compose already lays the road view over the scaled EGA picture, pixel by pixel, where the
original's picture is not in the way (the written mask and the foreground layer, ENHANCED.md §2). The HD
pictures use the same compose step, as one more layer, by **screen rectangle**:

* The cockpit's rectangles are fixed (where `race_run` blits it); the pillar, hood and mirror frame are drawn
  at fixed places each frame. A table maps each picture id to its screen rectangle(s), checked at 1× with
  the compare dumps.
* Over those rectangles, after the road view: the HD picture where its mask is set, unless the EGA page has a
  pixel there that is not the picture's (the needles, the knob, the shifter, messages, the crash picture):
  compare the shown EGA pixel with the original picture's pixel at that place; where they differ, the
  original's pixel stays (scaled). This keeps every dynamic thing correct with no special cases, as the
  written mask does for the road.
* Above the split: the pillar, hood and mirror frame are on the foreground layer today; the HD versions are
  drawn in their place.

### 5.2 Sky and horizon

`enh_road.c` `draw_sky` / `sheet_row` sample the sheets at the output resolution already. With HD sheets
present, `sheet_row` samples the HD sheet (same coordinates × 8) instead of `src_rows`. The town skyline
stays a separate HD picture with alpha, laid over the HD mountains at 2Ch,0Ch × 8, as the game does.

### 5.3 Palette

The HD picture is made for the palette of the dump (`palette_*.txt`). At run time, for each palette index
whose current colour differs from the dump's, the HD pixels with that index in the index map are recoloured
by the ratio of the colours (linear light); an all-black palette (the fades) blacks them out. For the
cockpit this only matters for the fades; it is needed for the cars later (paint).

### 5.4 Needles

* Hook in `needle_update` / `needle_init` (`ENH:`): the shown value and the tick time.
* Centre and length: fitted once from the end-point tables, so that at 1× the vector needle lies on the
  original's line.
* Compose: over the needle area, the dashboard without needle (the HD dashboard, or the original's clean copy
  scaled), then an anti-aliased line at the output resolution, its value interpolated between updates
  (smooth, not 8 steps a second). The original needle pixels are hidden by the rule of §5.1 being reversed
  for the needle areas only (they are known to be the needle).
* Colour: the needle colour register (`DS_needle_colour`), through the current palette.
* Also the clock's hands, if they are drawn the same way (to check).

### 5.5 Options

* `--hd on|off` (default on when `Game/hd/` holds pictures); off with `--classic`. Launcher: a check box in
  the Picture group.
* Missing or wrong-sized HD files: that picture stays original, a line on stderr.

## 6. Steps

| # | Step | Estimate |
|---|---|---|
| 1 | Picture dump `SR_ENH_DUMP` (done) | — |
| 2 | `tools/hd/idx2png.py`, PNGs of the dump (done) | — |
| 3 | Generation test: cockpit, sky, mountains, skyline; before / after pictures | 2–4 h, mostly generation; blocked on §4.3 |
| 4 | HD loader (PNG via SDL_image or stb_image), screen-rectangle table, compose layer with the rule of §5.1 | 1–2 days |
| 5 | Sky and horizon from HD sheets | ½ day |
| 6 | Vector needles (§5.4); works without HD art too | ½–1 day |
| 7 | Palette following (§5.3), fades | ½ day |
| 8 | Options, launcher, README, CHANGELOG, ENHANCED.md status | ½ day |

Steps 4–8 are code; the user tests them in the game.

## 7. Tests

* Headless (as `tests/retry/`): a drag race and a cruise with `SR_SNAPSHOT_DIR`; with HD off the snapshots
  are byte-identical to the build without this change (pixels of the enhanced view excepted, which are not
  reproducible: compare the dashboard rows only).
* With HD on and test pictures that are the 8× nearest scale of the originals: the output equals HD off
  (checks the rectangles, the masks and the §5.1 rule).
* Needles: at 1× the vector needle covers the original's pixels (the compare dump).

## 8. Not in this test

The steering wheel knob, shifter, gear label, crash picture, start light and messages stay original
(scaled). The rear-view mirror's content is the enhanced road already.

## 9. Later (if the test is good)

* **Left Side Mirror (Bal oldali tükör HD-ban)**:
  - *Eredeti játék működése*: `race_road.c:1417` szerint az eredeti DOS motor nem külön 3D kamerát renderel, hanem a középső visszapillantó tükör (`back_page()`) bal feléből vág ki egy 32×19-es téglalapot `(248, 22)` koordinátánál, és másolja a 0. oldalra `(8, 103)`-hoz.
  - *A megközelítés (Visszapillantó kivágás HD-ban, ac9d4cf mintájára)*:
    Mivel a középső visszapillantó tükröt az `enh_road.c` már nagy felbontásban, simított 60 fps-sel rendereli az `xrgb` pufferbe (`248*S .. 280*S, 22*S .. 41*S`), ezt a képterületet az `enh_hd_compose` fázisban közvetlenül át tudjuk másolni a bal tükör üvegfelületére `(8*S, 103*S)`. A `cockpit.png`-ben a tükörlap belső része átlátszóvá (alpha = 0) válik.
    *Becsült idő*: **~1.5 – 2 óra**.
  - *B megközelítés (Külön bal oldali kamera nézet)*:
    Dedikált bal sáv/autó oldalvonal kamera frustum (`row_at_side_mirror`), ami kissé kifelé és hátrafelé néz.
    *Becsült idő*: **~4 – 6 óra**.

* **Kormány forgatás (Steering Wheel Spokes Rotation)**:
  - *Hiba*: A `race_dash.c:206` (`wheel_draw`) kanyarodáskor (`idx != 0`) egy 184×26 pixeles forgatott küllő-képet másol a 0. oldalra `(80, 163)`-hoz. Mivel a `cockpit.png` egyenes kormányt ábrázol, az eltérés miatt a kis felbontású EGA forgatott küllők rárajzolódnak a sima HD kormányra (szellemképes kettős kormány).
  - *Megoldás*: HD módban a küllők EGA felülírását elnyomjuk, és csak a forgó kormánygombot (piros/fehér gömb a peremen, `shape_draw_dot`) rajzoljuk ki dinamikusan, VAGY 3 HD kormányállást generálunk (egyenes, bal, jobb).
  - *Becsült idő*: **~1 – 2 óra**.

* **Váltókulissza (Shifter Gate)**:
  - *Hiba*: A `shifter_draw` (`race_dash.c:227`) egy 48×43 pixeles EGA téglalapot (`gate`) másol a 0. oldalra `(264, 123)`-hoz az automata / 3 sebességes / 4 sebességes kulisszáról. A `cockpit.png`-ben ezen a helyen jelenleg egy ovális hangszórórács van, amire ráül a szögletes, pixeles EGA doboz.
  - *Megoldás*: A `cockpit.png`-ben a hangszórórács helyére króm keretes HD váltósémát rajzolunk / illesztünk, így csak maga a mozgó váltógomb (`shifter_knob`) marad dinamikus felülírás.
  - *Becsült idő*: **~1.5 – 2 óra**.

* Bob's Drive-In: mostly still pictures; the same screen-rectangle layer.
* The opponents' and police cars: 4 pictures per class; the paint is a palette register, so the pictures
  are generated in a neutral colour with a paint mask and coloured at run time (§5.3). The views of one
  car must look like the same car: the hardest part for generation.
* The garage: cars built from layered part pictures, animations, menus; the most work.
