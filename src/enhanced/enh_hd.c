/* HD pictures implementation: asset loading, cockpit compositing, HD sky/horizon sheets. */
#include "enhanced/enh_hd.h"
#include "enhanced/enh_internal.h"
#include "host.h"
#include "mem.h"
#include "platform/ega.h"
#include "platform/video.h"
#include "game/ui.h"
#include "game/retry.h"

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

static bool hd_cockpit_active = false;
static bool hd_crashed = false;
static int  hd_knob_type = 2;
static int  hd_knob_x = 0, hd_knob_y = 0;
static bool hd_knob_active = false;

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

bool enh_hd_is_crashed(void)
{
    return hd_crashed;
}

/* Clean original cockpit palette indices (320x190) from the game's descriptors */
static u8 orig_cockpit[190][320];
static bool has_orig_cockpit = false;

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

void enh_hd_set_needle(int kind, int x0, int y0, int x1, int y1)
{
    if (kind < 0 || kind > 1) return;
    hd_needles[kind].x0 = (float)x0;
    hd_needles[kind].y0 = (float)y0;
    hd_needles[kind].x1 = (float)x1;
    hd_needles[kind].y1 = (float)y1;
    hd_needles[kind].active = true;
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
static int hd_sky_w = 0, hd_sky_h = 0, hd_sky_sa = 0;

static u32 *load_image(const char *rel_path, int *w, int *h)
{
    char *path = host_game_path(rel_path, false);
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    host_free(path);
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }

    u8 *buf = SDL_malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        SDL_free(buf);
        return NULL;
    }
    fclose(f);

    int comp = 0;
    stbi_uc *data = stbi_load_from_memory(buf, (int)size, w, h, &comp, 4);
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
    if (out->pixels) {
        fprintf(stderr, "HD loaded: %s (%dx%d)\n", name, out->w, out->h);
    }
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
}

void enh_hd_set_screen_pic(s16 id, u16 seg, u16 off)
{
    enh_hd_clear_cockpit();
    (void)id; (void)seg; (void)off;
}

bool enh_hd_has_screen(void)
{
    return false;
}

void enh_hd_compose_screen(u32 *xrgb, int res_scale)
{
    (void)xrgb; (void)res_scale;
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

void enh_hd_set_clock(u16 seg, u16 off, int dx, int dy)
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

void enh_hd_set_gate(u16 seg, u16 off, int dx, int dy)
{
    enh_hd_set_clock(seg, off, dx, dy);
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

    /* Check if top rear-view mirror cutout is covered (e.g. crash dialog box or msg_box) */
    bool top_mirror_covered = hd_crashed;
    if (!top_mirror_covered) {
        for (int my = 22; my < 41 && !top_mirror_covered; my += 4) {
            u16 mrow = my < line_compare ? (u16)(start_addr + my * EGA_BYTES_PER_LINE)
                                         : (u16)((my - line_compare) * EGA_BYTES_PER_LINE);
            for (int mx = 248; mx < 280; mx += 8) {
                u16 o = (u16)(mrow + (mx >> 3));
                int bit = 7 - (mx & 7);
                int ega_c = ((p0[o] >> bit & 1)     ) |
                            ((p1[o] >> bit & 1) << 1) |
                            ((p2[o] >> bit & 1) << 2) |
                            ((p3[o] >> bit & 1) << 3);
                if (ega_c == 2 || ega_c == 10) {
                    top_mirror_covered = true;
                    break;
                }
            }
        }
    }

    /* Side mirror: copy high-res rear-view mirror cutout from (248*S, 22*S) to (8*S, 103*S) */
    int sx0 = 248 * res_scale, sy0 = 22 * res_scale;
    int dx0 = 8 * res_scale,   dy0 = 103 * res_scale;
    int mw  = 32 * res_scale,  mh  = 19 * res_scale;
    if (!top_mirror_covered) {
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

    for (int y = 0; y < 190; y++) {
        u16 row = y < line_compare ? (u16)(start_addr + y * EGA_BYTES_PER_LINE)
                                   : (u16)((y - line_compare) * EGA_BYTES_PER_LINE);
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
            bool in_dial = (dtx * dtx + dty * dty <= 27 * 27) || (dsx * dsx + dsy * dsy <= 27 * 27);
            bool in_wheel_spokes = (x >= 80 && x < 264 && y >= 163 && y < 190);
            bool in_shifter = (x >= 264 && x < 312 && y >= 123 && y < 166);
            bool in_d1 = top_mirror_covered && (x >= 30 && x <= 298 && y >= 6 && y <= 66);
            bool in_d2 = (retry_is_dialog_active() || (x >= 55 && x <= 272 && y >= 66 && y <= 128 && (ega_c == 15 || ega_c == 0 || ega_c == 7 || ega_c == 8)))
                         && (x >= 55 && x <= 272 && y >= 66 && y <= 128);
            if (in_d1 || in_d2) continue;
            if (ega_c != orig_c && !in_side_mirror && !in_dial && !in_wheel_spokes && !in_shifter) continue;

            for (int oy = 0; oy < res_scale; oy++) {
                int Y = y * res_scale + oy;
                int hy = (Y * ch) / (190 * res_scale);
                if (hy >= ch) hy = ch - 1;
                const u32 *hd_row = img_cockpit.pixels + (size_t)hy * cw;
                u32 *out = xrgb + (size_t)Y * W;

                for (int ox = 0; ox < res_scale; ox++) {
                    int X = x * res_scale + ox;
                    int hx = (X * cw) / W;
                    if (hx >= cw) hx = cw - 1;

                    u32 px = hd_row[hx];
                    u8 a = (u8)(px >> 24);
                    if (a == 0) continue; /* Transparent: windshield/mirror road view */

                    /* Dynamic hood/fender recoloring matching player car paint */
                    int hood_min_x = 72 + ((114 - y) * 26) / 16;
                    if (y >= 96 && y < 114 && x >= hood_min_x) {
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
    if (hd_crashed && img_crash.pixels != NULL) {
        /* Collect all active UI dialog bounding boxes to ensure NO cracks render over any dialog */
        typedef struct { int x0, y0, x1, y1; } UIRect;
        UIRect ui_boxes[8];
        int num_boxes = 0;

        s16 ui_depth = DSS(0x8B8E);
        if (ui_depth > 0 && ui_depth <= 4) {
            for (int d = 0; d < ui_depth && num_boxes < 8; d++) {
                s16 scr = DSS((u16)(0x8B86 + 2 * d));
                if (scr >= 0 && scr < 0x30) {
                    s16 bx, by, bw, bh;
                    ui_bounds(scr, &bx, &by, &bw, &bh);
                    if (bw > 0 && bh > 0) {
                        ui_boxes[num_boxes].x0 = bx - 2 >= 0 ? bx - 2 : 0;
                        ui_boxes[num_boxes].y0 = by - 2 >= 0 ? by - 2 : 0;
                        ui_boxes[num_boxes].x1 = bx + bw + 2 <= 320 ? bx + bw + 2 : 320;
                        ui_boxes[num_boxes].y1 = by + bh + 2 <= 200 ? by + bh + 2 : 200;
                        num_boxes++;
                    }
                }
            }
        }

        /* Safety bounds for crash dialog (menu 0x19/0x1A: x ~ 30..296, y ~ 6..66)
         * and retry dialog (x ~ 56..267, y ~ 67..125) */
        if (num_boxes < 8 && top_mirror_covered) {
            ui_boxes[num_boxes++] = (UIRect){ 30, 6, 296, 66 };
        }
        /* Check if retry dialog is active (center white popup at x ~ 56..267, y ~ 67..125) */
        if (num_boxes < 8) {
            u16 r_row = 72 < line_compare ? (u16)(start_addr + 72 * EGA_BYTES_PER_LINE)
                                           : (u16)((72 - line_compare) * EGA_BYTES_PER_LINE);
            u16 r_o = (u16)(r_row + (100 >> 3));
            int r_bit = 7 - (100 & 7);
            int r_c = ((p0[r_o] >> r_bit & 1)     ) |
                      ((p1[r_o] >> r_bit & 1) << 1) |
                      ((p2[r_o] >> r_bit & 1) << 2) |
                      ((p3[r_o] >> r_bit & 1) << 3);
            if (r_c == 15) { /* White background of retry dialog */
                ui_boxes[num_boxes++] = (UIRect){ 56, 67, 267, 125 };
            }
        }

        int cw2 = img_crash.w, ch2 = img_crash.h;
        for (int Y = 0; Y < 190 * res_scale; Y++) {
            int y = Y / res_scale;
            int cy = (Y * ch2) / (190 * res_scale);
            if (cy >= ch2) cy = ch2 - 1;
            const u32 *c_row = img_crash.pixels + (size_t)cy * cw2;
            u32 *out_row = xrgb + (size_t)Y * W;

            for (int X = 0; X < W; X++) {
                int x = X / res_scale;

                /* Never draw cracks over Dialog 1 (crash results) or Dialog 2 (retry offer) */
                bool in_d1 = top_mirror_covered && (x >= 30 && x <= 298 && y >= 6 && y <= 66);
                bool in_d2 = (retry_is_dialog_active() || (x >= 55 && x <= 272 && y >= 66 && y <= 128))
                             && (x >= 55 && x <= 272 && y >= 66 && y <= 128);
                if (in_d1 || in_d2) continue;

                /* Never draw cracks over any other active dialog box */
                bool in_dialog = false;
                for (int b = 0; b < num_boxes; b++) {
                    if (x >= ui_boxes[b].x0 && x <= ui_boxes[b].x1 &&
                        y >= ui_boxes[b].y0 && y <= ui_boxes[b].y1) {
                        in_dialog = true;
                        break;
                    }
                }
                if (in_dialog) continue;

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
}

void enh_hd_sky_begin(int w, int h, int sa)
{
    ensure_loaded();
    if (hd_sky_w != w || hd_sky_h != h) {
        SDL_free(hd_sky_buf);
        hd_sky_buf = SDL_malloc((size_t)w * (size_t)h * sizeof(u32));
        hd_sky_w = w;
        hd_sky_h = h;
    }
    hd_sky_sa = sa;
    if (hd_sky_buf) memset(hd_sky_buf, 0, (size_t)w * (size_t)h * sizeof(u32));
}

bool enh_hd_sky_row(int j, int sy, double shift, bool town)
{
    if (!hd_opt_enabled || !hd_sky_buf || j < 0 || j >= hd_sky_h) return false;

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
    if (!hd_sky_buf || j < 0 || j >= hd_sky_h || i1 <= i0) return;
    if (i0 < 0) i0 = 0;
    if (i1 > hd_sky_w) i1 = hd_sky_w;
    if (i1 > i0) {
        memset(hd_sky_buf + (size_t)j * hd_sky_w + i0, 0, (size_t)(i1 - i0) * sizeof(u32));
    }
}

void enh_hd_sky_clear_pixel(int j, int i)
{
    if (!hd_sky_buf || j < 0 || j >= hd_sky_h || i < 0 || i >= hd_sky_w) return;
    hd_sky_buf[j * hd_sky_w + i] = 0;
}

u32 enh_hd_sky_pixel(int j, int i)
{
    if (!hd_sky_buf || j < 0 || j >= hd_sky_h || i < 0 || i >= hd_sky_w) return 0;
    return hd_sky_buf[j * hd_sky_w + i];
}

bool enh_hd_has_sky(void)
{
    return hd_opt_enabled && (img_sheets_race.pixels != NULL || img_sheets_town.pixels != NULL);
}
