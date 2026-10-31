/* Sample buffer of the enhanced road view: palette-index samples at (res scale x aa) per original pixel,
 * command recording during road assembly, multithreaded rasterization across horizontal worker bands,
 * and linear-light supersample resolve laid over the scaled EGA picture. */
#include "enhanced/enh_internal.h"
#include "enhanced/enh_hd.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/ega.h"

#define MAX_CMDS 65536
#define MAX_WORKERS 32

#define COL_ROAD    1
#define COL_GRASS   9

typedef enum {
    CMD_CLIP,
    CMD_SKY,
    CMD_FILL_ROWS,
    CMD_FILL_RECT,
    CMD_FILL_POLY,
    CMD_DRAW_IMAGE,
} CmdType;

typedef struct {
    CmdType type;
    union {
        struct {
            int cx0, cy0, cx1, cy1;
        } clip;
        struct {
            bool town;
            double hy;
            double heading;
        } sky;
        struct {
            int j0, j1;
            u8 color;
        } fill_rows;
        struct {
            int i0, i1, j0, j1;
            u8 color;
        } fill_rect;
        struct {
            int n;
            u8 color;
            int j0, j1;
            double sx[8];
            double sy[8];
        } fill_poly;
        struct {
            EnhImage img;
            bool flip;
            double sl, sr, st, sb;
            double du, dv;
            int j0, j1, i0, i1;
        } draw_image;
    };
} RenderCmd;

EnhBuf enh_buf;

static RenderCmd cmd_list[MAX_CMDS];
static int cmd_count = 0;

static int num_workers = 1;
static SDL_Thread *worker_threads[MAX_WORKERS];
static SDL_Semaphore *sem_start[MAX_WORKERS];
static SDL_Semaphore *sem_done[MAX_WORKERS];
static bool workers_running = false;

typedef struct {
    u32 *xrgb;
    const u32 *pal;
    int npal;
    u16 page;
    int y_end;
    EnhRect dlg[4];                /* the dialogs on the UI stack: the original's picture stays there */
    int ndlg;
    const u8 *window, *cockpit;    /* enh_hd_window / enh_hd_orig_cockpit, NULL without the HD cockpit */
    float lr[256], lg[256], lb[256];
} FrameContext;

static FrameContext frame_ctx;

static u8 src_rows[0xB8][320];     /* page 1 rows 66h..B7h: the sky and horizon sheets */

/* sRGB <-> linear light tables */
static float to_lin[256];
static u8 to_srgb[4096];

static void tables(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < 256; i++) {
        double v = i / 255.0;
        to_lin[i] = (float)(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
    }
    for (int i = 0; i < 4096; i++) {
        double v = i / 4095.0;
        double s = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055;
        to_srgb[i] = (u8)SDL_clamp((int)(s * 255 + 0.5), 0, 255);
    }
}

static void enh_raster_read_sheets(void)
{
    for (int y = 0x66; y < 0xB8; y++) {
        u16 o = (u16)(0x2000 + y * EGA_BYTES_PER_LINE);
        for (int x = 0; x < 320; x++) {
            u16 b = (u16)(o + (x >> 3));
            int bit = 7 - (x & 7), c = 0;
            for (int k = 0; k < 4; k++) c |= (ega_plane(k)[b] >> bit & 1) << k;
            src_rows[y][x] = (u8)c;
        }
    }
}

void enh_buf_setup(void)
{
    int sa = enh_opt.res_scale * enh_opt.aa;
    int w = 320 * sa, h = (ENH_VIEW_Y1 - ENH_VIEW_Y0) * sa;
    if (enh_buf.px && enh_buf.sa == sa) return;
    SDL_free(enh_buf.px);
    enh_buf.px = SDL_malloc((size_t)w * h);
    enh_buf.w = w;
    enh_buf.h = h;
    enh_buf.sa = sa;
    enh_buf.cx0 = 0; enh_buf.cy0 = 0;
    enh_buf.cx1 = w; enh_buf.cy1 = h;
}

void enh_raster_begin(void)
{
    enh_buf_setup();
    enh_hd_sky_begin(enh_buf.w, enh_buf.h, enh_buf.sa);
    enh_raster_read_sheets();
    cmd_count = 0;
    enh_clip_view();
}

void enh_clip(double x0, double y0, double x1, double y1)
{
    EnhBuf *b = &enh_buf;
    int cx0 = SDL_clamp((int)ceil(x0 * b->sa - 0.5), 0, b->w);
    int cx1 = SDL_clamp((int)ceil(x1 * b->sa - 0.5), 0, b->w);
    int cy0 = SDL_clamp((int)ceil((y0 - ENH_VIEW_Y0) * b->sa - 0.5), 0, b->h);
    int cy1 = SDL_clamp((int)ceil((y1 - ENH_VIEW_Y0) * b->sa - 0.5), 0, b->h);
    b->cx0 = cx0; b->cy0 = cy0; b->cx1 = cx1; b->cy1 = cy1;

    if (cmd_count < MAX_CMDS) {
        RenderCmd *cmd = &cmd_list[cmd_count++];
        cmd->type = CMD_CLIP;
        cmd->clip.cx0 = cx0;
        cmd->clip.cy0 = cy0;
        cmd->clip.cx1 = cx1;
        cmd->clip.cy1 = cy1;
    }
}

void enh_clip_view(void)
{
    EnhBuf *b = &enh_buf;
    b->cx0 = 0; b->cy0 = 0; b->cx1 = b->w; b->cy1 = b->h;

    if (cmd_count < MAX_CMDS) {
        RenderCmd *cmd = &cmd_list[cmd_count++];
        cmd->type = CMD_CLIP;
        cmd->clip.cx0 = 0;
        cmd->clip.cy0 = 0;
        cmd->clip.cx1 = b->w;
        cmd->clip.cy1 = b->h;
    }
}

void enh_draw_sky(bool town, double hy, double heading)
{
    if (cmd_count < MAX_CMDS) {
        RenderCmd *cmd = &cmd_list[cmd_count++];
        cmd->type = CMD_SKY;
        cmd->sky.town = town;
        cmd->sky.hy = hy;
        cmd->sky.heading = heading;
    }
}

void enh_fill_rows(double y0, double y1, u8 c)
{
    if (cmd_count >= MAX_CMDS) return;
    EnhBuf *b = &enh_buf;
    RenderCmd *cmd = &cmd_list[cmd_count++];
    cmd->type = CMD_FILL_ROWS;
    cmd->fill_rows.j0 = (int)ceil((y0 - ENH_VIEW_Y0) * b->sa - 0.5);
    cmd->fill_rows.j1 = (int)ceil((y1 - ENH_VIEW_Y0) * b->sa - 0.5);
    cmd->fill_rows.color = c;
}

void enh_fill_rect(double x0, double y0, double x1, double y1, u8 c)
{
    if (x1 <= x0 || y1 <= y0 || cmd_count >= MAX_CMDS) return;
    EnhBuf *b = &enh_buf;
    int i0 = (int)ceil(x0 * b->sa - 0.5);
    int i1 = (int)ceil(x1 * b->sa - 0.5);
    int j0 = (int)ceil((y0 - ENH_VIEW_Y0) * b->sa - 0.5);
    int j1 = (int)ceil((y1 - ENH_VIEW_Y0) * b->sa - 0.5);
    if (i0 >= i1 || j0 >= j1) return;

    RenderCmd *cmd = &cmd_list[cmd_count++];
    cmd->type = CMD_FILL_RECT;
    cmd->fill_rect.i0 = i0;
    cmd->fill_rect.i1 = i1;
    cmd->fill_rect.j0 = j0;
    cmd->fill_rect.j1 = j1;
    cmd->fill_rect.color = c;
}

void enh_fill_poly(const double *xs, const double *ys, int n, u8 c)
{
    if (n <= 2 || n > 8 || cmd_count >= MAX_CMDS) return;
    EnhBuf *b = &enh_buf;
    RenderCmd *cmd = &cmd_list[cmd_count++];
    cmd->type = CMD_FILL_POLY;
    cmd->fill_poly.n = n;
    cmd->fill_poly.color = c;

    double ymin = 1e30, ymax = -1e30;
    for (int i = 0; i < n; i++) {
        double sx = xs[i] * b->sa;
        double sy = (ys[i] - ENH_VIEW_Y0) * b->sa;
        cmd->fill_poly.sx[i] = sx;
        cmd->fill_poly.sy[i] = sy;
        if (sy < ymin) ymin = sy;
        if (sy > ymax) ymax = sy;
    }
    cmd->fill_poly.j0 = (int)ceil(ymin - 0.5);
    cmd->fill_poly.j1 = (int)ceil(ymax - 0.5);
}

void enh_draw_image(const EnhImage *img, double x0, double y0, double x1, double y1)
{
    if (!img || !img->px || y1 <= y0 || x1 == x0 || cmd_count >= MAX_CMDS) return;
    EnhBuf *b = &enh_buf;
    bool flip = x1 < x0;
    double l = flip ? x1 : x0, r = flip ? x0 : x1;
    double sl = l * b->sa, sr = r * b->sa, st = (y0 - ENH_VIEW_Y0) * b->sa, sb = (y1 - ENH_VIEW_Y0) * b->sa;

    RenderCmd *cmd = &cmd_list[cmd_count++];
    cmd->type = CMD_DRAW_IMAGE;
    cmd->draw_image.img = *img;
    cmd->draw_image.flip = flip;
    cmd->draw_image.sl = sl;
    cmd->draw_image.sr = sr;
    cmd->draw_image.st = st;
    cmd->draw_image.sb = sb;
    cmd->draw_image.du = img->w / (sr - sl);
    cmd->draw_image.dv = img->h / (sb - st);
    cmd->draw_image.i0 = (int)ceil(sl - 0.5);
    cmd->draw_image.i1 = (int)ceil(sr - 0.5);
    cmd->draw_image.j0 = (int)ceil(st - 0.5);
    cmd->draw_image.j1 = (int)ceil(sb - 0.5);
}

/* ---------------------------------------------------------------- execution in bands */

static inline void sheet_row(int j, int sy, double shift)
{
    EnhBuf *b = &enh_buf;
    if (sy < 0x66) sy = 0x66;
    if (sy > 0xB7) sy = 0xB7;
    u8 *out = b->px + (size_t)j * b->w;
    double inv_sa = 1.0 / b->sa;
    for (int i = 0; i < b->w; i++) {
        double x = (i + 0.5) * inv_sa - shift;
        int col = (int)floor(x) % 320;
        if (col < 0) col += 320;
        out[i] = src_rows[sy][col];
    }
}

/* Sample row j from sheet row sy scrolled by shift, the HD sky row with it. The res scale x aa sample rows
 * of one original row are the same: a row equal to the one above (in this band) is copied, not sampled. */
static inline void sky_sample_row(int j, int j_start, int sy, double shift, bool town, int *last_sy,
                                  double *last_shift)
{
    EnhBuf *b = &enh_buf;
    if (j > j_start && sy == *last_sy && shift == *last_shift) {
        memcpy(b->px + (size_t)j * b->w, b->px + (size_t)(j - 1) * b->w, (size_t)b->w);
        enh_hd_sky_copy_row(j, j - 1);
        return;
    }
    sheet_row(j, sy, shift);
    enh_hd_sky_row(j, sy, shift, town);
    *last_sy = sy;
    *last_shift = shift;
}

static inline void render_sky_band(const RenderCmd *cmd, int j_start, int j_end)
{
    EnhBuf *b = &enh_buf;
    int last_sy = -1;
    double last_shift = 0;
    if (cmd->sky.town) {
        for (int j = j_start; j < j_end; j++) {
            double y = ENH_VIEW_Y0 + (j + 0.5) / b->sa;
            if (y < 0x2F)
                sky_sample_row(j, j_start, 0x66 + (int)floor(y - 0x12), 0, true, &last_sy, &last_shift);
            else if (y < 0x3F)
                sky_sample_row(j, j_start, 0xA3 + (int)floor(y - 0x2F), 0, true, &last_sy, &last_shift);
            else {
                memset(b->px + (size_t)j * b->w, COL_ROAD, (size_t)b->w);
                enh_hd_sky_clear_span(j, 0, b->w);
                last_sy = -1;
            }
        }
    } else {
        double hy = cmd->sky.hy;
        double heading = cmd->sky.heading;
        /* the original moves the sky x2 and the horizon x4 the heading; turning, both are at infinity and move
         * alike, so with the reduced rotation the sky goes with the horizon */
        double sky_k = enh_opt.sky_scroll < 100 ? 4.0 : 2.0;
        for (int j = j_start; j < j_end; j++) {
            double y = ENH_VIEW_Y0 + (j + 0.5) / b->sa;
            if (y < hy - 12)
                sky_sample_row(j, j_start, (int)floor(y + 0xA6 - hy), sky_k * heading, false,
                               &last_sy, &last_shift);
            else if (y < hy + 8)
                sky_sample_row(j, j_start, 0xA3 + (int)floor(y - (hy - 12)), 4.0 * heading, false,   /* x4 */
                               &last_sy, &last_shift);
            else {
                memset(b->px + (size_t)j * b->w, COL_GRASS, (size_t)b->w);
                enh_hd_sky_clear_span(j, 0, b->w);
                last_sy = -1;
            }
        }
    }
}

static inline void render_fill_rows_band(const RenderCmd *cmd, int j_start, int j_end,
                                         int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    int j0 = SDL_max(SDL_max(cmd->fill_rows.j0, clip_y0), j_start);
    int j1 = SDL_min(SDL_min(cmd->fill_rows.j1, clip_y1), j_end);
    if (j0 >= j1 || clip_x1 <= clip_x0) return;

    EnhBuf *b = &enh_buf;
    u8 c = cmd->fill_rows.color;
    size_t span_len = (size_t)(clip_x1 - clip_x0);

    for (int j = j0; j < j1; j++) {
        memset(b->px + (size_t)j * b->w + clip_x0, c, span_len);
        enh_hd_sky_clear_span(j, clip_x0, clip_x1);
    }
}

static inline void render_fill_rect_band(const RenderCmd *cmd, int j_start, int j_end,
                                         int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    int j0 = SDL_max(SDL_max(cmd->fill_rect.j0, clip_y0), j_start);
    int j1 = SDL_min(SDL_min(cmd->fill_rect.j1, clip_y1), j_end);
    if (j0 >= j1) return;

    int i0 = SDL_max(cmd->fill_rect.i0, clip_x0);
    int i1 = SDL_min(cmd->fill_rect.i1, clip_x1);
    if (i0 >= i1) return;

    EnhBuf *b = &enh_buf;
    u8 c = cmd->fill_rect.color;
    size_t span_len = (size_t)(i1 - i0);

    for (int j = j0; j < j1; j++) {
        memset(b->px + (size_t)j * b->w + i0, c, span_len);
        enh_hd_sky_clear_span(j, i0, i1);
    }
}

static inline void render_fill_poly_band(const RenderCmd *cmd, int j_start, int j_end,
                                         int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    int j0 = SDL_max(SDL_max(cmd->fill_poly.j0, clip_y0), j_start);
    int j1 = SDL_min(SDL_min(cmd->fill_poly.j1, clip_y1), j_end);
    if (j0 >= j1) return;

    EnhBuf *b = &enh_buf;
    int n = cmd->fill_poly.n;
    u8 c = cmd->fill_poly.color;
    const double *sx = cmd->fill_poly.sx;
    const double *sy = cmd->fill_poly.sy;

    for (int j = j0; j < j1; j++) {
        double yc = j + 0.5, xl = 1e30, xr = -1e30;
        for (int i = 0; i < n; i++) {
            int k = (i + 1) % n;
            double ya = sy[i], yb = sy[k];
            if ((yc < ya) == (yc < yb)) continue;
            double x = sx[i] + (sx[k] - sx[i]) * (yc - ya) / (yb - ya);
            if (x < xl) xl = x;
            if (x > xr) xr = x;
        }
        if (xl > xr) continue;
        int i0 = (int)ceil(xl - 0.5), i1 = (int)ceil(xr - 0.5);
        if (i0 < clip_x0) i0 = clip_x0;
        if (i1 > clip_x1) i1 = clip_x1;
        if (i1 > i0) {
            memset(b->px + (size_t)j * b->w + i0, c, (size_t)(i1 - i0));
            enh_hd_sky_clear_span(j, i0, i1);
        }
    }
}

static inline void render_draw_image_band(const RenderCmd *cmd, int j_start, int j_end,
                                          int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    int j0 = SDL_max(SDL_max(cmd->draw_image.j0, clip_y0), j_start);
    int j1 = SDL_min(SDL_min(cmd->draw_image.j1, clip_y1), j_end);
    if (j0 >= j1) return;

    int i0 = SDL_max(cmd->draw_image.i0, clip_x0);
    int i1 = SDL_min(cmd->draw_image.i1, clip_x1);
    if (i0 >= i1) return;

    EnhBuf *b = &enh_buf;
    const EnhImage *img = &cmd->draw_image.img;
    bool flip = cmd->draw_image.flip;
    double sl = cmd->draw_image.sl;
    double st = cmd->draw_image.st;
    double du = cmd->draw_image.du;
    double dv = cmd->draw_image.dv;

    for (int j = j0; j < j1; j++) {
        int v = (int)((j + 0.5 - st) * dv);
        if (v < 0 || v >= img->h) continue;
        const u8 *src = img->px + (size_t)v * img->w;
        u8 *out = b->px + (size_t)j * b->w;
        for (int i = i0; i < i1; i++) {
            int u = (int)((i + 0.5 - sl) * du);
            if (u < 0 || u >= img->w) continue;
            u8 c = src[flip ? img->w - 1 - u : u];
            if (c != ENH_CLEAR) {
                out[i] = c;
                enh_hd_sky_clear_pixel(j, i);
            }
        }
    }
}

static void resolve_band(int y_from, int y_to)
{
    EnhBuf *b = &enh_buf;
    int S = enh_opt.res_scale, aa = enh_opt.aa, W = 320 * S;
    const u8 *wr = ega_written(), *fg = ega_fg();
    float inv = 4095.0f / (float)(aa * aa);
    u16 page = frame_ctx.page;
    u32 *xrgb = frame_ctx.xrgb;
    const u32 *sky_buf = enh_hd_has_sky() ? enh_hd_sky_buffer() : NULL;
    const u8 *sky_live = enh_hd_sky_live();
    const float *lr = frame_ctx.lr;
    const float *lg = frame_ctx.lg;
    const float *lb = frame_ctx.lb;

    const u8 *sample_rows[8];
    const u32 *sky_rows[8];

    for (int y = y_from; y < y_to; y++) {
        u16 row = (u16)(page + y * EGA_BYTES_PER_LINE);
        for (int oy = 0; oy < S; oy++) {
            int Y = y * S + oy;
            u32 *out = xrgb + (size_t)Y * W;
            int base_j = ((y - ENH_VIEW_Y0) * S + oy) * aa;
            const u8 *src = b->px + (size_t)base_j * b->w;

            for (int j = 0; j < aa && j < 8; j++) {
                sample_rows[j] = src + (size_t)j * b->w;
                sky_rows[j] = sky_buf && sky_live && sky_live[base_j + j] ? sky_buf + (size_t)(base_j + j) * b->w
                                                                          : NULL;
            }

            for (int x = 0; x < 320; x++) {
                u16 o = (u16)(row + (x >> 3));
                u8 bit = (u8)(0x80 >> (x & 7));
                /* the original's pixel; in the mirror box (the original's mirror is replaced) only under a
                 * dialog */
                if ((wr[o] | fg[o]) & bit) {
                    bool in_mirror = x >= 240 && y >= 20 && y <= 45;
                    bool through = false;          /* the static cockpit where the HD cockpit is clear */
                    if (frame_ctx.window && y < 190 && frame_ctx.window[y * 320 + x]) {
                        int c = (ega_plane(0)[o] & bit ? 1 : 0) | (ega_plane(1)[o] & bit ? 2 : 0) |
                                (ega_plane(2)[o] & bit ? 4 : 0) | (ega_plane(3)[o] & bit ? 8 : 0);
                        through = c == frame_ctx.cockpit[y * 320 + x];
                    }
                    if (enh_hd_in_rects(frame_ctx.dlg, frame_ctx.ndlg, x, y) || (!in_mirror && !through)) continue;
                }
                for (int ox = 0; ox < S; ox++) {
                    int X = x * S + ox;
                    int base_i = X * aa;
                    float r = 0, g = 0, bl = 0;
                    for (int j = 0; j < aa; j++) {
                        const u8 *s = sample_rows[j] + base_i;
                        const u32 *sr = sky_rows[j] ? sky_rows[j] + base_i : NULL;
                        for (int i = 0; i < aa; i++) {
                            u32 sky_c = sr ? sr[i] : 0;
                            if (sky_c != 0) {
                                r += to_lin[(sky_c >> 16) & 0xFF];
                                g += to_lin[(sky_c >> 8) & 0xFF];
                                bl += to_lin[sky_c & 0xFF];
                            } else {
                                u8 idx = s[i];
                                r += lr[idx];
                                g += lg[idx];
                                bl += lb[idx];
                            }
                        }
                    }
                    out[X] = (u32)to_srgb[(int)(r * inv)] << 16 | (u32)to_srgb[(int)(g * inv)] << 8 |
                             to_srgb[(int)(bl * inv)];
                }
            }
        }
    }
}

static void worker_exec(int worker_id)
{
    EnhBuf *b = &enh_buf;
    int total_y = ENH_VIEW_Y1 - ENH_VIEW_Y0;
    int y_from = ENH_VIEW_Y0 + (worker_id * total_y) / num_workers;
    int y_to   = ENH_VIEW_Y0 + ((worker_id + 1) * total_y) / num_workers;
    int j_start = (y_from - ENH_VIEW_Y0) * b->sa;
    int j_end   = (y_to   - ENH_VIEW_Y0) * b->sa;

    int clip_x0 = 0, clip_y0 = 0, clip_x1 = b->w, clip_y1 = b->h;

    for (int c = 0; c < cmd_count; c++) {
        const RenderCmd *cmd = &cmd_list[c];
        switch (cmd->type) {
        case CMD_CLIP:
            clip_x0 = cmd->clip.cx0;
            clip_y0 = cmd->clip.cy0;
            clip_x1 = cmd->clip.cx1;
            clip_y1 = cmd->clip.cy1;
            break;
        case CMD_SKY:
            render_sky_band(cmd, j_start, j_end);
            break;
        case CMD_FILL_ROWS:
            render_fill_rows_band(cmd, j_start, j_end, clip_x0, clip_y0, clip_x1, clip_y1);
            break;
        case CMD_FILL_RECT:
            render_fill_rect_band(cmd, j_start, j_end, clip_x0, clip_y0, clip_x1, clip_y1);
            break;
        case CMD_FILL_POLY:
            render_fill_poly_band(cmd, j_start, j_end, clip_x0, clip_y0, clip_x1, clip_y1);
            break;
        case CMD_DRAW_IMAGE:
            render_draw_image_band(cmd, j_start, j_end, clip_x0, clip_y0, clip_x1, clip_y1);
            break;
        }
    }

    int res_y_end = SDL_min(y_to, frame_ctx.y_end);
    if (res_y_end > y_from) {
        resolve_band(y_from, res_y_end);
    }
}

/* a job for enh_parallel_rows instead of the frame's commands */
static void (*job_fn)(int r0, int r1, void *ctx);
static void *job_ctx;
static int job_rows;

static void job_part(int worker_id)
{
    job_fn(worker_id * job_rows / num_workers, (worker_id + 1) * job_rows / num_workers, job_ctx);
}

static int SDLCALL worker_thread_fn(void *data)
{
    int worker_id = (int)(intptr_t)data;
    while (workers_running) {
        SDL_WaitSemaphore(sem_start[worker_id]);
        if (!workers_running) break;
        if (job_fn) job_part(worker_id);
        else worker_exec(worker_id);
        SDL_SignalSemaphore(sem_done[worker_id]);
    }
    return 0;
}

void enh_parallel_rows(int rows, void (*fn)(int r0, int r1, void *ctx), void *ctx)
{
    if (!workers_running || num_workers <= 1) {
        fn(0, rows, ctx);
        return;
    }
    job_fn = fn;
    job_ctx = ctx;
    job_rows = rows;
    for (int i = 1; i < num_workers; i++) SDL_SignalSemaphore(sem_start[i]);
    job_part(0);
    for (int i = 1; i < num_workers; i++) SDL_WaitSemaphore(sem_done[i]);
    job_fn = NULL;
}

void enh_raster_init(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;

    tables();

    int cores = SDL_GetNumLogicalCPUCores();
    if (cores < 1) cores = 1;
    num_workers = SDL_clamp(cores, 1, MAX_WORKERS);

    workers_running = true;
    for (int i = 1; i < num_workers; i++) {
        sem_start[i] = SDL_CreateSemaphore(0);
        sem_done[i] = SDL_CreateSemaphore(0);
        char name[32];
        SDL_snprintf(name, sizeof(name), "enh_worker_%d", i);
        worker_threads[i] = SDL_CreateThread(worker_thread_fn, name, (void *)(intptr_t)i);
    }
    host_set_shutdown_handler(enh_raster_shutdown);   /* the workers end before SDL_Quit */
    atexit(enh_raster_shutdown);                     /* and on any other way out (it runs once) */
}

void enh_raster_shutdown(void)
{
    if (!workers_running) return;
    workers_running = false;
    for (int i = 1; i < num_workers; i++) {
        if (sem_start[i]) SDL_SignalSemaphore(sem_start[i]);
    }
    for (int i = 1; i < num_workers; i++) {
        if (worker_threads[i]) {
            SDL_WaitThread(worker_threads[i], NULL);
            worker_threads[i] = NULL;
        }
        if (sem_start[i]) {
            SDL_DestroySemaphore(sem_start[i]);
            sem_start[i] = NULL;
        }
        if (sem_done[i]) {
            SDL_DestroySemaphore(sem_done[i]);
            sem_done[i] = NULL;
        }
    }
    num_workers = 1;
}

void enh_raster_dispatch_and_resolve(u32 *xrgb, const u32 *pal, int npal, u16 page, int y_end)
{
    tables();
    static bool warned;
    if (cmd_count >= MAX_CMDS && !warned) {
        warned = true;
        fprintf(stderr, "enhanced view: more than %d drawing commands in a frame, the rest is dropped\n", MAX_CMDS);
    }

    for (int i = 0; i < npal && i < 256; i++) {
        frame_ctx.lr[i] = to_lin[(pal[i] >> 16) & 0xFF];
        frame_ctx.lg[i] = to_lin[(pal[i] >> 8) & 0xFF];
        frame_ctx.lb[i] = to_lin[pal[i] & 0xFF];
    }
    frame_ctx.xrgb = xrgb;
    frame_ctx.pal = pal;
    frame_ctx.npal = npal;
    frame_ctx.page = page;
    frame_ctx.y_end = y_end;
    frame_ctx.ndlg = enh_hd_dialogs(frame_ctx.dlg);
    frame_ctx.window = enh_hd_window();
    frame_ctx.cockpit = enh_hd_orig_cockpit();

    for (int i = 1; i < num_workers; i++) {
        SDL_SignalSemaphore(sem_start[i]);
    }

    worker_exec(0);

    for (int i = 1; i < num_workers; i++) {
        SDL_WaitSemaphore(sem_done[i]);
    }
}

void enh_resolve(u32 *xrgb, const u32 *pal, int npal, u16 page, int y_end)
{
    enh_raster_dispatch_and_resolve(xrgb, pal, npal, page, y_end);
}
