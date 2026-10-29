#pragma once
/* Retry after a lost race (docs/superpowers/specs/2026-10-29-retry-mod-design.md). Not part of the
 * original: off unless --retry is given. Before each race the game is saved silently into a slot (default
 * 16, which the original's save and load screens never list); after a lost race the player may load it.
 * Only the options, the request flag and a copy of the slot file live here; the game state stays in mem[]. */
#include "types.h"

void retry_set_options(bool on, int slot);     /* main.c: slot clamped to 1..16; forced off in demo mode */
bool retry_enabled(void);
int  retry_slot(void);

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
/* Loads the checked buffer into the game (after a passed retry_check). */
void retry_apply(void);

void retry_request(void);                      /* bob_drive_in: the player chose Retry */
bool retry_take_request(void);                 /* game_loop: true once per request */

bool retry_debug_main(void);                   /* developer aid SR_DEBUG_RETRY=<case> */
