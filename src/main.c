/* Street Rod Enhanced — entry point (the faithful port's, see ENGINE.md).
 *
 * usage: streetrod-enhanced [--game-dir DIR] [--scale N] [--fullscreen] [--check] [switches of SR.EXE]
 *   --game-dir    folder with the original game files (default: "Game" in the working directory)
 *   --scale       initial window scale: 320x240 times N (default 3)
 *   --fullscreen  start in full screen (Alt+Enter switches)
 *   --check       load and verify the original executable, print a summary and exit (no window)
 *   --music-volume N  volume of the music (the background tune), percent 0..100 (default 100)
 *   --sfx-volume N    volume of the sound effects, the engine and the loop sounds, 0..100 (default 100)
 * enhanced view (ENHANCED.md):
 *   --classic         the original's picture only
 *   --res-scale N     output = 320x200 times N, 1..8 (default 4; 1 with --classic)
 *   --aa N            anti-aliasing, N x N samples per output pixel, 1..4 (default 2)
 *   --motion-delay N  percent of a game frame the smooth view runs behind the game, 0..100 (default 100)
 *   --town on|off     enhance the town cruise too (default on)
 *   --reduced-sky-rotation on|off  the sky and the horizon turn a quarter as much as in the original (default on)
 *   --hd on|off       HD remastered cockpit and horizon graphics (default on; off with --classic)
 *   the original's command-line switches are passed on as they are: 1, nomouse, demo, auto, nouemem
 *   (platform.md §5.1)
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "host.h"
#include "mem.h"
#include "modules.h"
#include "platform/ega.h"
#include "platform/platform.h"
#include "enhanced/enhanced.h"
#include "game/retry.h"

#define MAX_GAME_ARGS 16

static int usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--game-dir DIR] [--scale N] [--fullscreen] [--check] "
                    "[--music-volume N] [--sfx-volume N] [--retry] [--retry-slot N] "
                    "[--town on|off] [--reduced-sky-rotation on|off] [--hd on|off] [--draw-distance N] [--fps N] [--vsync on|off] "
                    "[1] [nomouse] [demo] [auto] [nouemem]\n", prog);
    return 2;
}

/* The program file in `dir`: SR.EXE, or SRSE.EXE (Street Rod SE, the data-disk edition: the same program
 * with the protection skipped and CGA/Hercules switched off), whatever the case of the name. */
static bool find_exe(const char *dir, char *path, size_t n, const char **name)
{
    static const char *const names[] = { SR_EXE_NAME, "SRSE.EXE" };
    int count = 0;
    char **entries = SDL_GlobDirectory(dir, NULL, 0, &count);
    bool found = false;
    for (int k = 0; k < 2 && !found; k++)
        for (int i = 0; entries && i < count; i++)
            if (SDL_strcasecmp(entries[i], names[k]) == 0) {
                snprintf(path, n, "%s/%s", dir, entries[i]);
                *name = names[k];
                found = true;
                break;
            }
    SDL_free(entries);
    if (!found) { snprintf(path, n, "%s/%s", dir, SR_EXE_NAME); *name = SR_EXE_NAME; }
    return found;
}

int main(int argc, char **argv)
{
    const char *dir = "Game";
    int scale = 3;
    bool check = false, fullscreen = false;
    bool classic = false, town = true, hd = true;
    int sky_scroll = ENH_REDUCED_SKY_SCROLL;
    int music_volume = 100, sfx_volume = 100;
    int res_scale = -1, aa = ENH_DEFAULT_AA, motion_delay = ENH_DEFAULT_MOTION_DELAY;
    int draw_distance = ENH_DEFAULT_DRAW_DISTANCE;
    int target_fps = -1;
    bool vsync = true;
    bool retry = false;
    int retry_slot_n = 16;
    char *game_argv[MAX_GAME_ARGS + 1] = { argv[0] };
    int game_argc = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--game-dir") && v) { dir = v; i++; }
        else if (!strcmp(a, "--scale") && v) { scale = atoi(v); i++; }
        else if (!strcmp(a, "--fullscreen")) fullscreen = true;
        else if (!strcmp(a, "--check")) check = true;
        else if (!strcmp(a, "--music-volume") && v) { music_volume = atoi(v); i++; }
        else if (!strcmp(a, "--sfx-volume") && v) { sfx_volume = atoi(v); i++; }
        else if (!strcmp(a, "--classic")) classic = true;
        else if (!strcmp(a, "--res-scale") && v) { res_scale = atoi(v); i++; }
        else if (!strcmp(a, "--aa") && v) { aa = atoi(v); i++; }
        else if (!strcmp(a, "--motion-delay") && v) { motion_delay = atoi(v); i++; }
        else if (!strcmp(a, "--town") && v) { town = strcmp(v, "off") != 0; i++; }
        else if (!strcmp(a, "--hd") && v) { hd = strcmp(v, "off") != 0; i++; }
        else if (!strcmp(a, "--reduced-sky-rotation") && v) {
            sky_scroll = strcmp(v, "off") != 0 ? ENH_REDUCED_SKY_SCROLL : 100;
            i++;
        }
        else if (!strcmp(a, "--draw-distance") && v) { draw_distance = atoi(v); i++; }
        else if (!strcmp(a, "--fps") && v) { target_fps = !strcmp(v, "auto") ? -1 : atoi(v); i++; }
        else if (!strcmp(a, "--vsync") && v) { vsync = strcmp(v, "off") != 0 && strcmp(v, "0") != 0; i++; }
        else if (!strcmp(a, "--retry")) retry = true;                          /* RETRY: */
        else if (!strcmp(a, "--retry-slot") && v) { retry_slot_n = atoi(v); i++; }
        else if (a[0] == '-' && a[1] == '-') return usage(argv[0]);
        else if (game_argc < MAX_GAME_ARGS) game_argv[game_argc++] = argv[i];
    }

    char exe_path[1024];
    const char *exe_name;
    find_exe(dir, exe_path, sizeof exe_path, &exe_name);
    char err[256];
    if (!mem_load_exe(exe_path, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Street Rod Enhanced", err, NULL);
        return 1;
    }
    if (check) {
        printf("%s ok: image %u bytes at %04X:0000, DGROUP %04X\n", exe_name, mem_image_size, LOAD_SEG, DGROUP);
        return 0;
    }

    if (res_scale < 0) res_scale = classic ? 1 : ENH_DEFAULT_RES_SCALE;
    enh_init(!classic, res_scale, aa, motion_delay, town, draw_distance);
    enh_set_sky_scroll(sky_scroll);
    enh_hd_init(hd && !classic);
    /* RETRY: off in demo mode (the game plays itself) */
    for (int k = 1; k < game_argc; k++) if (!SDL_strcasecmp(game_argv[k], "demo")) retry = false;
    retry_set_options(retry, retry_slot_n);
    if (retry && retry_slot() != 16)
        fprintf(stderr, "warning: --retry-slot %d: saved game %d is overwritten before every race at Bob's\n",
                retry_slot(), retry_slot());
    host_set_fps(target_fps);
    host_set_vsync(vsync);
    if (!host_init(dir, scale, fullscreen)) return 1;
    host_set_volumes(music_volume, sfx_volume);
    ega_init();                     /* 16-colour planar display: frame source, planes */
    modules_init();                 /* hooks into subsystems ported separately (modules.h) */
    platform_main_init(game_argc, game_argv);   /* main 0000:066f up to the game loop */
    int rc = game_main();           /* game_loop 0000:503f, platform_exit 0000:0fea */
    host_shutdown();
    return rc;
}
