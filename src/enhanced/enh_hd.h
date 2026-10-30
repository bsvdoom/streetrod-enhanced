#pragma once
/* HD pictures: cockpit, dashboard, sky and horizon sheets (ENHANCED.md,
 * docs/superpowers/specs/2026-10-30-hd-cockpit-design.md).
 * Loads Game/hd/cockpit.png, Game/hd/sheets_race.png, Game/hd/sheets_town.png. */
#include "types.h"

void enh_hd_init(bool enabled);
bool enh_hd_enabled(void);

/* Called by race_run when the cockpit descriptor is loaded */
void enh_hd_set_cockpit(u16 seg, u16 off);
void enh_hd_set_clock(u16 seg, u16 off, int dx, int dy);
void enh_hd_set_gate(u16 seg, u16 off, int dx, int dy);
void enh_hd_set_knob_type(int type);
void enh_hd_set_knob_pos(int x, int y);
void enh_hd_set_needle(int kind, int x0, int y0, int x1, int y1);

/* Overlays the HD cockpit on xrgb according to Section 5.1 */
void enh_hd_compose(u32 *xrgb, const u32 rgb[16], int res_scale);

/* Screen backgrounds: garage (0x408), bob (0x445), gas (0x442) */
void enh_hd_set_screen_pic(s16 id, u16 seg, u16 off);
bool enh_hd_has_screen(void);
void enh_hd_compose_screen(u32 *xrgb, int res_scale);

/* Sky and horizon sampling (Section 5.2): fills row j of hd_sky_buf if HD sheets are present */
void enh_hd_sky_begin(int w, int h, int sa);
bool enh_hd_sky_row(int j, int sy, double shift, bool town);
void enh_hd_sky_clear_span(int j, int i0, int i1);
void enh_hd_sky_clear_pixel(int j, int i);
u32  enh_hd_sky_pixel(int j, int i);
bool enh_hd_has_sky(void);
