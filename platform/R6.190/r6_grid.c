/*
 * R6.190 M9.1: grids and frame masks as an LV overlay, Overlay > "Grids & masks"
 * (docs/m9_plan.md 1.3).
 *
 * What: rule-of-thirds / golden-ratio / diagonal grids, a centre mark, aspect
 * frames (1:1, 4:5 landscape (= a 5:4 frame) and portrait, 4:3, 16:9, 1.85:1,
 * 2.39:1) with optional masks (50 % / 75 % semi-transparent, solid), 90 % / 80 %
 * safe areas; colour and line width selectable. Saved in ML's config (user
 * decision 2026-10-04), default OFF.
 *
 * How: the geometry is computed here. The upstream cropmarks stay off (card
 * files, ~346 KB heap for one 720x480 BMP, Canon redraws). The pixels go into
 * ML's indexed buffer B and are marked in the existing mirror M with 0x80
 * (M = colour | 0x80); zebras, false color, peaking and clrscr_mirror already
 * leave such pixels alone (src/zebra.c, src/falsecolor.c). Canon's layer 0
 * gets them only through the ownership-aware conversion in the write-under
 * mode (r6_layer.c): Canon's AF frame, grid, level and icons stay on top.
 * Nothing of Canon's is called or written, no property, no MMIO, no file.
 * The only new memory is this file's state (a few hundred bytes).
 *
 * Pixel rules (colour c of the primitive, f = c | 0x80):
 *  - claim (draw / repair):
 *    M == f (own): B = c if B == 0 (clrscr, a box erase, the ML menu), else
 *      leave it (text or the ML menu on top of the line);
 *    M with another 0x80 mark (spotmeter protection, another grid colour):
 *      never taken over;
 *    M without mark and B == 0 or B == M (empty, or a zebra / false color /
 *      peaking pixel): B = c, M = f; the mirror-backed overlay pixels (B == M,
 *      no mark) of the same 4-pixel word in the rows above and below are
 *      erased once (zebras and false color work on row pairs and skip a pair
 *      with a marked pixel, peaking dots are 2x2: next to a new line their
 *      pixels would otherwise stay stale);
 *    anything else (text, the histogram / waveform boxes, NotifyBox): left
 *      alone and counted.
 *  - erase: only where M still holds the mark of the drawn colour: M = 0, and
 *    B = 0 where B still holds that colour.
 *  The drawn shape is erased from the snapshot of its settings, never from the
 *  current ones.
 *
 * Geometry (ML coordinates): image x 0..719, rows 0..479 on the LCD, rows
 * -30..449 in the EVF (R6_GD_EVF_IMG_Y, M7.7, the gate's screen r6_gd_evf).
 * Frames as in m9/grid/grid_geom_out.txt: taller than 3:2 -> full height,
 * width (480 N + D / 2) / D, centred; wider -> full width, height
 * (720 D + N / 2) / N, centred. Frame lines lie just outside the frame (in the
 * bar), the masks beyond them. Grid, centre mark and safe areas are relative
 * to the frame (= the whole image without one). Lines may run through Canon's
 * top / bottom bars on the LCD (user decision 2026-10-04, write-under keeps
 * Canon on top). Masks: EVF all formats; LCD only 4:3 and only in the band
 * R6_GD_LCD_Y0..Y1 - the wide formats, 1:1 and 4:5 show frame lines only on
 * the LCD (Canon's top / bottom bars cover their mask areas, the masks would
 * look skewed). The boxes of the other overlays are left out (r6_grid.h).
 *
 * Steps (src/zebra.c overlay task, every R6_GD_GRID_MS, BMP_LOCK held, ML menu
 * closed, overlay gate passed): a new signature (FNV-1a of the snapshot:
 * settings, screen, exclusions; r6_gd_bv_hash_mem) erases the old shape and
 * draws the new one; an unchanged one only repairs missing pixels. The caller
 * requests a refresh only if ML's buffer changed. r6_gd_erase erases the shape
 * with the other overlays (Canon MENU / Q / playback, display off, LCD <-> EVF,
 * magnify via zebra_should_run, Global Draw OFF).
 *
 * M9.3: R6 Motion's display (area frame, value bar, threshold tick, state
 * square; r6_motion.c r6_md_shape) goes through the same walk with its own
 * snapshot and signature, after the grid: its colours (orange, green 1,
 * magenta, light blue) are no grid colour, so the two never erase each
 * other's pixels and the grid wins where they overlap. drawn and
 * R6GD_DRAWN_GRID cover both; r6_grid_erase erases both.
 *
 * M11.0: R6 ETTR's LV line (Show in LV, r6_ettr.c) is drawn by the same step,
 * last: one text line in a black box above Canon's bottom bar, LCD only, its
 * mirror cleared; drawn / R6GD_DRAWN_GRID and r6_grid_erase cover it too.
 *
 * M14.1 (spec m14/m141 5): the temperature box (r6_temp.c, "A 37 degC", right
 * under the histogram place, black or, warning, yellow) by the same rules
 * before the ETTR line; the astro clock's line (r6_aclock.c) in the ETTR
 * line's place when R6 ETTR has no text (ETTR first), the owner flag
 * gr_et_own counts each erase for the one who drew. drawn and r6_grid_erase
 * cover both.
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "vram.h"
#include "propvalues.h"         /* os: the image rows src/vram.c uses */
#include "r6_gdraw.h"
#include "r6_grid.h"
#ifdef CONFIG_R6_MOTION
#include "r6_motion.h"
#endif
#ifdef CONFIG_R6_ETTR
#include "r6_ettr.h"
#endif
#ifdef CONFIG_R6_TEMP
#include "r6_temp.h"
#endif
#ifdef CONFIG_R6_ACLOCK
#include "r6_aclock.h"
#endif

#ifdef CONFIG_R6_GRID

#ifndef CONFIG_R6_GLOBAL_DRAW
#error "r6_grid.c needs CONFIG_R6_GLOBAL_DRAW (overlay gate, mirror, write-under conversion)"
#endif
#ifdef FEATURE_CROPMARKS
#error "R6 M9.1: the upstream cropmarks stay off (card files, heap); the grid is computed in r6_grid.c"
#endif

extern uint8_t * get_bvram_mirror();     /* src/zebra.c */

/* ---- settings (ML config, user decision 2026-10-04; default OFF) ---- */
static CONFIG_INT("r6.grid.on",     gr_on, 0);
static CONFIG_INT("r6.grid.type",   gr_type, 1);      /* 0 OFF, 1 thirds, 2 golden ratio, 3 diagonals, 4 thirds + diagonals */
static CONFIG_INT("r6.grid.center", gr_center, 0);    /* 0 OFF, 1 cross, 2 small cross, 3 dot */
static CONFIG_INT("r6.grid.aspect", gr_aspect, 0);    /* GR_AR_* */
static CONFIG_INT("r6.grid.style",  gr_style, 0);     /* 0 frame, 1 frame + mask 50 %, 2 frame + mask 75 %, 3 solid mask */
static CONFIG_INT("r6.grid.safe",   gr_safe, 0);      /* bit 0: 90 %, bit 1: 80 % */
static CONFIG_INT("r6.grid.color",  gr_color, 0);     /* index into gr_colors */
static CONFIG_INT("r6.grid.line",   gr_line, 0);      /* 0 1 px, 1 2 px, 2 1 px + shadow */

enum { GR_AR_NONE, GR_AR_1_1, GR_AR_5_4, GR_AR_4_5, GR_AR_4_3, GR_AR_16_9, GR_AR_185, GR_AR_239, GR_NAR };
static const uint16_t gr_ar[GR_NAR][2] = { { 0, 0 }, { 1, 1 }, { 5, 4 }, { 4, 5 }, { 4, 3 }, { 16, 9 }, { 185, 100 }, { 239, 100 } };

/* line colours: grey (0xffaaaaaa, close to Canon's grid grey), white, black (0xff010101), yellow, cyan, green,
 * red, white 60 % (R6_GRID_WHITE60). Masks: 50 % / 75 % black (R6_GRID_MASK50 / 75) or solid black.
 * All < 0x80 (mirror mark) and not 0 / 3 (transparent). */
static const uint8_t gr_colors[] = { 66, COLOR_WHITE, COLOR_BLACK, COLOR_YELLOW, COLOR_CYAN, COLOR_GREEN2, COLOR_RED,
                                     R6_GRID_WHITE60 };
#define GR_NCOL ((int) COUNT(gr_colors))

#define GR_W 720
#define GR_H 480

/* the snapshot of a shape: everything its pixels depend on (hashed, no padding) */
struct gr_cfg
{
    uint8_t type, center, aspect, style, safe, line, col, col2;
    uint8_t fill;            /* mask colour, 0 = no mask */
    uint8_t evf;             /* 1 = EVF rules (masks everywhere), 0 = LCD */
    int16_t y0;              /* image top row (ML) */
    uint16_t nex;
    struct r6_grid_rect ex[R6_GRID_NEXCL];
};

static struct gr_cfg gr_snap;            /* the drawn shape */
static uint32_t gr_snap_sig = 0;
static int gr_snap_valid = 0;
static struct r6_grid_stats st;

const struct r6_grid_stats * r6_grid_stats(void) { return &st; }

static int gr_wanted(void)
{
    return gr_on && (gr_type || gr_center || gr_aspect || gr_safe);
}
int r6_grid_wanted_grid(void) { return gr_wanted(); }

int r6_grid_wanted(void)
{
#ifdef CONFIG_R6_MOTION
    if (r6_md_disp_wanted()) return 1;
#endif
#ifdef CONFIG_R6_ETTR
    if (r6_ettr_lv_wanted()) return 1;      /* M11.0: R6 ETTR's LV line */
#endif
#ifdef CONFIG_R6_TEMP
    if (r6_temp_lv_wanted()) return 1;      /* M14.1: the temperature box */
#endif
#ifdef CONFIG_R6_ACLOCK
    if (r6_ac_lv_wanted()) return 1;        /* M14.1: the astro clock's line */
#endif
    return gr_wanted();
}

/* the wanted shape from the settings and the gate's screen; 0 = nothing to draw */
static int gr_build(struct gr_cfg * c, const struct r6_grid_rect * ex, int n)
{
    memset(c, 0, sizeof(*c));
    if (!gr_wanted()) return 0;
    c->type = COERCE(gr_type, 0, 4);
    c->center = COERCE(gr_center, 0, 3);
    c->aspect = COERCE(gr_aspect, 0, GR_NAR - 1);
    c->style = COERCE(gr_style, 0, 3);
    c->safe = COERCE(gr_safe, 0, 3);
    c->line = COERCE(gr_line, 0, 2);
    c->col = gr_colors[COERCE(gr_color, 0, GR_NCOL - 1)];
    c->col2 = c->col == COLOR_BLACK ? COLOR_WHITE : COLOR_BLACK;      /* shadow */
    c->evf = r6_gd_evf ? 1 : 0;
    c->y0 = c->evf ? R6_GD_EVF_IMG_Y : 0;
    if (c->aspect && c->style)
        c->fill = c->style == 1 ? R6_GRID_MASK50 : c->style == 2 ? R6_GRID_MASK75 : COLOR_BLACK;
    if (!c->evf && c->aspect != GR_AR_4_3) c->fill = 0;                /* LCD: masks only for 4:3 */
    n = COERCE(n, 0, R6_GRID_NEXCL);
    for (int i = 0; i < n; i++) c->ex[i] = ex[i];
    c->nex = n;
    return 1;
}

/* the aspect frame (the whole image without one) */
static void gr_frame(const struct gr_cfg * c, int * fx, int * fy, int * fw, int * fh)
{
    *fx = 0; *fy = c->y0; *fw = GR_W; *fh = GR_H;
    if (!c->aspect) return;
    int N = gr_ar[c->aspect][0], D = gr_ar[c->aspect][1];
    if (N * GR_H < D * GR_W)            /* taller than 3:2: full height */
    {
        *fw = (GR_H * N + D / 2) / D;
        *fx = (GR_W - *fw) / 2;
    }
    else                                /* wider: full width */
    {
        *fh = (GR_W * D + N / 2) / N;
        *fy = c->y0 + (GR_H - *fh) / 2;
    }
}

/* ---- the pixel walk ---- */
enum { GR_CLAIM, GR_ERASE };
enum { GR_K_LINE, GR_K_FILL, GR_K_SHADOW };

struct gr_walk
{
    int op;
    const struct gr_cfg * c;
    uint8_t * b, * m;                    /* ML's indexed buffer and mirror at ML (0, 0) */
    uint32_t px[3];                      /* pixels visited per kind (after clipping) */
    uint32_t changed, claimed, repaired, ovl, cleanup, foreign, mark;
};

/* a newly claimed pixel (x, y): erase the mirror-backed overlay pixels (B == M != 0, no mark) of its 4-pixel
 * word in the rows y - 1 .. y + 1 (zebras / false color row pairs, 2x2 peaking dots) */
static void gr_cleanup(struct gr_walk * w, int x, int y)
{
    int xa = x & ~3;
    for (int yy = MAX(y - 1, BMP_H_MINUS); yy <= MIN(y + 1, BMP_H_PLUS - 1); yy++)
    {
        uint8_t * b = w->b + yy * BMPPITCH + xa;
        uint8_t * m = w->m + yy * BMPPITCH + xa;
        for (int i = 0; i < 4; i++)
            if (b[i] && b[i] == m[i] && !(m[i] & 0x80)) { b[i] = m[i] = 0; w->cleanup++; w->changed++; }
    }
}

static void gr_px(struct gr_walk * w, int x0, int y0, int x1, int y1, int col, int kind)
{
    const uint8_t c = col, f = col | 0x80;
    const uint32_t cw = c * 0x01010101u, fw = f * 0x01010101u;
    for (int y = y0; y < y1; y++)
    {
        uint8_t * b = w->b + y * BMPPITCH;
        uint8_t * m = w->m + y * BMPPITCH;
        int cln = -1;                                   /* word whose neighbourhood was cleaned last (once per word) */
        for (int x = x0; x < x1; x++)
        {
            if (w->op == GR_ERASE)
            {
                w->px[kind]++;
                if (m[x] != f) continue;     /* not (or no longer) ours */
                m[x] = 0;
                if (b[x] == c) { b[x] = 0; w->changed++; }
                continue;
            }
            /* claim; fast path: a whole aligned word already as drawn */
            if (!(x & 3) && x + 4 <= x1 && *(uint32_t *) (b + x) == cw && *(uint32_t *) (m + x) == fw)
            {
                w->px[kind] += 4;
                x += 3;
                continue;
            }
            w->px[kind]++;
            uint8_t B = b[x], M = m[x];
            if (M == f)
            {
                if (B == c) continue;
                if (B) { w->foreign++; continue; }      /* text / ML menu on top: leave it */
                b[x] = c; w->changed++; w->repaired++;
                continue;
            }
            if (M & 0x80) { w->mark++; continue; }      /* someone else's mark: never taken over */
            if (B && B != M) { w->foreign++; continue; }
            if (B) w->ovl++;
            b[x] = c; m[x] = f; w->changed++; w->claimed++;
            if ((x >> 2) != cln) { gr_cleanup(w, x, y); cln = x >> 2; }
        }
    }
}

/* leave out the exclusion rectangles k.. (each splits the rest into at most 4 parts) */
static void gr_ex(struct gr_walk * w, int x0, int y0, int x1, int y1, int col, int kind, int k)
{
    for (; k < w->c->nex; k++)
    {
        const struct r6_grid_rect * e = &w->c->ex[k];
        if (x1 <= e->x0 || x0 >= e->x1 || y1 <= e->y0 || y0 >= e->y1) continue;
        if (y0 < e->y0) gr_ex(w, x0, y0, x1, e->y0, col, kind, k + 1);
        if (y1 > e->y1) gr_ex(w, x0, e->y1, x1, y1, col, kind, k + 1);
        int ya = MAX(y0, e->y0), yb = MIN(y1, e->y1);
        if (x0 < e->x0) gr_ex(w, x0, ya, e->x0, yb, col, kind, k + 1);
        if (x1 > e->x1) gr_ex(w, e->x1, ya, x1, yb, col, kind, k + 1);
        return;
    }
    gr_px(w, x0, y0, x1, y1, col, kind);
}

/* one primitive: clipped to the image (fills on the LCD to the band), minus the exclusions */
static void gr_rect(struct gr_walk * w, int x0, int y0, int x1, int y1, int col, int kind)
{
    const struct gr_cfg * c = w->c;
    int ylo = c->y0, yhi = c->y0 + GR_H;
    if (kind == GR_K_FILL && !c->evf)
    {
        ylo = MAX(ylo, c->y0 + R6_GD_LCD_Y0);
        yhi = MIN(yhi, c->y0 + R6_GD_LCD_Y1);
    }
    x0 = MAX(x0, 0); x1 = MIN(x1, GR_W);
    y0 = MAX(y0, ylo); y1 = MIN(y1, yhi);
    if (x0 >= x1 || y0 >= y1) return;
    gr_ex(w, x0, y0, x1, y1, col, kind, 0);
}

/* a line rectangle in the line colour, or (shadow pass) its shadow one pixel right and down */
static void gr_seg(struct gr_walk * w, int x0, int y0, int x1, int y1, int shadow)
{
    if (shadow) gr_rect(w, x0 + 1, y0 + 1, x1 + 1, y1 + 1, w->c->col2, GR_K_SHADOW);
    else gr_rect(w, x0, y0, x1, y1, w->c->col, GR_K_LINE);
}

/* the shape: pass 0 = lines and masks, pass 1 = shadows (an earlier pixel wins: a shadow never covers a line or
 * a mask, they carry another mark) */
static void gr_shape(struct gr_walk * w, int shadow)
{
    const struct gr_cfg * c = w->c;
    const int lw = c->line == 1 ? 2 : 1;
    const int Y0 = c->y0, Y1 = c->y0 + GR_H;
    int fx, fy, fw, fh;
    gr_frame(c, &fx, &fy, &fw, &fh);

    /* aspect frame: lines just outside the frame, masks beyond them */
    if (c->aspect)
    {
        if (fw < GR_W)
        {
            gr_seg(w, fx - lw, Y0, fx, Y1, shadow);
            gr_seg(w, fx + fw, Y0, fx + fw + lw, Y1, shadow);
            if (!shadow && c->fill)
            {
                gr_rect(w, 0, Y0, fx - lw, Y1, c->fill, GR_K_FILL);
                gr_rect(w, fx + fw + lw, Y0, GR_W, Y1, c->fill, GR_K_FILL);
            }
        }
        else
        {
            gr_seg(w, 0, fy - lw, GR_W, fy, shadow);
            gr_seg(w, 0, fy + fh, GR_W, fy + fh + lw, shadow);
            if (!shadow && c->fill)
            {
                gr_rect(w, 0, Y0, GR_W, fy - lw, c->fill, GR_K_FILL);
                gr_rect(w, 0, fy + fh + lw, GR_W, Y1, c->fill, GR_K_FILL);
            }
        }
    }

    /* grid inside the frame */
    if (c->type == 1 || c->type == 4)
    {
        for (int k = 1; k <= 2; k++)
        {
            int x = fx + fw * k / 3, y = fy + fh * k / 3;
            gr_seg(w, x, fy, x + lw, fy + fh, shadow);
            gr_seg(w, fx, y, fx + fw, y + lw, shadow);
        }
    }
    if (c->type == 2)
    {
        int gx = (fw * 382 + 500) / 1000, gy = (fh * 382 + 500) / 1000;
        gr_seg(w, fx + gx, fy, fx + gx + lw, fy + fh, shadow);
        gr_seg(w, fx + fw - gx, fy, fx + fw - gx + lw, fy + fh, shadow);
        gr_seg(w, fx, fy + gy, fx + fw, fy + gy + lw, shadow);
        gr_seg(w, fx, fy + fh - gy, fx + fw, fy + fh - gy + lw, shadow);
    }
    if (c->type == 3 || c->type == 4)
    {
        /* both diagonals of the frame, one connected run of columns per row */
        for (int r = 0; r < fh; r++)
        {
            int xa = fx + r * fw / fh, xb = fx + (r + 1) * fw / fh;
            if (xb <= xa) xb = xa + 1;
            int xe = xb + lw - 1;
            gr_seg(w, xa, fy + r, xe, fy + r + 1, shadow);
            gr_seg(w, 2 * fx + fw - xe, fy + r, 2 * fx + fw - xa, fy + r + 1, shadow);
        }
    }

    /* centre mark */
    int cx = fx + fw / 2, cy = fy + fh / 2;
    if (c->center == 1 || c->center == 2)
    {
        int h = c->center == 1 ? 40 : 12;
        gr_seg(w, cx - h, cy, cx + h + 1, cy + lw, shadow);
        gr_seg(w, cx, cy - h, cx + lw, cy + h + 1, shadow);
    }
    if (c->center == 3)
        gr_seg(w, cx - 1, cy - 1, cx + 1 + lw, cy + 1 + lw, shadow);

    /* safe areas (outlines inside their rectangle) */
    static const uint8_t pct[2] = { 90, 80 };
    for (int i = 0; i < 2; i++)
    {
        if (!(c->safe & (1 << i))) continue;
        int sw = (fw * pct[i] + 50) / 100, sh = (fh * pct[i] + 50) / 100;
        int sx = fx + (fw - sw) / 2, sy = fy + (fh - sh) / 2;
        gr_seg(w, sx, sy, sx + sw, sy + lw, shadow);
        gr_seg(w, sx, sy + sh - lw, sx + sw, sy + sh, shadow);
        gr_seg(w, sx, sy, sx + lw, sy + sh, shadow);
        gr_seg(w, sx + sw - lw, sy, sx + sw, sy + sh, shadow);
    }
}

static void gr_walk(struct gr_walk * w, int op, const struct gr_cfg * c, uint8_t * b, uint8_t * m)
{
    memset(w, 0, sizeof(*w));
    w->op = op; w->c = c; w->b = b; w->m = m;
    gr_shape(w, 0);
    if (c->line == 2) gr_shape(w, 1);
}

#ifdef CONFIG_R6_MOTION
/* M9.3: the motion display's snapshot (everything its pixels depend on; hashed, no padding) */
struct gr_md
{
    uint8_t n, evf;
    int16_t y0;
    uint16_t nex, pad;
    struct r6_grid_rect ex[R6_GRID_NEXCL];
    struct r6_md_rect r[R6MD_NRECT];
};
static struct gr_md gr_md_snap;
static uint32_t gr_md_sig = 0;
static int gr_md_valid = 0;
#endif
#ifdef CONFIG_R6_ETTR
static int gr_et_valid = 0;              /* M11.0: R6 ETTR's LV line is in ML's buffer */
#endif
#ifdef CONFIG_R6_TEMP
static int gr_tb_valid = 0;              /* M14.1: the temperature box is in ML's buffer */
#endif

/* drawn (stats, R6GD_DRAWN_GRID) from what is in ML's buffer: the grid and (M9.3) the motion display */
static void gr_drawn(void)
{
    int d = gr_snap_valid;
#ifdef CONFIG_R6_MOTION
    d |= gr_md_valid;
#endif
#ifdef CONFIG_R6_ETTR
    d |= gr_et_valid;
#endif
#ifdef CONFIG_R6_TEMP
    d |= gr_tb_valid;
#endif
    st.drawn = d;
    if (d) r6_gd_st.drawn |= R6GD_DRAWN_GRID;
    else r6_gd_st.drawn &= ~R6GD_DRAWN_GRID;
}

/* erase the drawn shape; 1 = ML's buffer changed */
static int gr_erase_snap(uint8_t * b, uint8_t * m)
{
    if (!gr_snap_valid) return 0;
    struct gr_walk w;
    gr_walk(&w, GR_ERASE, &gr_snap, b, m);
    gr_snap_valid = 0;
    st.erases++;
    gr_drawn();
    return w.changed != 0;
}

#ifdef CONFIG_R6_MOTION
/* the motion rectangles as line primitives (clipped to the image, minus the exclusions of the snapshot) */
static void gr_md_walk(struct gr_walk * w, int op, const struct gr_md * s, uint8_t * b, uint8_t * m)
{
    struct gr_cfg c;
    memset(&c, 0, sizeof(c));
    c.evf = s->evf; c.y0 = s->y0; c.nex = s->nex;
    for (int i = 0; i < s->nex; i++) c.ex[i] = s->ex[i];
    memset(w, 0, sizeof(*w));
    w->op = op; w->c = &c; w->b = b; w->m = m;
    for (int i = 0; i < s->n; i++) gr_rect(w, s->r[i].x0, s->r[i].y0, s->r[i].x1, s->r[i].y1, s->r[i].col, GR_K_LINE);
    w->c = 0;
}

static int gr_md_erase(uint8_t * b, uint8_t * m)
{
    if (!gr_md_valid) return 0;
    struct gr_walk w;
    gr_md_walk(&w, GR_ERASE, &gr_md_snap, b, m);
    gr_md_valid = 0;
    st.md_drawn = 0;
    gr_drawn();
    return w.changed != 0;
}

/* one step of the motion display: unchanged = repair, changed = erase the old, claim the new, nothing = erase */
static int gr_md_step(uint8_t * b, uint8_t * m, const struct r6_grid_rect * ex, int nex)
{
    struct gr_md want;
    memset(&want, 0, sizeof(want));
    int evf = r6_gd_evf ? 1 : 0;
    int n = r6_md_disp_wanted() ? r6_md_shape(want.r, R6MD_NRECT, evf) : 0;
    if (n <= 0) return gr_md_erase(b, m);
    want.n = n; want.evf = evf; want.y0 = evf ? R6_GD_EVF_IMG_Y : 0;
    nex = COERCE(nex, 0, R6_GRID_NEXCL);
    for (int i = 0; i < nex; i++) want.ex[i] = ex[i];
    want.nex = nex;
    uint32_t sig = r6_gd_bv_hash_mem(&want, sizeof(want));
    struct gr_walk w;
    int changed = 0;
    if (gr_md_valid && sig == gr_md_sig && !memcmp(&want, &gr_md_snap, sizeof(want)))
        gr_md_walk(&w, GR_CLAIM, &gr_md_snap, b, m);
    else
    {
        changed = gr_md_erase(b, m);
        gr_md_walk(&w, GR_CLAIM, &want, b, m);
        gr_md_snap = want;
        gr_md_sig = sig;
        gr_md_valid = 1;
        st.md_draws++;
    }
    st.md_drawn = 1; st.md_n = n; st.md_sig = sig;
    return changed | (w.changed != 0);
}
#endif

#ifdef CONFIG_R6_ETTR
/* M11.0: R6 ETTR's LV line (Show in LV, r6_ettr.c r6_ettr_lv_text): one text line in a black box at R6ETTR_LV_X /
 * R6ETTR_LV_Y, LCD shooting screen only, drawn by this step (overlay task, BMP_LOCK held, ML menu closed, overlay gate
 * passed), so the write-under conversion keeps Canon's elements on top, as for the grid and R6 Motion. The box's mirror
 * is cleared (no stale zebra / false color / peaking value equals a box pixel; the grid and the motion display skip the
 * box as foreign pixels and repair theirs once it is erased). Redrawn only when the text changed or a corner of the box
 * is gone (someone cleared it); erased with r6_grid_erase (r6_gd_erase), on the EVF, and when it is not wanted. */
#define GR_ET_FONT FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK)
static char gr_et_text[64];              /* the text in the box */
static int gr_et_w = 0, gr_et_h = 0;     /* the box */
#ifdef CONFIG_R6_ACLOCK
static int gr_et_own = 0;                /* M14.1: whose text the box holds: 1 R6 ETTR, 2 the astro clock */
#endif

static int gr_et_intact(const uint8_t * b)
{
    const int x0 = R6ETTR_LV_X, y0 = R6ETTR_LV_Y, x1 = x0 + gr_et_w - 1, y1 = y0 + gr_et_h - 1;
    return b[x0 + y0 * BMPPITCH] == COLOR_BLACK && b[x1 + y0 * BMPPITCH] == COLOR_BLACK
        && b[x0 + y1 * BMPPITCH] == COLOR_BLACK && b[x1 + y1 * BMPPITCH] == COLOR_BLACK;
}

static int gr_et_erase(uint8_t * b, uint8_t * m)
{
    if (!gr_et_valid) return 0;
    bmp_fill(COLOR_EMPTY, R6ETTR_LV_X, R6ETTR_LV_Y, gr_et_w, gr_et_h);
    gr_et_valid = 0;
    gr_et_text[0] = 0;
#ifdef CONFIG_R6_ACLOCK
    if (gr_et_own == 2) r6_ac_lv_count(0);  /* M14.1: the erase counts for the one who drew */
    else r6_ettr_lv_count(0);
#else
    r6_ettr_lv_count(0);
#endif
    gr_drawn();
    return 1;
}

static int gr_et_step(uint8_t * b, uint8_t * m)
{
    char t[sizeof(gr_et_text)];
#ifdef CONFIG_R6_ACLOCK
    /* M14.1: R6 ETTR's text first (E13); only without it the astro clock's (its short form if the long one is too wide) */
    int own = 1;
    if (r6_gd_evf) return gr_et_erase(b, m);
    if (!r6_ettr_lv_text(t, sizeof(t)))
    {
        if (!r6_ac_lv_text(t, sizeof(t), 0)) return gr_et_erase(b, m);
        if (bmp_string_width(GR_ET_FONT, t) + 4 > R6ETTR_LV_WMAX) r6_ac_lv_text(t, sizeof(t), 1);
        own = 2;
    }
    if (gr_et_valid && gr_et_own == own && !strcmp(t, gr_et_text) && gr_et_intact(b)) return 0;
#else
    if (r6_gd_evf || !r6_ettr_lv_text(t, sizeof(t))) return gr_et_erase(b, m);
    if (gr_et_valid && !strcmp(t, gr_et_text) && gr_et_intact(b)) return 0;
#endif
    int h = fontspec_height(GR_ET_FONT) + 4, n = strlen(t), w = 0;
    while (n > 0 && (w = bmp_string_width(GR_ET_FONT, t) + 4) > R6ETTR_LV_WMAX) t[--n] = 0;   /* cut at the box */
    if (n <= 0) return gr_et_erase(b, m);
    int changed = gr_et_erase(b, m);
    bmp_fill(COLOR_BLACK, R6ETTR_LV_X, R6ETTR_LV_Y, w, h);
    bmp_printf(GR_ET_FONT, R6ETTR_LV_X + 2, R6ETTR_LV_Y + 2, "%s", t);
    for (int y = 0; y < h; y++) memset(m + R6ETTR_LV_X + (R6ETTR_LV_Y + y) * BMPPITCH, 0, w);
    gr_et_w = w;
    gr_et_h = h;
    snprintf(gr_et_text, sizeof(gr_et_text), "%s", t);
    gr_et_valid = 1;
#ifdef CONFIG_R6_ACLOCK
    gr_et_own = own;
    if (own == 2) r6_ac_lv_count(1);
    else r6_ettr_lv_count(1);
#else
    r6_ettr_lv_count(1);
#endif
    gr_drawn();
    return changed | 1;
}
#endif

#ifdef CONFIG_R6_TEMP
/* M14.1: the temperature box (r6_temp.c r6_temp_lv_text, spec m14/m141 5.2): right-aligned at R6TB_X1, top R6TB_Y0, under
 * Canon's and ML's histogram place, outside the ETTR line; fixed width (the widest text "A -40 degC! S99"), white on black
 * or (warning) black on yellow; LCD shooting screen only, the same rules as the ETTR line (mirror cleared, drawn only on a
 * new text / colour or a missing corner: the corners are checked against the current fill colour, skeptic M7) */
#define GR_TB_FONT  FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK)
#define GR_TB_FONTW FONT(FONT_MED, COLOR_BLACK, COLOR_YELLOW)
static char gr_tb_text[24];              /* the text in the box */
static int gr_tb_w = 0, gr_tb_h = 0, gr_tb_col = COLOR_BLACK;

static int gr_tb_intact(const uint8_t * b)
{
    const int x0 = R6TB_X1 - gr_tb_w, y0 = R6TB_Y0, x1 = R6TB_X1 - 1, y1 = y0 + gr_tb_h - 1;
    return b[x0 + y0 * BMPPITCH] == gr_tb_col && b[x1 + y0 * BMPPITCH] == gr_tb_col
        && b[x0 + y1 * BMPPITCH] == gr_tb_col && b[x1 + y1 * BMPPITCH] == gr_tb_col;
}

static int gr_tb_erase(uint8_t * b, uint8_t * m)
{
    if (!gr_tb_valid) return 0;
    bmp_fill(COLOR_EMPTY, R6TB_X1 - gr_tb_w, R6TB_Y0, gr_tb_w, gr_tb_h);
    gr_tb_valid = 0;
    gr_tb_text[0] = 0;
    r6_temp_lv_count(0);
    gr_drawn();
    return 1;
}

static int gr_tb_step(uint8_t * b, uint8_t * m)
{
    char t[sizeof(gr_tb_text)];
    int warn = 0;
    if (r6_gd_evf || !r6_temp_lv_text(t, sizeof(t), &warn)) return gr_tb_erase(b, m);
    const int col = warn ? COLOR_YELLOW : COLOR_BLACK;
    if (gr_tb_valid && col == gr_tb_col && !strcmp(t, gr_tb_text) && gr_tb_intact(b)) return 0;
    if (!gr_tb_w)
    {
        gr_tb_w = bmp_string_width(GR_TB_FONT, "A -40" SYM_DEGREE "C! S99") + 4;
        gr_tb_h = fontspec_height(GR_TB_FONT) + 4;
    }
    int changed = gr_tb_erase(b, m);
    const int x0 = R6TB_X1 - gr_tb_w;
    bmp_fill(col, x0, R6TB_Y0, gr_tb_w, gr_tb_h);
    bmp_printf(warn ? GR_TB_FONTW : GR_TB_FONT, x0 + 2, R6TB_Y0 + 2, "%s", t);
    for (int y = 0; y < gr_tb_h; y++) memset(m + x0 + (R6TB_Y0 + y) * BMPPITCH, 0, gr_tb_w);
    snprintf(gr_tb_text, sizeof(gr_tb_text), "%s", t);
    gr_tb_col = col;
    gr_tb_valid = 1;
    r6_temp_lv_count(1);
    gr_drawn();
    return changed | 1;
}
#endif

int r6_grid_erase(void)
{
    uint8_t * b = bmp_vram(), * m = get_bvram_mirror();
    if (!b || !m) return 0;
    int c = gr_erase_snap(b, m);
#ifdef CONFIG_R6_MOTION
    c |= gr_md_erase(b, m);
#endif
#ifdef CONFIG_R6_ETTR
    c |= gr_et_erase(b, m);
#endif
#ifdef CONFIG_R6_TEMP
    c |= gr_tb_erase(b, m);
#endif
    return c;
}

static uint32_t gr_pack(const struct gr_cfg * c)
{
    return 1 | (c->type << 4) | (c->center << 8) | (c->aspect << 12) | (c->style << 16) | (c->safe << 20)
        | ((uint32_t) COERCE(gr_color, 0, GR_NCOL - 1) << 24) | ((uint32_t) c->line << 28);
}

int r6_grid_step(const struct r6_grid_rect * ex, int n)
{
    uint8_t * b = bmp_vram(), * m = get_bvram_mirror();
    if (!b || !m) return 0;
    uint64_t t0 = get_us_clock();
    st.steps++;
    struct gr_cfg want;
    struct gr_walk w;
    int changed = 0, drew = 0;
    if (!gr_build(&want, ex, n))
    {
        st.off++;
        changed = gr_erase_snap(b, m);
    }
    else
    {
        if (os.y0 != want.y0) st.y0_mismatch++;
        uint32_t sig = r6_gd_bv_hash_mem(&want, sizeof(want));
        if (gr_snap_valid && sig == gr_snap_sig && !memcmp(&want, &gr_snap, sizeof(want)))
        {
            gr_walk(&w, GR_CLAIM, &gr_snap, b, m);          /* unchanged: repair only */
            if (w.repaired || w.claimed) { st.repairs++; st.repair_px += w.repaired + w.claimed; }
        }
        else
        {
            changed = gr_erase_snap(b, m);
            gr_walk(&w, GR_CLAIM, &want, b, m);
            gr_snap = want;
            gr_snap_sig = sig;
            gr_snap_valid = 1;
            st.draws++;
            drew = 1;
            st.cfg = gr_pack(&want);
            st.sig = sig;
            st.evf = want.evf;
            st.y0 = want.y0;
            st.excl = want.nex;
            st.px_line = w.px[GR_K_LINE];
            st.px_fill = w.px[GR_K_FILL];
            st.px_shadow = w.px[GR_K_SHADOW];
        }
        changed |= w.changed != 0;
        st.claim_ovl += w.ovl;
        st.cleanup_px += w.cleanup;
        st.skip_foreign = w.foreign;             /* of the last walk */
        st.skip_mark = w.mark;
        st.drawn = 1;
        r6_gd_st.drawn |= R6GD_DRAWN_GRID;
    }
#ifdef CONFIG_R6_MOTION
    changed |= gr_md_step(b, m, ex, n);                 /* M9.3: after the grid (the grid wins where they overlap) */
    gr_drawn();
#endif
#ifdef CONFIG_R6_TEMP
    changed |= gr_tb_step(b, m);                        /* M14.1: the temperature box, before the ETTR line */
    gr_drawn();
#endif
#ifdef CONFIG_R6_ETTR
    changed |= gr_et_step(b, m);                        /* M11.0: R6 ETTR's LV line, last (over the grid / motion pixels) */
    gr_drawn();
#endif
    if (changed) st.changed++;
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    st.us_last = us;
    if (us > st.us_max) st.us_max = us;
    if (drew && us > st.us_draw_max) st.us_draw_max = us;
    return changed;
}

/* ---- menu (Overlay tab) ---- */
static const char * gr_type_names[] = { "OFF", "Thirds", "Golden ratio", "Diagonals", "Thirds + diagonals" };
static const char * gr_center_names[] = { "OFF", "Cross", "Small cross", "Dot" };
static const char * gr_aspect_names[] = { "OFF (3:2)", "1:1", "4:5 landscape", "4:5 portrait", "4:3", "16:9",
                                                "1.85:1", "2.39:1" };
static const char * gr_style_names[] = { "Frame", "Frame + mask 50%", "Frame + mask 75%", "Solid mask" };
static const char * gr_safe_names[] = { "OFF", "90%", "80%", "90% + 80%" };
static const char * gr_color_names[] = { "Grey", "White", "Black", "Yellow", "Cyan", "Green", "Red", "White 60%" };
static const char * gr_line_names[] = { "1 px", "2 px", "1 px + shadow" };

static MENU_UPDATE_FUNC(gr_main_update)
{
    if (!gr_on) return;
    const char * g = gr_type ? gr_type_names[COERCE(gr_type, 0, 4)] : "";
    const char * a = gr_aspect ? gr_aspect_names[COERCE(gr_aspect, 0, GR_NAR - 1)] : "";
    if (gr_type || gr_aspect)
        MENU_SET_VALUE("%s%s%s", g, (gr_type && gr_aspect) ? ", " : "", a);
    else if (gr_center || gr_safe)
        MENU_SET_VALUE("%s", gr_center ? "Center mark" : "Safe areas");
    if (!gr_wanted())
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Nothing selected: choose a grid, center mark, aspect frame or safe area.");
    else if (r6_gd_st.last_outside_menu != R6GD_OK)
        MENU_SET_WARNING(MENU_WARN_INFO, "Last LV screen: not drawn (%s).", r6_gd_reason(r6_gd_st.last_outside_menu));
    else
        MENU_SET_WARNING(MENU_WARN_INFO, "Under Canon's display: AF frame, Canon grid and icons stay on top.");
}

static MENU_UPDATE_FUNC(gr_style_update)
{
    if (gr_style && gr_aspect != GR_AR_4_3)
        MENU_SET_WARNING(MENU_WARN_INFO, "Masks in the EVF; on the LCD only 4:3 (Canon's bars cover the others).");
}

static struct menu_entry gr_menu[] = {
    {
        .name    = "Grids & masks",
        .priv    = &gr_on,
        .max     = 1,
        .update  = gr_main_update,
        .depends_on = DEP_GLOBAL_DRAW,
        .help    = "Grid, center mark, aspect frames / masks, safe areas (computed, no files).",
        .help2   = "Drawn under Canon's display, LCD and EVF shooting screen. Saved; default OFF.",
        .children = (struct menu_entry[]) {
            {
                .name    = "Grid",
                .priv    = &gr_type,
                .max     = 4,
                .choices = gr_type_names,
                .help    = "Lines relative to the aspect frame (the whole image without one).",
                .help2   = "Canon's own grid covers ML's lines (Canon stays on top).",
            },
            {
                .name    = "Center mark",
                .priv    = &gr_center,
                .max     = 3,
                .choices = gr_center_names,
                .help    = "Center of the aspect frame.",
            },
            {
                .name    = "Aspect frame",
                .priv    = &gr_aspect,
                .max     = GR_NAR - 1,
                .choices = gr_aspect_names,
                .help    = "Frame lines just outside the frame. 4:5 landscape = a 5:4 frame.",
                .help2   = "Assumes Canon's image ratio 3:2 (cropping / aspect ratio: Full).",
            },
            {
                .name    = "Aspect style",
                .priv    = &gr_style,
                .max     = 3,
                .choices = gr_style_names,
                .update  = gr_style_update,
                .help    = "Masks outside the frame: 50 % / 75 % dark or solid.",
                .help2   = "EVF: all formats. LCD: only 4:3, between Canon's top and bottom bars.",
            },
            {
                .name    = "Safe areas",
                .priv    = &gr_safe,
                .max     = 3,
                .choices = gr_safe_names,
                .help    = "Outlines at 90 % / 80 % of the aspect frame.",
            },
            {
                .name    = "Color",
                .priv    = &gr_color,
                .max     = GR_NCOL - 1,
                .choices = gr_color_names,
                .help    = "Line color. White 60%: semi-transparent white.",
            },
            {
                .name    = "Line",
                .priv    = &gr_line,
                .max     = 2,
                .choices = gr_line_names,
                .help    = "Line width; shadow: 1 px black (white for black lines) right / below.",
            },
            MENU_EOL,
        },
    },
};

static void gr_init()
{
    menu_add("Overlay", gr_menu, COUNT(gr_menu));
}

INIT_FUNC("r6_grid", gr_init);

#endif /* CONFIG_R6_GRID */
