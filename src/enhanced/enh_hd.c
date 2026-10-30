/* HD pictures implementation: asset loading, cockpit compositing, HD sky/horizon sheets. */
#include "enhanced/enh_hd.h"
#include "enhanced/enh_internal.h"
#include "host.h"
#include "mem.h"
#include "platform/ega.h"
#include "platform/video.h"

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

/* Clean original cockpit palette indices (320x190) from the game's descriptors */
static u8 orig_cockpit[190][320];
static bool has_orig_cockpit = false;

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
}

bool enh_hd_enabled(void)
{
    ensure_loaded();
    return hd_opt_enabled && (img_cockpit.pixels != NULL || img_sheets_race.pixels != NULL);
}

void enh_hd_set_cockpit(u16 seg, u16 off)
{
    ensure_loaded();
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

void enh_hd_compose(u32 *xrgb, int res_scale)
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

    /* Side mirror: copy high-res rear-view mirror cutout from (248*S, 22*S) to (8*S, 103*S) */
    int sx0 = 248 * res_scale, sy0 = 22 * res_scale;
    int dx0 = 8 * res_scale,   dy0 = 103 * res_scale;
    int mw  = 32 * res_scale,  mh  = 19 * res_scale;
    for (int y = 0; y < mh; y++) {
        memcpy(xrgb + (size_t)(dy0 + y) * W + dx0,
               xrgb + (size_t)(sy0 + y) * W + sx0,
               (size_t)mw * sizeof(u32));
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
             * The side mirror area (8..39, 103..121) is replaced by the HD cutout above. */
            bool in_side_mirror = (x >= 8 && x < 40 && y >= 103 && y < 122);
            if (ega_c != orig_c && !in_side_mirror) continue;

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

                    out[X] = px & 0xFFFFFF;
                }
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
