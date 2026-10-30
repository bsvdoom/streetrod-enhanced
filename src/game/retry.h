#pragma once
/* Retry after a lost race (docs/superpowers/specs/2026-10-29-retry-mod-design.md). Not part of the
 * original: off unless --retry is given. Before each race the game is saved silently into a slot (default
 * 16, which the original's save and load screens never list); after a lost race the player may load it.
 * Only the options, the request flag and a copy of the slot file live here; the game state stays in mem[]. */
#include "types.h"

void retry_set_options(bool on, int slot);     /* main.c: slot clamped to 1..16; forced off in demo mode */
bool retry_enabled(void);
int  retry_slot(void);
bool retry_is_dialog_active(void);

void retry_before_race(void);                  /* the silent save into the slot (logs, remembers success) */

typedef enum {
    RO_WON, RO_JUMPED_LIGHT, RO_POLICE, RO_JAIL, RO_LOST, RO_CRASH, RO_BREAKDOWN, RO_KING_LOST
} RetryOutcome;
/* The outcome of the race just finished, from DS_player_won, DS_player_result, DS_vs_king and the
 * return value r of race_results (1..4). */
RetryOutcome retry_classify(s16 r);
const char  *retry_outcome_name(RetryOutcome o);
bool         retry_is_loss(RetryOutcome o);

/* Reads the slot file into a buffer and checks its structure; touches no game state. */
bool retry_check(void);
/* Loads the checked buffer into the game (after a passed retry_check). False (a defect only): the game state
 * was put back as it was. */
bool retry_apply(void);

/* bob_drive_in, right after race_results() returned r: after a lost race (and a successful save before
 * it) asks "Retry" / "Go on". True = Retry chosen and the slot checked: the caller calls
 * retry_restart_race() and evaluates that race in turn. A failed check shows "Retry failed !" and returns
 * false. */
bool retry_offer(s16 r);
/* The state before the race, then the same race again (same opponent, kind, bet, random-number state);
 * adds the race's game time as challenge() does. */
void retry_restart_race(void);

/* Developer aids for the race path of SR_DEBUG_RACE (hooks_race.c): before / after the race. */
void retry_debug_race_before(void);
void retry_debug_race_after(void);
void retry_debug_race_done(s16 r);             /* the offer (and the load) as bob_drive_in would */
void retry_debug_race_before_bob(void);        /* SR_DEBUG_RACE=bob: the state line before Bob's */
void retry_debug_state(const char *when);      /* "debug race: <when> money .. drag .. road .." */

bool retry_debug_main(void);                   /* developer aid SR_DEBUG_RETRY=<case> */
