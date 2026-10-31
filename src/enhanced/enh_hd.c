/* HD pictures implementation: asset loading, cockpit compositing, HD sky/horizon sheets. */
#include "enhanced/enh_hd.h"
#include "enhanced/enh_internal.h"
#include "enhanced/enhanced.h"
#include "host.h"
#include "mem.h"
#include "platform/ega.h"
#include "platform/video.h"
#include "game/ui.h"

#ifndef DS_ptr_bank
#define DS_ptr_bank       0x7AFC
#define DS_ptr_bank_mask  0x7B00
#define DS_ptr_shape_off  0x7B04
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#include "third_party/stb_image.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    int w, h;
    u32 *pixels; /* 0xAARRGGBB */
} ImageHD;

static bool hd_opt_enabled = true;
static ImageHD img_cockpit;
static ImageHD img_sheets_race;
static ImageHD img_sheets_town;
static ImageHD img_plate[3]; /* 0: auto, 1: 3-speed, 2: 4-speed */
static ImageHD img_knob[3];  /* 0: auto, 1: 3-speed, 2: 4-speed */
static ImageHD img_crash;
static ImageHD img_wheel[3];
static ImageHD img_clock;     /* the clock face of automatic cars, 260 x 260 around the tachometer's centre */
#define CLOCK_CX 581          /* the tachometer's centre in cockpit.png (1280 x 760) */
#define CLOCK_CY 569  /* the wheel straight (in the cockpit picture), turned left, turned right */

static bool hd_cockpit_active = false;
static bool hd_crashed = false;
static bool hd_clock = false;
static int  hd_wheel = 0;               /* wheel_draw's picture: 0 straight, 1 left, 2 right */           /* an automatic car: the clock instead of the tachometer */
static int  hd_knob_type = 2;
static int  hd_knob_x = 0, hd_knob_y = 0;
static bool hd_knob_active = false;

static int  hd_wheel_knob_x, hd_wheel_knob_y;
static bool hd_wheel_knob = false;

void enh_hd_set_wheel_knob(int x, int y)
{
    hd_wheel_knob_x = x;
    hd_wheel_knob_y = y;
    hd_wheel_knob = true;
}

void enh_hd_set_wheel(int idx)
{
    hd_wheel = idx >= 0 && idx <= 2 ? idx : 0;
}

void enh_hd_set_knob_type(int type)
{
    hd_knob_type = (type >= 0 && type <= 2) ? type : 2;
}

void enh_hd_set_knob_pos(int x, int y)
{
    hd_knob_x = x;
    hd_knob_y = y;
    hd_knob_active = true;
}

void enh_hd_set_crashed(bool crashed)
{
    hd_crashed = crashed;
}

/* Clean original cockpit palette indices (320x190) from the game's descriptors */
static u8 orig_cockpit[190][320];
static bool has_orig_cockpit = false;
static u8 hd_window[190][320];                 /* 1: the HD cockpit is (partly) clear here */

/* Rear-view mirror cutout cache to retain scenic view when dialog covers top mirror */
static u32 mirror_cache[32 * 8 * 19 * 8];
static bool mirror_cache_valid = false;
static int mirror_cache_mw = 0;
static int mirror_cache_mh = 0;

void enh_hd_clear_cockpit(void)
{
    hd_cockpit_active = false;
    has_orig_cockpit = false;
    hd_crashed = false;
    mirror_cache_valid = false;
}

bool enh_hd_has_cockpit(void)
{
    return hd_opt_enabled && hd_cockpit_active && img_cockpit.pixels != NULL && has_orig_cockpit;
}

typedef struct {
    float x0, y0; /* Tip in 320x190 coords */
    float x1, y1; /* Tail/hub in 320x190 coords */
    bool active;
} NeedleHD;

static NeedleHD hd_needles[2]; /* 0: speedo, 1: tach */
static NeedleHD hd_hands[2];   /* the clock's hour and minute hands (base -> tip) */

void enh_hd_set_clock_hand(int minute, int x0, int y0, int x1, int y1)
{
    NeedleHD *h = &hd_hands[minute ? 1 : 0];
    h->x0 = (float)x0; h->y0 = (float)y0; h->x1 = (float)x1; h->y1 = (float)y1;
    h->active = true;
}

void enh_hd_set_needle(int kind, int x0, int y0, int x1, int y1)
{
    if (kind < 0 || kind > 1) return;
    hd_needles[kind].x0 = (float)x0;
    hd_needles[kind].y0 = (float)y0;
    hd_needles[kind].x1 = (float)x1;
    hd_needles[kind].y1 = (float)y1;
    hd_needles[kind].active = true;
}

/* The dialogs of the frame being composited: the drawn overlays (needles, the wheel's knob, the shifter,
 * the clock's hands) never go over them. Set by enh_hd_compose. */
static EnhRect ov_dlg[4];
static int ov_ndlg, ov_scale = 1;
static inline bool ov_hidden(int X, int Y)
{
    return ov_ndlg && enh_hd_in_rects(ov_dlg, ov_ndlg, X / ov_scale, Y / ov_scale);
}

/* Anti-aliased capsule line drawing at host resolution */
static void draw_capsule_line(u32 *xrgb, int W, int H,
                              float x0, float y0, float r0,
                              float x1, float y1, float r1,
                              u32 color)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len_sq = dx * dx + dy * dy;
    if (len_sq < 1e-4f) return;

    float max_r = fmaxf(r0, r1);
    int min_x = SDL_max(0, (int)floorf(fminf(x0, x1) - max_r - 1.0f));
    int max_x = SDL_min(W - 1, (int)ceilf(fmaxf(x0, x1) + max_r + 1.0f));
    int min_y = SDL_max(0, (int)floorf(fminf(y0, y1) - max_r - 1.0f));
    int max_y = SDL_min(H - 1, (int)ceilf(fmaxf(y0, y1) + max_r + 1.0f));

    u32 cr = (color >> 16) & 0xFF;
    u32 cg = (color >> 8)  & 0xFF;
    u32 cb = color & 0xFF;

    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        u32 *row = xrgb + (size_t)y * W;
        for (int x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;
            float vx = px - x0;
            float vy = py - y0;
            float t = (vx * dx + vy * dy) / len_sq;
            t = SDL_clamp(t, 0.0f, 1.0f);

            float cx = x0 + t * dx;
            float cy = y0 + t * dy;
            float dist = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            float r_at_t = r0 + t * (r1 - r0);

            float alpha = SDL_clamp(r_at_t + 0.5f - dist, 0.0f, 1.0f);
            if (alpha <= 0.0f) continue;

            if (ov_hidden(x, y)) continue;
            u32 bg = row[x];
            u32 br = (bg >> 16) & 0xFF;
            u32 bg_g = (bg >> 8) & 0xFF;
            u32 bb = bg & 0xFF;

            u32 out_r = (u32)(cr * alpha + br * (1.0f - alpha) + 0.5f);
            u32 out_g = (u32)(cg * alpha + bg_g * (1.0f - alpha) + 0.5f);
            u32 out_b = (u32)(cb * alpha + bb * (1.0f - alpha) + 0.5f);
            row[x] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

static void draw_aa_circle(u32 *xrgb, int W, int H, float cx, float cy, float radius, u32 color)
{
    int min_x = SDL_max(0, (int)floorf(cx - radius - 1.0f));
    int max_x = SDL_min(W - 1, (int)ceilf(cx + radius + 1.0f));
    int min_y = SDL_max(0, (int)floorf(cy - radius - 1.0f));
    int max_y = SDL_min(H - 1, (int)ceilf(cy + radius + 1.0f));

    u32 cr = (color >> 16) & 0xFF;
    u32 cg = (color >> 8)  & 0xFF;
    u32 cb = color & 0xFF;

    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        u32 *row = xrgb + (size_t)y * W;
        for (int x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;
            float dist = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            float alpha = SDL_clamp(radius + 0.5f - dist, 0.0f, 1.0f);
            if (alpha <= 0.0f) continue;

            if (ov_hidden(x, y)) continue;
            u32 bg = row[x];
            u32 br = (bg >> 16) & 0xFF;
            u32 bg_g = (bg >> 8) & 0xFF;
            u32 bb = bg & 0xFF;

            u32 out_r = (u32)(cr * alpha + br * (1.0f - alpha) + 0.5f);
            u32 out_g = (u32)(cg * alpha + bg_g * (1.0f - alpha) + 0.5f);
            u32 out_b = (u32)(cb * alpha + bb * (1.0f - alpha) + 0.5f);
            row[x] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

/* HD sky sampling buffer, sized to enh_buf (w * h) */
static u32 *hd_sky_buf = NULL;
static u8 *hd_sky_live = NULL;          /* per sample row: 1 = it may hold HD sky (the ground rows hold none) */
static int hd_sky_w = 0, hd_sky_h = 0, hd_sky_sa = 0;

static u32 *load_image(const char *rel_path, int *w, int *h)
{
    size_t size = 0;
    void *buf = NULL;

    /* 1. Try host_game_path */
    char *path = host_game_path(rel_path, false);
    if (path) {
        buf = SDL_LoadFile(path, &size);
        host_free(path);
    }

    /* 2. Candidate paths */
    if (!buf) {
        const char *base = SDL_GetBasePath();
        const char *game = host_game_dir();
        char p[512];

        if (game) {
            SDL_snprintf(p, sizeof p, "%s/%s", game, rel_path);
            buf = SDL_LoadFile(p, &size);
            if (!buf) {
                const char *slash = strrchr(rel_path, '/');
                SDL_snprintf(p, sizeof p, "%s/%s", game, slash ? slash + 1 : rel_path);
                buf = SDL_LoadFile(p, &size);
            }
        }
        if (!buf && base) {
            SDL_snprintf(p, sizeof p, "%s%s", base, rel_path);
            buf = SDL_LoadFile(p, &size);
            if (!buf) {
                SDL_snprintf(p, sizeof p, "%sGame/%s", base, rel_path);
                buf = SDL_LoadFile(p, &size);
            }
            if (!buf) {
                SDL_snprintf(p, sizeof p, "%s../Game/%s", base, rel_path);
                buf = SDL_LoadFile(p, &size);
            }
        }
        if (!buf) {
            buf = SDL_LoadFile(rel_path, &size);
            if (!buf) {
                SDL_snprintf(p, sizeof p, "Game/%s", rel_path);
                buf = SDL_LoadFile(p, &size);
            }
        }
    }

    if (!buf || size == 0) {
        if (buf) SDL_free(buf);
        return NULL;
    }

    int comp = 0;
    stbi_uc *data = stbi_load_from_memory((const stbi_uc *)buf, (int)size, w, h, &comp, 4);
    SDL_free(buf);
    if (!data) return NULL;

    u32 *pixels = SDL_malloc((size_t)(*w) * (size_t)(*h) * sizeof(u32));
    if (!pixels) { stbi_image_free(data); return NULL; }

    for (int i = 0; i < (*w) * (*h); i++) {
        u8 r = data[i * 4 + 0];
        u8 g = data[i * 4 + 1];
        u8 b = data[i * 4 + 2];
        u8 a = data[i * 4 + 3];
        pixels[i] = ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b;
    }
    stbi_image_free(data);
    return pixels;
}

static void load_asset(const char *name, ImageHD *out)
{
    char path[128];
    SDL_snprintf(path, sizeof path, "hd/%s.png", name);
    out->pixels = load_image(path, &out->w, &out->h);
    if (!out->pixels) {
        SDL_snprintf(path, sizeof path, "hd/%s.bmp", name);
        out->pixels = load_image(path, &out->w, &out->h);
    }
    if (!out->pixels) fprintf(stderr, "HD notice: %s not found (vanilla graphics used)\n", name);
}

void enh_hd_init(bool enabled)
{
    hd_opt_enabled = enabled;
}

static void ensure_loaded(void)
{
    static bool loaded = false;
    if (loaded || !hd_opt_enabled) return;
    loaded = true;
    load_asset("cockpit", &img_cockpit);
    load_asset("sheets_race", &img_sheets_race);
    load_asset("sheets_town", &img_sheets_town);
    load_asset("plate_auto", &img_plate[0]);
    load_asset("plate_3speed", &img_plate[1]);
    load_asset("plate_4speed", &img_plate[2]);
    load_asset("knob_auto", &img_knob[0]);
    load_asset("knob_3speed", &img_knob[1]);
    load_asset("knob_4speed", &img_knob[2]);
    load_asset("crash", &img_crash);
    load_asset("clock", &img_clock);
    load_asset("wheel_left", &img_wheel[1]);
    load_asset("wheel_right", &img_wheel[2]);
    if (img_cockpit.pixels) {                  /* the see-through pixels: any part of their block clear */
        int cw = img_cockpit.w, ch = img_cockpit.h;
        for (int y = 0; y < 190; y++)
            for (int x = 0; x < 320; x++) {
                bool clear = false;
                for (int hy = y * ch / 190; hy < (y + 1) * ch / 190 && !clear; hy++)
                    for (int hx = x * cw / 320; hx < (x + 1) * cw / 320 && !clear; hx++)
                        clear = (img_cockpit.pixels[(size_t)hy * cw + hx] >> 24) == 0;
                hd_window[y][x] = clear;
            }
    }
}

const u8 *enh_hd_window(void)
{
    return enh_hd_has_cockpit() ? &hd_window[0][0] : NULL;
}

const u8 *enh_hd_orig_cockpit(void)
{
    return &orig_cockpit[0][0];
}

void enh_hd_set_screen_pic(s16 id, u16 seg, u16 off)
{
    enh_hd_clear_cockpit();
    (void)id; (void)seg; (void)off;
}

bool enh_hd_enabled(void)
{
    ensure_loaded();
    return hd_opt_enabled && (img_cockpit.pixels != NULL || img_sheets_race.pixels != NULL);
}

void enh_hd_set_cockpit(u16 seg, u16 off)
{
    ensure_loaded();
    hd_cockpit_active = true;
    hd_crashed = false;
    hd_clock = false;
    hd_wheel = 0;
    hd_wheel_knob = false;
    hd_knob_active = false;
    hd_needles[0].active = hd_needles[1].active = false;
    hd_hands[0].active = hd_hands[1].active = false;
    FarPtr d = { off, seg };
    if (far_is_null(d)) return;
    int w = desc_w(d), h = desc_h(d), stride = desc_stride(d);
    if (w != 320 || h != 190) return;

    FarPtr pl = desc_planes(d);
    for (int y = 0; y < 190; y++) {
        for (int x = 0; x < 320; x++) {
            u16 o = (u16)(y * stride + (x >> 3));
            int bit = 7 - (x & 7), c = 0;
            for (int k = 0; k < 4; k++)
                c |= (rd8(pl.seg, (u16)(pl.off + k * stride * h + o)) >> bit & 1) << k;
            orig_cockpit[y][x] = (u8)c;
        }
    }
    has_orig_cockpit = true;
}

/* a picture blitted into the cockpit picture: into the copy of its colours as well */
static void blit_into_cockpit(u16 seg, u16 off, int dx, int dy)
{
    if (!has_orig_cockpit) return;
    FarPtr d = { off, seg };
    if (far_is_null(d)) return;
    int w = desc_w(d), h = desc_h(d), stride = desc_stride(d);
    FarPtr pl = desc_planes(d), mk = desc_mask(d);

    for (int y = 0; y < h && (dy + y) < 190; y++) {
        for (int x = 0; x < w && (dx + x) < 320; x++) {
            u16 o = (u16)(y * stride + (x >> 3));
            int bit = 7 - (x & 7);
            if (!far_is_null(mk) && (rd8(mk.seg, (u16)(mk.off + o)) >> bit & 1)) continue;
            int c = 0;
            for (int k = 0; k < 4; k++)
                c |= (rd8(pl.seg, (u16)(pl.off + k * stride * h + o)) >> bit & 1) << k;
            orig_cockpit[dy + y][dx + x] = (u8)c;
        }
    }
}

/* The clock face of automatic cars sits where the tachometer is: there the original's clock stays (the HD
 * picture has a tachometer, and these cars have no tachometer needle). */
void enh_hd_set_clock(u16 seg, u16 off, int dx, int dy)
{
    blit_into_cockpit(seg, off, dx, dy);
    hd_clock = true;
}

void enh_hd_set_gate(u16 seg, u16 off, int dx, int dy)
{
    blit_into_cockpit(seg, off, dx, dy);
}

static void enh_hd_draw_cursor(u32 *xrgb, const u32 rgb[16], int res_scale)
{
    if (DSC(DS_g_ptr_show) <= 0) return;
    s16 x = DSS(DS_cursor_x);
    s16 y = DSS(DS_cursor_y);
    if (x < 0) x = 0; else if (x > 319) x = 319;
    if (y < 0) y = 0; else if (y > 199) y = 199;

    u8 sh = (u8)DSB(DS_g_px_shift);
    s16 px = (s16)((u16)x & (u16)DSW(DS_g_x_align));
    s16 w = (s16)(320 - px); if (w > 24) w = 24;
    s16 h = (s16)(200 - y); if (h > 16) h = 16;
    u16 off = (u16)((u16)((s16)((u16)x & (u16)~DSW(DS_g_x_align)) * (s16)((s16)24 >> sh))
                    + DSW(DS_ptr_shape_off));
    FarPtr img = ds_far(DS_ptr_bank);
    FarPtr msk = ds_far(DS_ptr_bank_mask);
    if (far_is_null(img) || far_is_null(msk)) return;

    int W = 320 * res_scale;
    for (int r = 0; r < h; r++) {
        int sy = y + r;
        if (sy >= 200) break;
        for (int c = 0; c < w; c++) {
            int sx = px + c;
            if (sx >= 320) break;

            int byte_idx = c >> 3;
            int bit = 7 - (c & 7);
            u16 m_off = (u16)(off + msk.off + r * 24 + byte_idx);
            u8 m_byte = rd8(msk.seg, m_off);
            if (!((m_byte >> bit) & 1)) continue;

            u16 s_off = (u16)(off + img.off + r * 24 + byte_idx);
            int col_idx = ((rd8(img.seg, s_off) >> bit) & 1) |
                          (((rd8(img.seg, (u16)(s_off + 0x600)) >> bit) & 1) << 1) |
                          (((rd8(img.seg, (u16)(s_off + 0xC00)) >> bit) & 1) << 2) |
                          (((rd8(img.seg, (u16)(s_off + 0x1200)) >> bit) & 1) << 3);
            u32 col = rgb ? rgb[col_idx] : 0xFFFFFF;

            for (int dy = 0; dy < res_scale; dy++) {
                int Y = sy * res_scale + dy;
                if (Y >= 200 * res_scale) break;
                u32 *row_out = xrgb + (size_t)Y * W;
                for (int dx = 0; dx < res_scale; dx++) {
                    int X = sx * res_scale + dx;
                    if (X < W) row_out[X] = col;
                }
            }
        }
    }
}

#define UI_DEPTH 0x8B8E                /* ui.h: the UI stack's depth (max 4) and its screens */
#define UI_STACK 0x8B86

/* ui_bounds (ui_menu.c) without its writes to DGROUP: the rectangles [x0, x1) x [y0, y1) of the screens on
 * the UI stack that have records (the dialogs and menus); the screens without (the "click anywhere" waits)
 * cover nothing. Inside them the original's picture stays over every enhanced layer. */
int enh_hd_dialogs(EnhRect out[4])
{
    int n = 0, depth = DSS(UI_DEPTH);
    for (int d = 0; d < depth && d < 4; d++) {
        u16 rec = ui_screen_rec(DSS((u16)(UI_STACK + 2 * d)));
        int x0 = 1000, y0 = 1000, x1 = 1, y1 = 1;
        for (int guard = 0; DSW((u16)(rec + UI_R_NEXT)) != 0 && guard < 256; guard++) {
            rec = UI_REC(DSW((u16)(rec + UI_R_NEXT)));
            x0 = SDL_min(x0, DSS((u16)(rec + UI_R_X0)));
            x1 = SDL_max(x1, DSS((u16)(rec + UI_R_X1)));
            y0 = SDL_min(y0, DSS((u16)(rec + UI_R_Y0)));
            y1 = SDL_max(y1, DSS((u16)(rec + UI_R_Y1)));
        }
        x0 = SDL_max(x0 - 3, 0);
        y0 = SDL_max(y0 - 3, 0);
        x1 = SDL_min(x1 + 3, 320);
        y1 = SDL_min(y1 + 3, 200);
        if (x1 > x0 && y1 > y0) out[n++] = (EnhRect){ x0, y0, x1, y1 };
    }
    return n;
}

bool enh_hd_in_rects(const EnhRect *r, int n, int x, int y)
{
    for (int i = 0; i < n; i++)
        if (x >= r[i].x0 && x < r[i].x1 && y >= r[i].y0 && y < r[i].y1) return true;
    return false;
}

/* enh_hd_compose's state for the parallel parts */
typedef struct {
    u32 *xrgb;
    int W, res_scale, line_compare;
    u16 start_addr;
    const u8 *p[4];
    int cw, ch;
    int c6[3], c7[3];          /* the paint: palette registers 6 and 7 */
    EnhRect dlg[4];
    int ndlg;
} ComposeCtx;

/* Output column / row -> cockpit picture column / row: once per frame, not per output pixel. */
static int hx_of[320 * ENH_MAX_RES_SCALE], hy_of[190 * ENH_MAX_RES_SCALE];

/* the cockpit rows y_from..y_to-1 (original rows) */
static void compose_rows(int y_from, int y_to, void *vctx)
{
    const ComposeCtx *c = vctx;
    u32 *xrgb = c->xrgb;
    int W = c->W, res_scale = c->res_scale, line_compare = c->line_compare, cw = c->cw, ch = c->ch;
    u16 start_addr = c->start_addr;
    const u8 *p0 = c->p[0], *p1 = c->p[1], *p2 = c->p[2], *p3 = c->p[3];
    int c6_r = c->c6[0], c6_g = c->c6[1], c6_b = c->c6[2], c7_r = c->c7[0], c7_g = c->c7[1], c7_b = c->c7[2];
    const EnhRect *dlg = c->dlg;
    int ndlg = c->ndlg;
    for (int y = y_from; y < y_to; y++) {
        u16 row = y < line_compare ? (u16)(start_addr + y * EGA_BYTES_PER_LINE)
                                   : (u16)((y - line_compare) * EGA_BYTES_PER_LINE);
        /* Dynamic hood/fender recoloring applies right of this column on rows 96..113 */
        int hood_min_x = (y >= 96 && y < 114) ? 72 + ((114 - y) * 26) / 16 : 320;
        for (int x = 0; x < 320; x++) {
            u16 o = (u16)(row + (x >> 3));
            int bit = 7 - (x & 7);
            int ega_c = ((p0[o] >> bit & 1)     ) |
                        ((p1[o] >> bit & 1) << 1) |
                        ((p2[o] >> bit & 1) << 2) |
                        ((p3[o] >> bit & 1) << 3);

            u8 orig_c = orig_cockpit[y][x];

            /* Section 5.1 rule: if the shown EGA pixel differs from original picture,
             * something dynamic is there (needles, shifter, knob, crash sprite) -> keep it!
             * The side mirror area (8..39, 103..121) is replaced by the HD cutout above.
             * Dials: tach at (145, 142), speedo at (196, 142) with radius 28 px are replaced by HD dials + vector needles.
             * Steering wheel spokes: (80..263, 163..189) are kept clean HD; knob at top rim remains dynamic. */
            bool in_side_mirror = (x >= 8 && x < 40 && y >= 103 && y < 122);
            int dtx = x - 145, dty = y - 142;
            int dsx = x - 196, dsy = y - 142;
            bool in_tach = dtx * dtx + dty * dty <= 27 * 27;
            bool hd_face = in_tach && hd_clock && img_clock.pixels;
            if (in_tach && hd_clock && !hd_face) continue;      /* no HD face: the original's clock */
            bool in_dial = in_tach || (dsx * dsx + dsy * dsy <= 27 * 27);
            bool in_wheel_spokes = (x >= 80 && x < 264 && y >= 163 && y < 190);
            bool in_shifter = (x >= 264 && x < 312 && y >= 123 && y < 166);
            bool in_hood = (x >= 50 && y >= 70);
            if (enh_hd_in_rects(dlg, ndlg, x, y)) continue;    /* a dialog: the original's picture */
            if (ega_c != orig_c && !in_hood && !in_side_mirror && !in_dial && !in_wheel_spokes && !in_shifter) continue;

            bool hood = x >= hood_min_x;
            for (int oy = 0; oy < res_scale; oy++) {
                int Y = y * res_scale + oy;
                const u32 *hd_row = img_cockpit.pixels + (size_t)hy_of[Y] * cw;
                const ImageHD *wimg = in_wheel_spokes && hd_wheel ? &img_wheel[hd_wheel] : NULL;
                int wy = 0, wx0 = 0;
                if (wimg && wimg->pixels) {                       /* the turned wheel's strip */
                    wy = hy_of[Y] - 163 * ch / 190;
                    wx0 = 80 * cw / 320;
                    if (wy < 0 || wy >= wimg->h) wimg = NULL;
                } else {
                    wimg = NULL;
                }
                u32 *out = xrgb + (size_t)Y * W;

                for (int ox = 0; ox < res_scale; ox++) {
                    int X = x * res_scale + ox;
                    u32 px = hd_row[hx_of[X]];
                    if (hd_face) {                                /* the HD clock face over it */
                        int kx = hx_of[X] - (CLOCK_CX - img_clock.w / 2);
                        int ky = hy_of[Y] - (CLOCK_CY - img_clock.h / 2);
                        if (kx >= 0 && kx < img_clock.w && ky >= 0 && ky < img_clock.h) {
                            u32 k = img_clock.pixels[(size_t)ky * img_clock.w + kx];
                            u32 ka = k >> 24;
                            if (ka == 255) px = k;
                            else if (ka) {
                                u32 r = (((k >> 16) & 0xFF) * ka + ((px >> 16) & 0xFF) * (255 - ka)) / 255;
                                u32 g = (((k >> 8) & 0xFF) * ka + ((px >> 8) & 0xFF) * (255 - ka)) / 255;
                                u32 b = ((k & 0xFF) * ka + (px & 0xFF) * (255 - ka)) / 255;
                                px = 0xFF000000u | r << 16 | g << 8 | b;
                            }
                        }
                    }
                    if (wimg) {
                        int wx = hx_of[X] - wx0;
                        if (wx >= 0 && wx < wimg->w) px = wimg->pixels[(size_t)wy * wimg->w + wx];
                    }
                    u8 a = (u8)(px >> 24);
                    if (a == 0) continue; /* Transparent: windshield/mirror road view */

                    /* Dynamic hood/fender recoloring matching player car paint */
                    if (hood) {
                        int pr = (int)((px >> 16) & 0xFF);
                        int pg = (int)((px >> 8) & 0xFF);
                        int pb = (int)(px & 0xFF);
                        int lum = (299 * pr + 587 * pg + 114 * pb) / 1000;
                        bool in_wheel = (Y > 105 * res_scale && X > 105 * res_scale && X < 240 * res_scale &&
                                         abs(pr - pg) < 6 && abs(pg - pb) < 6);
                        if (!in_wheel && (pb > pr + 4) && (pb > pg + 2) && lum < 160) {
                            int t = (lum * 256) / 45;
                            if (t > 512) t = 512;
                            int nr, ng, nb;
                            if (t < 256) {
                                int factor = 90 + ((166 * t) >> 8);
                                nr = (c6_r * factor) >> 8;
                                ng = (c6_g * factor) >> 8;
                                nb = (c6_b * factor) >> 8;
                            } else {
                                int t2 = t - 256;
                                nr = (c6_r * (256 - t2) + c7_r * t2) >> 8;
                                ng = (c6_g * (256 - t2) + c7_g * t2) >> 8;
                                nb = (c6_b * (256 - t2) + c7_b * t2) >> 8;
                            }
                            if (nr > 255) nr = 255;
                            if (ng > 255) ng = 255;
                            if (nb > 255) nb = 255;
                            px = ((u32)nr << 16) | ((u32)ng << 8) | (u32)nb;
                        }
                    }

                    out[X] = px & 0xFFFFFF;
                }
            }
        }
    }

}

/* the crack layer over output rows of original rows y_from..y_to-1 */
static void crack_rows(int y_from, int y_to, void *vctx)
{
    const ComposeCtx *c = vctx;
    int W = c->W, res_scale = c->res_scale;
    int cw2 = img_crash.w, ch2 = img_crash.h;
    for (int Y = y_from * res_scale; Y < y_to * res_scale; Y++) {
        int y = Y / res_scale;
        int cy = (Y * ch2) / (190 * res_scale);
        if (cy >= ch2) cy = ch2 - 1;
        const u32 *c_row = img_crash.pixels + (size_t)cy * cw2;
        u32 *out_row = c->xrgb + (size_t)Y * W;
        for (int X = 0; X < W; X++) {
            int x = X / res_scale;
            if (enh_hd_in_rects(c->dlg, c->ndlg, x, y)) continue;   /* never over a dialog */
            int cx = (X * cw2) / W;
            if (cx >= cw2) cx = cw2 - 1;
            u32 cp = c_row[cx];
            u32 a = (cp >> 24) & 0xFF;
            if (a == 0) continue;
            u32 bg = out_row[X];
            u32 br = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bb = bg & 0xFF;
            u32 cr = (cp >> 16) & 0xFF, cg = (cp >> 8) & 0xFF, cb = cp & 0xFF;
            u32 nr = (cr * a + br * (255 - a)) / 255;
            u32 ng = (cg * a + bg_g * (255 - a)) / 255;
            u32 nb = (cb * a + bb * (255 - a)) / 255;
            out_row[X] = (nr << 16) | (ng << 8) | nb;
        }
    }
}

bool enh_hd_cockpit_shown(void)
{
    return hd_cockpit_active && has_orig_cockpit;
}

/* The side mirror (race_road.c: the original copies the rear-view mirror's (248, 22) 32 x 19 to (8, 103)
 * while it draws the road): the enhanced view's copy, live while the road is drawn, else the last one. */
void enh_hd_side_mirror(u32 *xrgb, int res_scale, bool live)
{
    if (!enh_hd_cockpit_shown()) return;
    int W = 320 * res_scale;
    EnhRect dlg[4];
    int ndlg = enh_hd_dialogs(dlg);
    bool top_mirror_covered = false;           /* a dialog over the part of the mirror the side mirror shows */
    for (int i = 0; i < ndlg; i++)
        if (dlg[i].x0 < 280 && dlg[i].x1 > 248 && dlg[i].y0 < 41 && dlg[i].y1 > 22) top_mirror_covered = true;
    for (int i = 0; i < ndlg; i++)                 /* a dialog over the side mirror itself: leave it */
        if (dlg[i].x0 < 40 && dlg[i].x1 > 8 && dlg[i].y0 < 122 && dlg[i].y1 > 103) return;

    int sx0 = 248 * res_scale, sy0 = 22 * res_scale;
    int dx0 = 8 * res_scale,   dy0 = 103 * res_scale;
    int mw  = 32 * res_scale,  mh  = 19 * res_scale;
    if (live && !top_mirror_covered) {
        if (mw * mh <= (int)(sizeof(mirror_cache) / sizeof(mirror_cache[0]))) {
            for (int y = 0; y < mh; y++) {
                memcpy(mirror_cache + y * mw,
                       xrgb + (size_t)(sy0 + y) * W + sx0,
                       (size_t)mw * sizeof(u32));
                memcpy(xrgb + (size_t)(dy0 + y) * W + dx0,
                       xrgb + (size_t)(sy0 + y) * W + sx0,
                       (size_t)mw * sizeof(u32));
            }
            mirror_cache_valid = true;
            mirror_cache_mw = mw;
            mirror_cache_mh = mh;
        }
    } else if (mirror_cache_valid && mirror_cache_mw == mw && mirror_cache_mh == mh) {
        for (int y = 0; y < mh; y++) {
            memcpy(xrgb + (size_t)(dy0 + y) * W + dx0,
                   mirror_cache + y * mw,
                   (size_t)mw * sizeof(u32));
        }
    }
}

void enh_hd_compose(u32 *xrgb, const u32 rgb[16], int res_scale)
{
    ensure_loaded();
    if (!hd_opt_enabled || !img_cockpit.pixels || !has_orig_cockpit) return;

    int W = 320 * res_scale;
    int line_compare = ega_line_compare();
    u16 start_addr = ega_start();
    const u8 *p0 = ega_plane(0);
    const u8 *p1 = ega_plane(1);
    const u8 *p2 = ega_plane(2);
    const u8 *p3 = ega_plane(3);

    int cw = img_cockpit.w, ch = img_cockpit.h;

    /* Body paint colors from EGA palette registers 6 and 7 */
    u32 c6_raw = rgb ? rgb[6] : 0x0000AA;
    u32 c7_raw = rgb ? rgb[7] : 0x5555FF;
    int c6_r = (int)((c6_raw >> 16) & 0xFF);
    int c6_g = (int)((c6_raw >> 8) & 0xFF);
    int c6_b = (int)(c6_raw & 0xFF);
    int c7_r = (int)((c7_raw >> 16) & 0xFF);
    int c7_g = (int)((c7_raw >> 8) & 0xFF);
    int c7_b = (int)(c7_raw & 0xFF);

    EnhRect dlg[4];
    int ndlg = enh_hd_dialogs(dlg);
    for (int i = 0; i < ndlg; i++) ov_dlg[i] = dlg[i];
    ov_ndlg = ndlg;
    ov_scale = res_scale;
    for (int X = 0; X < W; X++) {
        int hx = (X * cw) / W;
        hx_of[X] = hx < cw ? hx : cw - 1;
    }
    for (int Y = 0; Y < 190 * res_scale; Y++) {
        int hy = (Y * ch) / (190 * res_scale);
        hy_of[Y] = hy < ch ? hy : ch - 1;
    }

    ComposeCtx cc = { xrgb, W, res_scale, line_compare, start_addr, { p0, p1, p2, p3 }, cw, ch,
                      { c6_r, c6_g, c6_b }, { c7_r, c7_g, c7_b }, { dlg[0], dlg[1], dlg[2], dlg[3] }, ndlg };
    enh_parallel_rows(190, compose_rows, &cc);

    /* Render smooth anti-aliased vector needles over the clean HD dials */
    float sc = (float)res_scale;
    float r_tip  = 1.1f * (sc / 4.0f);
    float r_tail = 2.4f * (sc / 4.0f);
    float r_hub  = 4.2f * (sc / 4.0f);
    float r_pin  = 1.8f * (sc / 4.0f);

    for (int k = 0; k < 2; k++) {
        if (!hd_needles[k].active) continue;
        float x0 = hd_needles[k].x0 * sc;
        float y0 = hd_needles[k].y0 * sc;
        float x1 = hd_needles[k].x1 * sc;
        float y1 = hd_needles[k].y1 * sc;

        /* Needle body (tapered capsule) in bright needle red */
        draw_capsule_line(xrgb, W, 200 * res_scale, x0, y0, r_tip, x1, y1, r_tail, 0xFF2A1A);
        /* Subtle orange core highlight */
        draw_capsule_line(xrgb, W, 200 * res_scale, x0, y0, r_tip * 0.4f, x1, y1, r_tail * 0.4f, 0xFFAA44);
        /* Center hub chrome cap and dark pin */
        draw_aa_circle(xrgb, W, 200 * res_scale, x1, y1, r_hub, 0xD4D8E0);
        draw_aa_circle(xrgb, W, 200 * res_scale, x1, y1, r_pin, 0x24262A);
    }

    /* The wheel's knob (shape_draw_dot, 6 x 5 px centred on (x + 1, y + 0.5)): it shows which way the wheel
     * is turned; the HD cockpit covers the original's */
    if (hd_wheel_knob && !hd_crashed) {
        float kx = (hd_wheel_knob_x + 1.0f) * sc, ky = (hd_wheel_knob_y + 0.5f) * sc;
        draw_aa_circle(xrgb, W, 200 * res_scale, kx, ky, 3.0f * sc, 0x5A1010);
        draw_aa_circle(xrgb, W, 200 * res_scale, kx, ky, 2.5f * sc, 0xFF5555);
        draw_aa_circle(xrgb, W, 200 * res_scale, kx - 0.7f * sc, ky - 0.7f * sc, 0.8f * sc, 0xFFB0B0);
    }

    /* The clock's hands over the HD face (automatic cars) */
    if (hd_clock && img_clock.pixels) {
        for (int k = 0; k < 2; k++) {
            if (!hd_hands[k].active) continue;
            float hw = k ? 1.7f : 2.4f;
            draw_capsule_line(xrgb, W, 200 * res_scale, hd_hands[k].x0 * sc, hd_hands[k].y0 * sc, hw * (sc / 4.0f),
                              hd_hands[k].x1 * sc, hd_hands[k].y1 * sc, 0.9f * (sc / 4.0f), 0xF2F2F2);
        }
        if (hd_hands[0].active || hd_hands[1].active) {
            draw_aa_circle(xrgb, W, 200 * res_scale, 145.5f * sc, 142.5f * sc, r_hub, 0xD4D8E0);
            draw_aa_circle(xrgb, W, 200 * res_scale, 145.5f * sc, 142.5f * sc, r_pin, 0x24262A);
        }
    }

    /* Render HD shifter plate matching transmission type (0: auto, 1: 3-speed, 2: 4-speed) */
    if (img_plate[hd_knob_type].pixels) {
        const ImageHD *pimg = &img_plate[hd_knob_type];
        int pw = pimg->w, ph = pimg->h;
        int px0 = 264 * res_scale;
        int py0 = 123 * res_scale;

        for (int py = 0; py < ph; py++) {
            int Y = py0 + py;
            if (Y < 0 || Y >= 200 * res_scale) continue;
            u32 *out_row = xrgb + (size_t)Y * W;
            const u32 *p_row = pimg->pixels + (size_t)py * pw;

            for (int px = 0; px < pw; px++) {
                int X = px0 + px;
                if (X < 0 || X >= W) continue;
                if (ov_hidden(X, Y)) continue;
                u32 pp = p_row[px];
                u32 a = (pp >> 24) & 0xFF;
                if (a == 0) continue;

                if (a == 255) {
                    out_row[X] = pp & 0xFFFFFF;
                } else {
                    u32 bg = out_row[X];
                    u32 br = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bb = bg & 0xFF;
                    u32 pr = (pp >> 16) & 0xFF, pg = (pp >> 8) & 0xFF, pb = pp & 0xFF;
                    u32 nr = (pr * a + br * (255 - a)) / 255;
                    u32 ng = (pg * a + bg_g * (255 - a)) / 255;
                    u32 nb = (pb * a + bb * (255 - a)) / 255;
                    out_row[X] = (nr << 16) | (ng << 8) | nb;
                }
            }
        }
    }

    /* Render anti-aliased vector shift knob/handle over the HD plate */
    if (hd_knob_active && img_knob[hd_knob_type].pixels) {
        const ImageHD *kimg = &img_knob[hd_knob_type];
        int kw = kimg->w, kh = kimg->h;
        int kcx = (264 + hd_knob_x + 12) * res_scale;
        int kcy = (123 + hd_knob_y + (hd_knob_type == 0 ? 4 : 10)) * res_scale;
        int kx0 = kcx - kw / 2;
        int ky0 = kcy - kh / 2;

        for (int ky = 0; ky < kh; ky++) {
            int Y = ky0 + ky;
            if (Y < 0 || Y >= 200 * res_scale) continue;
            u32 *out_row = xrgb + (size_t)Y * W;
            const u32 *k_row = kimg->pixels + (size_t)ky * kw;

            for (int kx = 0; kx < kw; kx++) {
                int X = kx0 + kx;
                if (X < 0 || X >= W) continue;
                if (ov_hidden(X, Y)) continue;
                u32 kp = k_row[kx];
                u32 a = (kp >> 24) & 0xFF;
                if (a == 0) continue;

                if (a == 255) {
                    out_row[X] = kp & 0xFFFFFF;
                } else {
                    u32 bg = out_row[X];
                    u32 br = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bb = bg & 0xFF;
                    u32 kr = (kp >> 16) & 0xFF, kg = (kp >> 8) & 0xFF, kb = kp & 0xFF;
                    u32 nr = (kr * a + br * (255 - a)) / 255;
                    u32 ng = (kg * a + bg_g * (255 - a)) / 255;
                    u32 nb = (kb * a + bb * (255 - a)) / 255;
                    out_row[X] = (nr << 16) | (ng << 8) | nb;
                }
            }
        }
    }

    /* Render HD cracked windshield overlay on crash */
    if (hd_crashed && img_crash.pixels != NULL) enh_parallel_rows(190, crack_rows, &cc);

    /* Redraw mouse cursor on top of all HD composited layers (left mirror, dials, needles, shifter, cracks) */
    enh_hd_draw_cursor(xrgb, rgb, res_scale);
}

void enh_hd_sky_begin(int w, int h, int sa)
{
    ensure_loaded();
    if (!enh_hd_has_sky()) {                   /* nothing to sample: no buffer, the spans skip it */
        SDL_free(hd_sky_buf);
        SDL_free(hd_sky_live);
        hd_sky_buf = NULL;
        hd_sky_live = NULL;
        hd_sky_w = hd_sky_h = 0;
        return;
    }
    if (hd_sky_w != w || hd_sky_h != h) {
        SDL_free(hd_sky_buf);
        SDL_free(hd_sky_live);
        hd_sky_buf = SDL_malloc((size_t)w * (size_t)h * sizeof(u32));
        hd_sky_live = SDL_calloc((size_t)h, 1);
        hd_sky_w = w;
        hd_sky_h = h;
        if (hd_sky_buf) memset(hd_sky_buf, 0, (size_t)w * (size_t)h * sizeof(u32));
    }
    hd_sky_sa = sa;
}

bool enh_hd_sky_row(int j, int sy, double shift, bool town)
{
    if (!hd_opt_enabled || !hd_sky_buf || !hd_sky_live || j < 0 || j >= hd_sky_h) return false;

    const ImageHD *sheet = town ? (&img_sheets_town) : (&img_sheets_race);
    if (!sheet->pixels) sheet = town ? (&img_sheets_race) : (&img_sheets_town);
    if (!sheet->pixels) return false;

    if (sy < 0x66) sy = 0x66;
    if (sy > 0xB7) sy = 0xB7;

    int sw = sheet->w, sh = sheet->h;
    int hy = ((sy - 0x66) * sh) / 82;
    if (hy >= sh) hy = sh - 1;
    const u32 *src = sheet->pixels + (size_t)hy * sw;
    u32 *dst = hd_sky_buf + (size_t)j * hd_sky_w;
    hd_sky_live[j] = 1;

    for (int i = 0; i < hd_sky_w; i++) {
        double x = (i + 0.5) / hd_sky_sa - shift;
        double u = x / 320.0;
        int col = (int)floor(u * sw) % sw;
        if (col < 0) col += sw;
        dst[i] = src[col] & 0xFFFFFF;
    }
    return true;
}

void enh_hd_sky_clear_span(int j, int i0, int i1)
{
    if (!hd_sky_buf || !hd_sky_live || j < 0 || j >= hd_sky_h || i1 <= i0 || !hd_sky_live[j]) return;
    if (i0 < 0) i0 = 0;
    if (i1 > hd_sky_w) i1 = hd_sky_w;
    if (i1 > i0) {
        memset(hd_sky_buf + (size_t)j * hd_sky_w + i0, 0, (size_t)(i1 - i0) * sizeof(u32));
        if (i0 == 0 && i1 == hd_sky_w) hd_sky_live[j] = 0;          /* the whole row: no sky left */
    }
}

void enh_hd_sky_clear_pixel(int j, int i)
{
    if (!hd_sky_buf || !hd_sky_live || j < 0 || j >= hd_sky_h || i < 0 || i >= hd_sky_w || !hd_sky_live[j]) return;
    hd_sky_buf[j * hd_sky_w + i] = 0;
}

u32 enh_hd_sky_pixel(int j, int i)
{
    if (!hd_sky_buf || j < 0 || j >= hd_sky_h || i < 0 || i >= hd_sky_w) return 0;
    return hd_sky_buf[j * hd_sky_w + i];
}

u32 *enh_hd_sky_buffer(void)
{
    return hd_sky_buf;
}

const u8 *enh_hd_sky_live(void)
{
    return hd_sky_live;
}

void enh_hd_sky_copy_row(int j, int from)
{
    if (!hd_sky_buf || !hd_sky_live || j < 0 || j >= hd_sky_h || from < 0 || from >= hd_sky_h) return;
    memcpy(hd_sky_buf + (size_t)j * hd_sky_w, hd_sky_buf + (size_t)from * hd_sky_w, (size_t)hd_sky_w * sizeof(u32));
    hd_sky_live[j] = hd_sky_live[from];
}

bool enh_hd_has_sky(void)
{
    return hd_opt_enabled && (img_sheets_race.pixels != NULL || img_sheets_town.pixels != NULL);
}
