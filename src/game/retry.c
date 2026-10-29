/* Retry after a lost race: options, the silent save, the checked load and the outcome of a race.
 * See retry.h and docs/superpowers/specs/2026-10-29-retry-mod-design.md. Not part of the original. */
#include "game/retry.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/flow.h"
#include "game/garage.h"
#include "game/race.h"
#include "host.h"
#include "platform/platform.h"
#include "platform/video.h"

#define POOL_CARS   0x10        /* pools_reset 0000:3614: 16 car records, 0x8C part records */
#define POOL_PARTS  0x8C
#define FILE_MAX    0x10000     /* far more than 16 cars and 140 parts can take */

static bool enabled;
static int  slot = 16;
static bool saved_ok;           /* the save before the current race succeeded */
static bool requested;          /* the player chose Retry; the game loop loads the slot */
static u8   file_buf[FILE_MAX];
static u32  file_len;

static void logf_(const char *fmt, ...)
{
    if (!getenv("SR_RETRY_LOG")) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("retry: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void retry_set_options(bool on, int s)
{
    enabled = on;
    slot = s < 1 ? 1 : s > 16 ? 16 : s;
}

bool retry_enabled(void) { return enabled; }
int  retry_slot(void) { return slot; }

static u16 slot_name(void)
{
    return fl_save_name(FLOW_STK_FNAME, FLOW_TMPL_SAVE, (s16)slot);
}

void retry_before_race(void)
{
    saved_ok = enabled && flow_save_write(slot_name());
    if (enabled) logf_("saved slot %d %s", slot, saved_ok ? "ok" : "failed");
}

/* ---- outcome (spec 4.3; race_results.c: police 0x40, jumped light 0x81, lost-intact 1, breakdown
 * 5 / 0x11 / 0x15, everything else that is not a win is the crash branch) */
RetryOutcome retry_classify(s16 r)
{
    u16 res = DSW(DS_player_result);
    if (DSW(DS_player_won)) return RO_WON;
    if (DSW(DS_vs_king)) return res == 0x81 ? RO_JUMPED_LIGHT : RO_KING_LOST;
    if (r == 4) return RO_JAIL;
    if (res & 0x40) return RO_POLICE;
    if (res == 0x81) return RO_JUMPED_LIGHT;
    if (res == 1) return RO_LOST;
    if (res == 5 || res == 0x11 || res == 0x15) return RO_BREAKDOWN;
    return RO_CRASH;
}

const char *retry_outcome_name(RetryOutcome o)
{
    static const char *n[] = { "WON", "JUMPED_LIGHT", "POLICE", "JAIL", "LOST", "CRASH", "BREAKDOWN", "KING_LOST" };
    return (unsigned)o < sizeof n / sizeof *n ? n[o] : "?";
}

bool retry_is_loss(RetryOutcome o) { return o != RO_WON && o != RO_JUMPED_LIGHT; }

/* ---- the checked load. The walk mirrors load_car_chain / load_part_chain (flow_save.c): a pointer
 * word only says "zero / non-zero". */
static u16 buf16(u32 o) { return (u16)(file_buf[o] | file_buf[o + 1] << 8); }

static bool walk_parts(u32 *pos, int *parts)
{
    for (;;) {
        if (file_len - *pos < 8 || ++*parts > POOL_PARTS) return false;
        u16 next = buf16(*pos + 6);
        *pos += 8;
        if (next == 0) return true;
    }
}

static bool walk_cars(u32 *pos, int *cars, int *parts, bool one)
{
    for (;;) {
        if (file_len - *pos < 0x28 || ++*cars > POOL_CARS) return false;
        u32 c = *pos;
        *pos += 0x28;
        static const u16 fields[] = { 6, 8, 0xA, 0xC, 0xE, 0x10, 0x12 };
        for (unsigned k = 0; k < sizeof fields / sizeof *fields; k++)
            if (buf16(c + fields[k]) && !walk_parts(pos, parts)) return false;
        if (one || buf16(c + 0x26) == 0) return true;
    }
}

bool retry_check(void)
{
    file_len = 0;
    bool ok = false;
    char base[16];                                    /* the name without the drive: DGROUP stays untouched */
    snprintf(base, sizeof base, "HOTROD%d.SAV", slot);
    char *path = host_game_path(base, false);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) {
        size_t n = fread(file_buf, 1, sizeof file_buf, f);
        ok = n < sizeof file_buf && !ferror(f);
        fclose(f);
        file_len = (u32)n;
    }
    if (path) host_free(path);
    if (ok) {
        u32 pos = 0x24;
        int cars = 0, parts = 0;
        ok = file_len >= 0x24 &&
             (!buf16(0x16) || walk_cars(&pos, &cars, &parts, true)) &&
             (!buf16(0x18) || walk_cars(&pos, &cars, &parts, false)) &&
             (!buf16(0x1E) || walk_parts(&pos, &parts)) &&
             file_len - pos == 8 + 0x18C + 4 + 4 + 2;
    }
    logf_("check %s", ok ? "ok" : "failed");
    if (!ok) file_len = 0;
    return ok;
}

void retry_apply(void)
{
    if (file_len == 0) return;
    for (u16 i = 0; i < 0x24; i++) DSB((u16)(DS_player_name + i)) = file_buf[i];
    SaveReader rd;
    save_reader_mem(&rd, file_buf + 0x24, file_len - 0x24);
    bool ok = flow_load_apply(&rd);
    logf_(ok ? "applied" : "apply failed after a passed check");
    file_len = 0;
}

void retry_request(void) { requested = true; }

bool retry_take_request(void)
{
    bool r = requested;
    requested = false;
    return r;
}

/* ------------------------------------------------------------------------------------------------ */
/* DEVELOPER AID (off by default): SR_DEBUG_RETRY=<case> skips the title (as SR_DEBUG_RACE), starts a new
 * game with a stock car and runs one case of tests/retry/t3_core.sh, then exits (0 = passed). */
static u32 dgroup_hash(void)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < 0x10000; i++) h = (h ^ DSB((u16)i)) * 16777619u;
    return h;
}

static void debug_setup(void)
{
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
    fl_strcpy(DS_player_name, 0x4607);                /* any name: "_______________" */
    u16 car = car_new(5, 0, 0);
    if (DSW(DS_car) == 0) DSW(DS_car) = car;
}

bool retry_debug_main(void)
{
    const char *c = getenv("SR_DEBUG_RETRY");
    if (!c || !*c) return false;
    debug_setup();
    int rc = 0;
    if (!strcmp(c, "save")) {
        retry_before_race();
    } else if (!strcmp(c, "check")) {
        u32 h = dgroup_hash();
        retry_check();
        fprintf(stderr, "debug retry: dgroup %s\n", h == dgroup_hash() ? "unchanged" : "CHANGED");
    } else if (!strcmp(c, "roundtrip")) {
        s32 money = DSSL(DS_money);
        retry_before_race();
        DSL(DS_money) = (u32)(money + 1234);
        rc = !(saved_ok && retry_check());
        if (!rc) { retry_apply(); rc = DSSL(DS_money) != money; }
    } else if (!strcmp(c, "classify")) {
        /* {won, result, king, r, expected} */
        static const struct { u16 won, res, king; s16 r; RetryOutcome want; } t[] = {
            { 1, 0x01, 0, 3, RO_WON },          { 1, 0x01, 1, 3, RO_WON },
            { 0, 0x81, 0, 3, RO_JUMPED_LIGHT }, { 0, 0x81, 1, 3, RO_JUMPED_LIGHT },
            { 0, 0x41, 0, 3, RO_POLICE },       { 0, 0x49, 0, 3, RO_POLICE },
            { 0, 0x41, 0, 4, RO_JAIL },         { 0, 0x01, 0, 3, RO_LOST },
            { 0, 0x01, 0, 2, RO_LOST },         { 0, 0x05, 0, 2, RO_BREAKDOWN },
            { 0, 0x11, 0, 2, RO_BREAKDOWN },    { 0, 0x15, 0, 2, RO_BREAKDOWN },
            { 0, 0x21, 0, 2, RO_CRASH },        { 0, 0x03, 0, 1, RO_CRASH },
            { 0, 0x29, 0, 2, RO_CRASH },        { 0, 0x01, 1, 3, RO_KING_LOST },
            { 0, 0x21, 1, 2, RO_KING_LOST },
        };
        for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
            DSW(DS_player_won) = t[i].won;
            DSW(DS_player_result) = t[i].res;
            DSW(DS_vs_king) = t[i].king;
            RetryOutcome o = retry_classify(t[i].r);
            bool good = o == t[i].want && retry_is_loss(o) == (t[i].want != RO_WON && t[i].want != RO_JUMPED_LIGHT);
            fprintf(stderr, "debug retry: classify %u %s %s\n", i, retry_outcome_name(o), good ? "ok" : " bad ");
            if (!good) rc = 1;
        }
    } else {
        fprintf(stderr, "debug retry: unknown case %s\n", c);
        rc = 2;
    }
    exit(rc);
}
