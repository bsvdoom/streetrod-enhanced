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
#include "game/race_int.h"
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
static int  tries;              /* restarts of the current race (each adds the race's time) */
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

/* ---- the race record (spec 4.6): what the save does not hold, kept for the restart. In C only, never
 * written to a file, so the slot stays a vanilla save. */
#define BOB_PICS     0x827A     /* 827A opponent picture, 827C colour, 827E tyre set, 8280 face */
static struct {
    u16 opp, type, bet_kind, bet_amount, bet_button, king_drag_won;
    u8  king_ready;
    u16 rnd[3];
    u16 bob[4];
} rec;

static void race_record_save(void)
{
    rec.opp = DSW(DS_opp_index);
    rec.type = DSW(DS_race_type);
    rec.bet_kind = DSW(DS_bet_kind);
    rec.bet_amount = DSW(DS_bet_amount);
    rec.bet_button = DSW(DS_bet_button);
    rec.king_drag_won = DSW(DS_king_drag_won);
    rec.king_ready = DSB(DS_king_ready);
    rec.rnd[0] = DSW(DS_rnd_s1); rec.rnd[1] = DSW(DS_rnd_s2); rec.rnd[2] = DSW(DS_rnd_s3);
    for (int i = 0; i < 4; i++) rec.bob[i] = DSW((u16)(BOB_PICS + 2 * i));
}

/* Everything but the random-number state, which comes last (race_record_rnd). The opponent fields are set
 * as opponent_load 0000:98ba sets them, without its random sticker / customising. */
static void race_record_restore(void)
{
    DSW(DS_opp_index) = rec.opp;
    DSW(DS_race_type) = rec.type;
    DSW(DS_bet_kind) = rec.bet_kind;
    DSW(DS_bet_amount) = rec.bet_amount;
    DSW(DS_bet_button) = rec.bet_button;
    DSW(DS_king_drag_won) = rec.king_drag_won;
    DSB(DS_king_ready) = rec.king_ready;
    u16 orec = OREC(rec.opp);
    DSW(DS_opp_rec) = orec;
    DSW(DS_opp_model) = DSW((u16)(orec + OR_MODEL));
    DSW(DS_opp_model_ptr) = MODEL(DSS(DS_opp_model));
    DSW(DS_vs_king) = rec.opp == 0x15;
    for (int i = 0; i < 4; i++) DSW((u16)(BOB_PICS + 2 * i)) = rec.bob[i];
}

static void race_record_rnd(void)
{
    DSW(DS_rnd_s1) = rec.rnd[0]; DSW(DS_rnd_s2) = rec.rnd[1]; DSW(DS_rnd_s3) = rec.rnd[2];
}

/* "retry: start ..." right before race_start: what the race is run with (tests compare these). pal0c is
 * the shadow of palette register 0Ch, which drive_run lends to the opponent: equal on every start means it
 * was given back before the restart. */
static void log_start(void)
{
    if (!enabled) return;
    logf_("start opp %u type %u bet %u/%u/%u king %u/%u rnd %u %u %u money %ld clock %lu pal0c %u",
          DSW(DS_opp_index), DSW(DS_race_type), DSW(DS_bet_kind), DSW(DS_bet_amount), DSW(DS_bet_button),
          DSW(DS_king_drag_won), DSB(DS_king_ready), DSW(DS_rnd_s1), DSW(DS_rnd_s2), DSW(DS_rnd_s3),
          (long)DSSL(DS_money), (unsigned long)DSL(DS_game_clock), DSB((u16)(DS_pal_shadow + 0x0C)));
}

void retry_before_race(void)
{
    saved_ok = enabled && flow_save_write(slot_name());
    if (enabled) logf_("saved slot %d %s", slot, saved_ok ? "ok" : "failed");
    if (saved_ok) race_record_save();
    tries = 0;
    log_start();
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

/* Loads the checked buffer into the game. retry_check mirrors the loader, so this cannot fail; should it all
 * the same (a defect), the whole DGROUP (all of the game's state) is put back as it was and false is returned,
 * so a failed Retry never loses the current game (spec 4.4). */
static u8 dgroup_backup[0x10000];

bool retry_apply(void)
{
    if (file_len == 0) return false;
    memcpy(dgroup_backup, mp(DGROUP, 0), sizeof dgroup_backup);
    for (u16 i = 0; i < 0x24; i++) DSB((u16)(DS_player_name + i)) = file_buf[i];
    SaveReader rd;
    /* developer aid SR_RETRY_APPLY_SHORT=1: hand the loader a shortened buffer (tests the fallback) */
    u32 len = file_len - 0x24;
    if (getenv("SR_RETRY_APPLY_SHORT")) len = len > 8 ? len - 8 : 0;
    save_reader_mem(&rd, file_buf + 0x24, len);
    bool ok = flow_load_apply(&rd);
    if (!ok) memcpy(mp(DGROUP, 0), dgroup_backup, sizeof dgroup_backup);
    logf_(ok ? "applied" : "apply failed after a passed check; game state put back");
    file_len = 0;
    return ok;
}

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
static bool retry_dlg_active = false;
bool retry_is_dialog_active(void) { return retry_dlg_active; }

static bool dialog_ask(const char *text, const char *yes, const char *no)
{
    retry_dlg_active = true;
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
    retry_dlg_active = false;
    return r == -20;
}

bool retry_offer(s16 r)
{
    if (!enabled) return false;
    RetryOutcome o = retry_classify(r);
    logf_("outcome %s", retry_outcome_name(o));
    if (!retry_is_loss(o) || !saved_ok) return false;
    logf_("offer");
    bool yes;
    const char *ans = getenv("SR_RETRY_ANSWERS");      /* developer aid: answers without the dialog */
    static int n_ans;
    if (ans && *ans) yes = ans[n_ans < (int)strlen(ans) ? n_ans++ : (int)strlen(ans) - 1] == 'R';
    else yes = dialog_ask("Lost it ? Run that race again ?", "Retry", "Go on");
    if (!yes) { saved_ok = false; return false; }
    if (!retry_check()) {                             /* nothing touched yet: "Go on" */
        saved_ok = false;
        dialog_msg("Retry failed !");                 /* the game's own message box, OK */
        return false;
    }
    /* give register 0Ch back (the race lent it to the opponent; opp_palette_set(-1) works once only), then
     * load. Should the load fail, retry_apply has put all of DGROUP back (the palette state with it), and
     * bob_drive_in's own drv_pal_black / opp_palette_set(-1) run as after any race: "Go on". */
    drv_pal_black();
    opp_palette_set(-1);
    if (!retry_apply()) {
        saved_ok = false;
        drv_pal_normal();
        dialog_msg("Retry failed !");
        return false;
    }
    return true;
}

/* After a true retry_offer: the pre-race state, then the same race (spec 4.6). saved_ok stays true: the
 * slot still holds the state before the first try, so a new loss is offered again. */
static void (*after_start_hook)(void);

void retry_restart_race(void)
{
    race_record_restore();
    /* each try costs the race's time (spec 4.5): the reload set the clock back to before the first try, so
     * add back the earlier tries' cost; the cost of this try is added after race_start, as challenge() does */
    tries++;
    flow_clock_add((u32)tries * (DSW(DS_race_type) == 0 ? 0x444u : 0x777u));
    race_record_rnd();
    logf_("restarted opp %u type %u bet %u rnd %u %u %u", rec.opp, rec.type, rec.bet_kind,
          rec.rnd[0], rec.rnd[1], rec.rnd[2]);
    log_start();
    race_start();
    if (after_start_hook) after_start_hook();
    flow_clock_add(DSW(DS_race_type) == 0 ? 0x444 : 0x777);   /* this try's time, as challenge() adds it */
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

/* SR_RETRY_FORCE=a,b,..: the result of race n (1-based) is the n-th entry (hex player_result, W = win). */
static int race_no;

void retry_debug_race_after(void)
{
    const char *v = getenv("SR_RETRY_FORCE");
    int n = race_no++;
    if (!v || !*v) return;
    for (; n > 0 && v; n--) { v = strchr(v, ','); if (v) v++; }
    if (!v || !*v || *v == ',') return;
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
    after_start_hook = retry_debug_race_after;
    while (retry_offer(r)) {                          /* as bob_drive_in */
        retry_restart_race();
        r = (s16)race_results();
    }
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
    } else {
        fprintf(stderr, "debug retry: unknown case %s\n", c);
        rc = 2;
    }
    exit(rc);
}
