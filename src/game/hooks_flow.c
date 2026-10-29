/* Hook registration of the flow subsystem (modules.h), and a developer aid. Owned by the flow port. */
#include "game/flow.h"

#include <stdio.h>
#include <stdlib.h>

#include "game/garage.h"
#include "game/race.h"
#include "modules.h"
#include "platform/platform.h"
#include "platform/video.h"

void flow_register_hooks(void)
{
    modules.demo_step = demo_step;                /* 0000:1bae, from timer_isr in demo mode */
}

/* ------------------------------------------------------------------------------------------------ */
/* DEVELOPER AID (not part of the original, off by default).
 *
 * SR_DEBUG_SAVE=<in>:<out> skips the title and the menus (the set-up of title_and_setup 0000:39c0
 * without the title, as SR_DEBUG_RACE), loads HOTROD<in>.SAV through flow_load_apply, saves the state as
 * HOTROD<out>.SAV through flow_save_write and exits: 0 on success, 2 when the load fails, 3 when the save
 * fails (tests/retry/t2_refactor.sh). */
bool flow_debug_save_main(void)
{
    const char *spec = getenv("SR_DEBUG_SAVE");
    int in = 0, out = 0;
    if (!spec || sscanf(spec, "%d:%d", &in, &out) != 2) return false;
    gfx_screen_mode(0);
    drv_pal_black();
    gfx_screen_mode(1);
    rnd(-1);
    if (DSW(DS_libs_preloaded) == 0) lib_read_dir(2);
    cursor_ctl(-4);
    track_build_all();
    if (DSW(DS_libs_preloaded) == 0) {
        hot_data_load();
        if (DSS(DS_driver_id) == -2) pic_park_list(DS_resident_pics);
    }
    pic_load_list(DS_resident_pics, 1);
    cursor_ctl(-2);
    new_game();

    u16 name = fl_save_name(FLOW_STK_FNAME, FLOW_TMPL_SAVE, (s16)in);
    u16 fd = 0;
    u16 got = 0;
    bool ok = dos_open(ds_str(name), 0, &fd) == 0;
    if (ok) {
        ok = dos_read(fd, ds_ptr(DS_player_name), 0x24, &got) == 0 && got == 0x24;
        SaveReader rd;
        save_reader_fd(&rd, fd);
        ok = ok && flow_load_apply(&rd);
        dos_close(fd);
    }
    if (!ok) { fprintf(stderr, "debug save: cannot load slot %d\n", in); exit(2); }
    name = fl_save_name(FLOW_STK_FNAME, FLOW_TMPL_SAVE, (s16)out);
    if (!flow_save_write(name)) { fprintf(stderr, "debug save: cannot save slot %d\n", out); exit(3); }
    fprintf(stderr, "debug save: slot %d -> slot %d\n", in, out);
    exit(0);
}
