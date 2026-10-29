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
#include "game/ui.h"
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

/* ---- the dialog: screen 0x1E ("The old game will be replaced !" with "Go ahead" -20 / "Forget it" -21,
 * keys Enter / G -> -20, Esc / F -> -21) shown with other texts. The three label words of its records
 * and the four scan codes of its key map are changed for the call and restored afterwards. The texts
 * go into the unused part of the DGROUP stack (as FLOW_STK_*), referred to by label offsets from DS:239E. */
#define DLG_SCREEN    0x1E
#define DLG_REC_YES   270           /* "Go ahead", code -20 */
#define DLG_REC_NO    271           /* "Forget it", code -21 */
#define DLG_REC_TEXT  272           /* the centred line */
#define DLG_TEXT_BUF  0x94C0        /* 3 strings, below DS:990E */
#define UI_LABELS     0x239E

static u16 put_text(u16 at, const char *s)
{
    u16 i = 0;
    for (; s[i]; i++) DSB((u16)(at + i)) = (u8)s[i];
    DSB((u16)(at + i)) = 0;
    return (u16)(i + 1);
}

/* true = the first button. Keys: R / Enter = first, G / Esc = second. */
static bool dialog_ask(const char *text, const char *yes, const char *no)
{
    static const u16 recs[3] = { DLG_REC_YES, DLG_REC_NO, DLG_REC_TEXT };
    const char *str[3] = { yes, no, text };
    u16 saved_lab[3];
    u16 at = DLG_TEXT_BUF;
    for (int i = 0; i < 3; i++) {
        u16 lab = (u16)(0x09F4 + 0x12 * recs[i] + 4);
        saved_lab[i] = DSW(lab);
        DSW(lab) = (u16)(at - UI_LABELS);
        at = (u16)(at + put_text(at, str[i]));
    }
    /* key map {scan, key}: 01 Esc, 21 F -> no; 1C Enter, 22 G -> yes. Here: 01 Esc, 22 G -> no;
     * 1C Enter, 13 R -> yes. */
    u16 km = DSW((u16)(0x579F + DLG_SCREEN * 2));
    u8 saved_km[8];
    for (int i = 0; i < 8; i++) saved_km[i] = DSB((u16)(km + i));
    u8 no_key = DSB((u16)(km + 1)), yes_key = DSB((u16)(km + 5));
    const u8 map[8] = { 0x01, no_key, 0x22, no_key, 0x1C, yes_key, 0x13, yes_key };
    for (int i = 0; i < 8; i++) DSB((u16)(km + i)) = map[i];

    cursor_ctl(-2);
    s16 r = ui_dialog(DLG_SCREEN);
    cursor_ctl(-4);

    for (int i = 0; i < 8; i++) DSB((u16)(km + i)) = saved_km[i];
    for (int i = 0; i < 3; i++) DSW((u16)(0x09F4 + 0x12 * recs[i] + 4)) = saved_lab[i];
    return r == -20;
}

bool retry_offer(s16 r)
{
    if (!enabled) return false;
    RetryOutcome o = retry_classify(r);
    logf_("outcome %s", retry_outcome_name(o));
    if (!retry_is_loss(o) || !saved_ok) return false;
    saved_ok = false;                                 /* one offer per save */
    logf_("offer");
    if (!dialog_ask("Lost it ? Run that race again ?", "Retry", "Go on")) return false;
    if (!retry_check()) {
        dialog_msg("Retry failed !");                 /* the game's own message box, OK */
        return false;
    }
    retry_request();
    return true;
}

bool retry_take_request(void)
{
    bool r = requested;
    requested = false;
    return r;
}

/* ------------------------------------------------------------------------------------------------ */
/* DEVELOPER AIDS for SR_DEBUG_RACE=drag|road (hooks_race.c), with --retry (tests/retry/t4_offer.sh):
 *   SR_DEBUG_MONEY=n, SR_DEBUG_BET=k (bet kind 0..5, amount from the game's table), SR_DEBUG_WINS=n
 *   (drag and road wins) set up the game before the race;
 *   SR_RETRY_FORCE=<hex> replaces the race's result (W = a win); SR_RETRY_TRUNCATE=1 cuts the slot file
 *   by one byte right after the save. The state line "money .. drag .. road .. car .. parts .." is
 *   printed before the race and after the offer (and the load, when Retry was chosen). */
static void debug_state(const char *when)
{
    u16 car = DSW(DS_car);
    fprintf(stderr, "debug race: %s money %ld drag %d road %d car %u model %d races %d\n", when,
            (long)DSSL(DS_money), DSS(DS_drag_wins), DSS(DS_road_wins), car ? 1u : 0u,
            car ? DSS((u16)(car + 2)) : -1, DSS(DS_races_total));
}

void retry_debug_state(const char *when) { debug_state(when); }
void retry_debug_race_before_bob(void) { debug_state("before"); }

void retry_debug_race_before(void)
{
    const char *v;
    if ((v = getenv("SR_DEBUG_MONEY"))) DSL(DS_money) = (u32)atol(v);
    if ((v = getenv("SR_DEBUG_WINS"))) DSW(DS_drag_wins) = DSW(DS_road_wins) = (u16)atoi(v);
    if ((v = getenv("SR_DEBUG_BET"))) {
        static const s16 amount[6] = { 0, 10, 0x32, 0x19, 0x64, 0 };
        int k = atoi(v) & 7;
        if (k > 5) k = 5;
        DSW(DS_bet_kind) = (u16)k;
        DSW(DS_bet_amount) = (u16)amount[k];
        DSW(DS_bet_button) = 2;
    }
    if (enabled) {
        retry_before_race();
        if (getenv("SR_RETRY_TRUNCATE")) {
            char base[16];
            snprintf(base, sizeof base, "HOTROD%d.SAV", slot);
            char *path = host_game_path(base, false);
            FILE *f = path ? fopen(path, "rb") : NULL;
            if (f) {                                  /* portable: read it, write it back one byte shorter */
                static u8 tmp[FILE_MAX];
                size_t n = fread(tmp, 1, sizeof tmp, f);
                fclose(f);
                f = n > 0 ? fopen(path, "wb") : NULL;
                if (f) { fwrite(tmp, 1, n - 1, f); fclose(f); }
            }
            if (path) host_free(path);
        }
    }
    debug_state("before");
}

void retry_debug_race_after(void)
{
    const char *v = getenv("SR_RETRY_FORCE");
    if (!v || !*v) return;
    if (*v == 'W') {
        DSB(DS_opp_result) |= 1;
    } else {
        DSB(DS_opp_result) &= (u8)~1;
        DSW(DS_player_result) = (u16)strtol(v, NULL, 16);
        DSB(DS_tires_blown) = 0;
    }
}

void retry_debug_race_done(s16 r)
{
    if (retry_offer(r) && retry_take_request()) retry_apply();
    debug_state("after");
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
    } else if (!strncmp(c, "dumpui", 6)) {
        s16 scr = (s16)strtol(c + 7, NULL, 0);
        u16 r = DSW((u16)(0x2BAD + 2 * scr));
        for (int k = 0; k < 10; k++) {
            u16 o = (u16)(0x09F4 + 0x12 * r);
            s16 t = DSS(o), lab = DSS((u16)(o + 4)), nx = DSS((u16)(o + 0x10));
            fprintf(stderr, "rec %u @%04X type %d style %d label %#x code %d (%d,%d)-(%d,%d) next %d '%s'\n", r, o, t,
                    DSS((u16)(o + 2)), (u16)lab, DSS((u16)(o + 6)), DSS((u16)(o + 8)), DSS((u16)(o + 10)),
                    DSS((u16)(o + 12)), DSS((u16)(o + 14)), nx, t > 0 && t < 15 ? ds_str((u16)(0x239E + lab)) : "");
            r++;
        }
        for (u16 si = DSW((u16)(0x579F + scr * 2)); DSB(si); si += 2)
            fprintf(stderr, "key scan %02X -> %02X\n", DSB(si), DSB((u16)(si + 1)));
    } else {
        fprintf(stderr, "debug retry: unknown case %s\n", c);
        rc = 2;
    }
    exit(rc);
}
