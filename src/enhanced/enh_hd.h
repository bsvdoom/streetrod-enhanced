#pragma once
/* HD pictures: cockpit, dashboard, sky and horizon sheets (ENHANCED.md,
 * docs/superpowers/specs/2026-10-30-hd-cockpit-design.md).
 * Loads hd/<name>.png from the game folder (an override), else from beside the program (assets/hd in the
 * source, copied there by the build and shipped in the release). */
#include "types.h"

void enh_hd_init(bool enabled);
bool enh_hd_enabled(void);

/* Called by race_run when the cockpit descriptor is loaded */
/* Cockpit active state */
void enh_hd_set_cockpit(u16 seg, u16 off);
void enh_hd_clear_cockpit(void);
bool enh_hd_has_cockpit(void);
void enh_hd_set_clock(u16 seg, u16 off, int dx, int dy);
void enh_hd_set_gate(u16 seg, u16 off, int dx, int dy);
void enh_hd_set_wheel_knob(int x, int y);       /* wheel_draw: the red knob on the rim (screen x, y) */
void enh_hd_set_wheel(int idx);                  /* wheel_draw: 0 straight, 1 turned left, 2 right */
void enh_hd_set_knob_type(int type);
void enh_hd_set_knob_pos(int x, int y);
void enh_hd_set_needle(int kind, int x0, int y0, int x1, int y1);
void enh_hd_set_clock_hand(int minute, int x0, int y0, int x1, int y1);   /* clock_hand: base -> tip */
void enh_hd_set_crashed(bool crashed);

/* The dialogs and menus on the game's UI stack, [x0, x1) x [y0, y1) in original pixels: the original's
 * picture stays inside them over every enhanced layer. */
typedef struct { int x0, y0, x1, y1; } EnhRect;
int  enh_hd_dialogs(EnhRect out[4]);
bool enh_hd_in_rects(const EnhRect *r, int n, int x, int y);

/* 320 x 190 masks, NULL without the HD cockpit: where the HD cockpit is clear (the windshield, the mirror),
 * and the original cockpit picture's colours. Where the HD cockpit is clear and the shown pixel is still the
 * original cockpit's, the enhanced view shows through (the HD hood is lower than the original's). */
const u8 *enh_hd_window(void);
const u8 *enh_hd_orig_cockpit(void);

/* Overlays the HD cockpit on xrgb according to Section 5.1 */
/* live: the road view is drawn this frame (the side mirror copies it); otherwise the side mirror keeps the
 * last copy, as the original only copies it while it draws the road */
void enh_hd_compose(u32 *xrgb, const u32 rgb[16], int res_scale, bool live);

/* show_picture: a full-screen picture ends the cockpit */
void enh_hd_set_screen_pic(s16 id, u16 seg, u16 off);

/* Sky and horizon sampling (Section 5.2): fills row j of hd_sky_buf if HD sheets are present */
void enh_hd_sky_begin(int w, int h, int sa);
bool enh_hd_sky_row(int j, int sy, double shift, bool town);
void enh_hd_sky_clear_span(int j, int i0, int i1);
void enh_hd_sky_clear_pixel(int j, int i);
u32  enh_hd_sky_pixel(int j, int i);
u32 *enh_hd_sky_buffer(void);
const u8 *enh_hd_sky_live(void);                /* per sample row: 0 = no HD sky in it (skip it) */
void enh_hd_sky_copy_row(int j, int from);
bool enh_hd_has_sky(void);
