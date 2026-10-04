/*
 * R6.190 M7.3: first real ML LiveView overlay, Overlay > "Histogram (Canon)".
 *
 * Draws Canon's own LV display histogram (confirmed on the camera in M7.2a:
 * ctx = MEM(0x11F44+4), DISP sets 0/1 of 4 x 256 u32 bins in Canon's
 * uncached heap, channel order Y, R, G, B, every array sums to numPix) as a
 * small box into ML's indexed buffer. Nothing of Canon's is called or
 * written: plain RAM reads with the lvprobe.c rules (every Canon pointer
 * non-zero, aligned, whole range inside 0x1000..0x3FFFFFFF or the verified
 * uncached alias 0x40001000..0x7FFFFFFF), drawing through ML's bmp_* path
 * (BMP_LOCK, ml_refresh_display_needed); the refresh guard and the
 * ownership-aware conversion (r6_layer.c) put the pixels into layer 0 and
 * erase them again.
 *
 * Deliberately NOT FEATURE_GLOBAL_DRAW / zebra.c: that enables many
 * unreviewed paths on R6 (lvinfo bars, zebra.c writes through
 * bmp_vram_real() into Canon's RGBA layer with the wrong pitch, the
 * full-alpha refresh, YUV LV buffer readers that are NULL on R6).
 *
 * Shown only when all hold (checked every 100 ms, drawn at most 5 per s,
 * only when Canon's FrameNum changed):
 *  - LV: ML's lv, or Canon's PROP_LV_ACTION == 0 as recorded by
 *    propvalues.c (the same rule as the refresh guard's rate limit). So the
 *    overlay also works with "LV flag follows Canon" OFF (its default): it
 *    only reads, and switching lv on would enable ML's other LV code paths.
 *  - no ML menu (gui_menu_shown() == 0) and CURRENT_GUI_MODE == 0
 *  - LV_OVERLAYS_MODE != 6 (Canon's settings screen, OLC, M7.1 table)
 *  - DISPLAY_IS_ON, display mask exactly 0x01 = LCD only (not with the EVF
 *    0x10 or the HDMI/LINE bits 0x1100; the EVF has another geometry, and
 *    the one-off EVF -> LCD hang of M7.1 is not explained yet), LiveViewApp
 *    dialog != 0
 *  - published pointers = DISP set 0 or 1, each used array passes the range
 *    check and sums to numPix, and the set is unchanged after the copy
 *  - RandomData FrameNum readable; unchanged for 1 s = stale
 * Otherwise ML's box is erased (bmp_fill with COLOR_EMPTY of exactly the
 * box, under BMP_LOCK); while the ML menu is open nothing is touched.
 * A new frame whose 128 bar heights and clip marks equal the box on screen
 * is not drawn again and requests no refresh (same_bins); a redraw is forced
 * after the ML menu was open and when the box frame is no longer intact in
 * ML's indexed buffer (someone cleared it). So a static scene costs no
 * XimrExe; a changing one at most 5 per second.
 * The setting is NOT saved in ML's config file (plain variable, OFF at every
 * boot) until the first camera test has passed.
 *
 * M7.5: the same reads also feed ML's own histogram (r6_ml_hist_fill, called
 * by src/zebra.c hist_build instead of its FaceYuv pixel loop), with its own
 * copy buffer and FrameNum state; the box here steps aside while ML's
 * histogram is enabled (R6H_MLHIST), so only one of them is drawn.
 *
 * Box: 128 x 64 bins area + 2 px frame at ML (572,50)..(704,118), right
 * below Canon's Q icon and top bar on the LCD shooting screen (M5 photo:
 * Canon's top row ends at about y 48, the Q icon covers x 631..710 up to
 * y 46, left icons x < 95, bottom bar from y ~388; the status line of the
 * LV probe starts at y 120). 256 -> 128 bins (pairs summed), linear scale
 * to the highest bin; bin 255 > 0 (clipping) is marked by a small square
 * at the top right in the channel colour.
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "propvalues.h"
#include "tasks.h"
#include "r6_overlays.h"

#ifdef FEATURE_R6_CANON_HISTOGRAM

#if !defined(CONFIG_R6_LAYER_OWNERSHIP) || (defined(FEATURE_GLOBAL_DRAW) && !defined(CONFIG_R6_GLOBAL_DRAW))
#error "r6_overlays.c: needs the ownership-aware refresh (and, with global draw, the R6 global draw rules)"
#endif
#ifdef FEATURE_HISTOGRAM
#include "histogram.h"          /* hist_draw, struct Histogram (M7.5) */
extern int get_global_draw_setting(void);   /* zebra.c */
extern int lv_follow_canon_get(void);       /* propvalues.c */
#endif

extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c */

/* ---- Canon RAM, R6 1.9.0 (evidence: lvprobe.c, M7.2a camera logs) ------ */
#define OV_HIST_STATE    0x11F44   /* LvHistController: +4 = ctx (HistDisp 0xE012C0B8) */
#define OV_HIST_CTX_SIZE 0xD0      /* init 0xE012BC56 */
#define OV_DTS           0x1200C   /* DataStore: +0x10 use working copy, +0x1C ring index (0xE079D026) */
#define OV_RD_WORK       0x131B70  /* RandomData working copy */
#define OV_RD_RING       0x132A70  /* published RandomData pointers */
#define OV_RD_RING_N     8
#define OV_RD_SIZE       0xF00
#define OV_MEMIF_WIN     0x1355A4  /* RAM copy of the static memif windows, [11] = copied flag */
#define OV_DISPDEV_PTR   0xEA40    /* [[0xEA40]+0x18] = display mask (0xE01694C8) */
#define OV_MASK_PNL      0x01
#define OV_MASK_EVF      0x10
#define OV_LVAPP_DIALOG  0x1624C   /* LiveViewApp_dialog (stubs.S) */
#define OV_OLC           6         /* LV_OVERLAYS_MODE of Canon's settings screen (docs/m7_1_zensus.md) */

/* ---- box (ML coordinates, 720x480 = LCD) ---- */
#define R6H_X       574
#define R6H_Y       52
#define R6H_W       128
#define R6H_H       64
#define R6H_FRAME   2
#define R6H_TICK_MS 100
#define R6H_RATE_MS 200            /* at most 5 draws per second */
#define R6H_STALE_MS 1000
#if R6H_X - R6H_FRAME != R6H_BOX_X0 || R6H_Y - R6H_FRAME != R6H_BOX_Y0 \
 || R6H_X + R6H_W + R6H_FRAME != R6H_BOX_X1 || R6H_Y + R6H_H + R6H_FRAME != R6H_BOX_Y1
#error "r6_overlays.h R6H_BOX_* must match the box (M9.1 grid exclusion)"
#endif

#define RD(a) (*(volatile uint32_t *)(a))

/* 0 OFF, 1 Luma, 2 RGB; M7.3: not a CONFIG_INT yet (not saved, OFF at every
 * boot) until the first camera test of the overlay has passed */
int r6h_mode = 0;

static struct r6_overlay_stats st = { .set = -1 };
static uint32_t r6h_bins[4][256];
static uint32_t r6h_h128[4][128];
static uint32_t r6h_last_frame = 0;
static int r6h_t_frame = 0, r6h_frame_seen = 0;
static uint32_t r6h_drawn[4][128];          /* bar heights of the box on screen */
static uint32_t r6h_drawn_clip = 0;         /* bit c: clip mark of channel c on screen */
static int r6h_drawn_mode = -1;
static int r6h_force = 1;                   /* next draw repaints the whole box */

static const char * const r6h_names[R6H_NREASONS] = {
    "shown", "off", "no LV", "ML menu", "Canon GUI", "OLC", "display off", "not LCD",
    "no LV dialog", "no hist set", "bad pointer", "sum != numPix", "no FrameNum", "stale",
    "ML histogram on", "torn",
};

const char * r6_overlay_reason(uint32_t s)
{
    return s < R6H_NREASONS ? r6h_names[s] : "?";
}

const struct r6_overlay_stats * r6_overlay_stats(void)
{
    return &st;
}

/* ---- guarded reads (same rule as lvprobe.c) ---- */
static int ov_uc_ok(void)
{
    if (!RD(OV_MEMIF_WIN + 0x2C)) return 0;
    for (int i = 0; i < 4; i++)
        if (RD(OV_MEMIF_WIN + 4 * (i + 4)) != RD(OV_MEMIF_WIN + 4 * i)) return 0;
    return 1;
}

static int ov_region(uint32_t a, int uc)
{
    if (a >= 0x1000 && a < 0x40000000) return 1;
    if (uc && a >= 0x40001000 && a < 0x80000000) return 2;
    return 0;
}

/* [a, a+n) aligned and inside one allowed region */
static int ov_ram_ok(uint32_t a, uint32_t n, int uc)
{
    if ((a & 3) || !n) return 0;
    int r = ov_region(a, uc);
    return r && a + n - 1 > a && ov_region(a + n - 1, uc) == r;
}

/* DISP set 0/1 of the published payload, else -1 (lvprobe.c lp_hist_ptrs) */
static int ov_hist_set(uint32_t ctx, uint32_t p[4], int uc)
{
    p[0] = p[1] = p[2] = p[3] = 0;
    if (!ctx || !ov_ram_ok(ctx, OV_HIST_CTX_SIZE, uc)) return -2;
    for (int i = 0; i < 4; i++) p[i] = RD(ctx + 8 + 4 * i);
    static const uint8_t first[2] = { 0x4C, 0x50 };      /* DISP set 0 / 1, stride 8 */
    for (int s = 0; s < 2; s++)
    {
        int m = 1;
        for (int i = 0; i < 4; i++)
            if (RD(ctx + first[s] + 8 * i) != p[i]) m = 0;
        if (m) return s;
    }
    return -1;
}

/* RandomData FrameNum as DTS_GetAllRandomData 0xE079D026 finds it (not called) */
static int ov_framenum(uint32_t * fr, int uc)
{
    uint32_t base;
    if (!ov_ram_ok(OV_DTS + 0x10, 0x10, uc)) return 0;
    if (RD(OV_DTS + 0x10)) base = OV_RD_WORK;
    else
    {
        uint32_t idx = RD(OV_DTS + 0x1C);
        if (idx >= OV_RD_RING_N) return 0;
        base = RD(OV_RD_RING + 4 * idx);
        if (!base || !ov_ram_ok(base, OV_RD_SIZE, uc)) return 0;
    }
    *fr = RD(base);
    return 1;
}

static int ov_in_lv(void)
{
    uint32_t v = 1;
    int len = 0;
    if (lv) return 1;
    return lv_action_raw_get(&v, &len) && len == 4 && v == 0;
}

/* GUI / display conditions; R6H_SHOWN = all fine */
static int r6h_gui_state(void)
{
    if (!r6h_mode) return R6H_OFF;
    if (gui_menu_shown()) return R6H_MENU;
#ifdef FEATURE_HISTOGRAM
    /* M7.4: ML's own histogram (Overlay > Global Draw + Histogram) uses the
     * same box position: only one of them at a time. Only when ML's can
     * draw at all (LV flag follows Canon ON), else no histogram would show */
    if (hist_draw && get_global_draw_setting() && lv_follow_canon_get()) return R6H_MLHIST;
#endif
    if (!ov_in_lv()) return R6H_NO_LV;
    if (CURRENT_GUI_MODE != 0) return R6H_GUI;
    if (LV_OVERLAYS_MODE == OV_OLC) return R6H_OLC;
    if (!DISPLAY_IS_ON) return R6H_DISP_OFF;
    uint32_t dd = RD(OV_DISPDEV_PTR);
    if (!ov_ram_ok(dd, 0x1C, ov_uc_ok())) return R6H_NOT_PANEL;
    uint32_t mask = RD(dd + 0x18);
    if (mask != OV_MASK_PNL) return R6H_NOT_PANEL;      /* LCD only: no EVF, no HDMI/LINE bits */
    if (!RD(OV_LVAPP_DIALOG)) return R6H_NO_DIALOG;
    return R6H_SHOWN;
}

/* copy Canon's arrays c0..c1 (Y = 0, R, G, B = 1..3) of the published DISP
 * set into bins; returns R6H_SHOWN (*numpix, *frame set), a failure state,
 * or -1 = torn (set changed during the copy: retry). *set_out = the set. */
static int r6h_read_bins(uint32_t bins[4][256], int c0, int c1, uint32_t * frame, uint32_t * numpix, int * set_out)
{
    int uc = ov_uc_ok();
    uint32_t ctx = RD(OV_HIST_STATE + 4), p[4], q[4];
    int set = ov_hist_set(ctx, p, uc);
    *set_out = set;
    if (set < 0) return R6H_NO_SET;
    uint32_t n = RD(ctx + 4);
    for (int c = c0; c <= c1; c++)
    {
        if (!p[c] || !ov_ram_ok(p[c], 0x400, uc)) return R6H_BAD_PTR;
        uint32_t s = 0;
        for (int i = 0; i < 256; i++)
        {
            uint32_t v = RD(p[c] + 4 * i);       /* 32-bit loads, uncached heap */
            bins[c][i] = v;
            s += v;
        }
        if (!n || s != n) return R6H_SUM;
    }
    if (ov_hist_set(ctx, q, uc) != set || RD(ctx + 4) != n) return -1;
    for (int i = 0; i < 4; i++) if (q[i] != p[i]) return -1;
    if (!ov_framenum(frame, uc)) return R6H_NO_FRAME;
    *numpix = n;
    return R6H_SHOWN;
}

/* copy the arrays used by mode (Luma: Y = 0; RGB: 1..3); returns R6H_SHOWN,
 * a failure state, or -1 = torn (set changed during the copy: retry) */
static int r6h_read(int mode, uint32_t * frame)
{
    int c0 = mode == 1 ? 0 : 1, c1 = mode == 1 ? 0 : 3;
    uint32_t n = 0;
    int set = -1;
    int s = r6h_read_bins(r6h_bins, c0, c1, frame, &n, &set);
    st.set = set;
    if (s == R6H_SHOWN) st.numpix = n;
    return s;
}

/* R, G, B lit -> colour (additive) */
static const uint8_t r6h_rgb_col[8] = {
    COLOR_BLACK, COLOR_RED, COLOR_GREEN2, COLOR_YELLOW,
    COLOR_LIGHT_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE,
};

/* the 4 outer frame corners of ML's box are still in the indexed buffer
 * (BMP_LOCK held by the caller) */
static int r6h_box_intact(const uint8_t * bv)
{
    const int x0 = R6H_X - R6H_FRAME, x1 = R6H_X + R6H_W + R6H_FRAME - 1;
    const int y0 = R6H_Y - R6H_FRAME, y1 = R6H_Y + R6H_H + R6H_FRAME - 1;
    return bv[x0 + y0 * BMPPITCH] == COLOR_GRAY(50) && bv[x1 + y0 * BMPPITCH] == COLOR_GRAY(50)
        && bv[x0 + y1 * BMPPITCH] == COLOR_GRAY(50) && bv[x1 + y1 * BMPPITCH] == COLOR_GRAY(50);
}

static void r6h_draw(int mode)
{
    int c0 = mode == 1 ? 0 : 1, c1 = mode == 1 ? 0 : 3;
    uint32_t peak = 1;
    for (int c = c0; c <= c1; c++)
        for (int i = 0; i < 128; i++)
        {
            uint32_t v = r6h_bins[c][2 * i] + r6h_bins[c][2 * i + 1];
            r6h_h128[c][i] = v;
            if (v > peak) peak = v;
        }
    /* bar heights 0..R6H_H, any non-zero pair at least 1 px */
    for (int c = c0; c <= c1; c++)
        for (int i = 0; i < 128; i++)
        {
            uint64_t v = r6h_h128[c][i];
            r6h_h128[c][i] = v ? (uint32_t) ((v * R6H_H + peak - 1) / peak) : 0;
        }
    uint32_t clip = 0;
    for (int c = 0; c < 4; c++)
    {
        st.clip[c] = (c >= c0 && c <= c1) ? r6h_bins[c][255] : 0;
        if (st.clip[c]) clip |= 1 << c;
    }
    st.peak = peak;

    BMP_LOCK(
        uint8_t * bv = bmp_vram_indexed_ready() ? bmp_vram() : NULL;
        int same = 0;
        if (bv && !gui_menu_shown() && st.box && !r6h_force && mode == r6h_drawn_mode && clip == r6h_drawn_clip
            && r6h_box_intact(bv))
        {
            same = 1;
            for (int c = c0; c <= c1 && same; c++)
                if (memcmp(r6h_h128[c], r6h_drawn[c], sizeof(r6h_drawn[c]))) same = 0;
        }
        if (same)
            st.same_bins++;                     /* box on screen already shows this: no draw, no refresh */
        else if (bv && !gui_menu_shown())
        {
            /* a repaint of a box that is (or may be) on screen, forced after the menu or a foreign clear */
            if (st.box && (r6h_force || !r6h_box_intact(bv))) st.forced++;
            /* frame: 1 px grey line, 1 px black gap */
            for (int y = -R6H_FRAME; y < R6H_H + R6H_FRAME; y++)
                for (int x = -R6H_FRAME; x < R6H_W + R6H_FRAME; x++)
                {
                    if (x >= 0 && x < R6H_W && y >= 0 && y < R6H_H) continue;
                    int edge = x == -R6H_FRAME || y == -R6H_FRAME || x == R6H_W + R6H_FRAME - 1 || y == R6H_H + R6H_FRAME - 1;
                    bv[(R6H_X + x) + (R6H_Y + y) * BMPPITCH] = edge ? COLOR_GRAY(50) : COLOR_BLACK;
                }
            for (int y = 0; y < R6H_H; y++)
            {
                uint8_t * row = bv + R6H_X + (R6H_Y + y) * BMPPITCH;
                uint32_t level = R6H_H - y;           /* bar reaches this row if height >= level */
                for (int x = 0; x < R6H_W; x++)
                {
                    if (mode == 1)
                        row[x] = r6h_h128[0][x] >= level ? COLOR_WHITE : COLOR_BLACK;
                    else
                        row[x] = r6h_rgb_col[(r6h_h128[1][x] >= level) | ((r6h_h128[2][x] >= level) << 1)
                                             | ((r6h_h128[3][x] >= level) << 2)];
                }
            }
            /* clipping marks (bin 255 > 0): 4x4 squares at the top right */
            for (int c = c0; c <= c1; c++)
            {
                if (!st.clip[c]) continue;
                int col = mode == 1 ? COLOR_RED : r6h_rgb_col[1 << (c - 1)];
                int x0 = R6H_W - 5 - 6 * (c1 - c);
                for (int y = 1; y < 5; y++)
                    for (int x = x0; x < x0 + 4; x++)
                        bv[(R6H_X + x) + (R6H_Y + y) * BMPPITCH] = col;
            }
            for (int c = c0; c <= c1; c++) memcpy(r6h_drawn[c], r6h_h128[c], sizeof(r6h_drawn[c]));
            r6h_drawn_clip = clip;
            r6h_drawn_mode = mode;
            r6h_force = 0;
            st.box = 1;
            st.draws++;
            ml_refresh_display_needed = 1;
        }
    )
}

/* erase exactly ML's box (never while the ML menu owns the screen) */
static void r6h_erase(void)
{
    BMP_LOCK(
        if (!gui_menu_shown())
        {
            bmp_fill(COLOR_EMPTY, R6H_X - R6H_FRAME, R6H_Y - R6H_FRAME, R6H_W + 2 * R6H_FRAME, R6H_H + 2 * R6H_FRAME);
            st.box = 0;
            st.erases++;
        }
    )
}

static void r6h_step(void)
{
    int now = get_ms_clock();
    int mode = r6h_mode;
    st.ticks++;
    st.mode = mode;

    int s = r6h_gui_state();
    if (s == R6H_MENU)
    {
        /* the ML menu draws over the whole screen and clears it on close:
         * repaint the whole box afterwards */
        if (st.state != R6H_MENU) st.last_off_menu = st.state;
        r6h_force = 1;
        st.state = s;
        st.reasons[s]++;
        return;
    }
    if (s == R6H_SHOWN)
    {
        if (st.box && now - st.t_draw < R6H_RATE_MS && now >= st.t_draw) { st.rate++; return; }
        uint64_t t0 = get_us_clock();
        uint32_t fr = 0;
        s = r6h_read(mode, &fr);
        if (s < 0) { st.torn++; return; }      /* retry next tick, box unchanged */
        if (s == R6H_SHOWN)
        {
            if (!r6h_frame_seen || fr != r6h_last_frame)
            {
                r6h_frame_seen = 1;
                r6h_last_frame = fr;
                r6h_t_frame = now;
            }
            else if (now - r6h_t_frame > R6H_STALE_MS || now < r6h_t_frame)
                s = R6H_STALE;
            else if (st.box)
            {
                st.same_frame++;                 /* nothing new: keep the box */
                st.state = R6H_SHOWN;
                st.reasons[R6H_SHOWN]++;
                return;
            }
        }
        if (s == R6H_SHOWN)
        {
            r6h_draw(mode);
            st.frame = fr;
            st.t_draw = now;
            uint32_t us = (uint32_t) (get_us_clock() - t0);
            st.draw_us_last = us;
            if (us > st.draw_us_max) st.draw_us_max = us;
        }
    }
    if (s != R6H_SHOWN)
    {
        if (st.box) r6h_erase();
        if (s != R6H_STALE) r6h_frame_seen = 0;
    }
    st.state = s;
    st.reasons[s]++;
}

static void r6h_task(void * unused)
{
    TASK_LOOP
    {
        msleep(R6H_TICK_MS);
        r6h_step();
    }
}

TASK_CREATE("r6_hist_task", r6h_task, 0, 0x1f, 0x1000);

#ifdef FEATURE_HISTOGRAM
/* ---- M7.5: ML's own histogram from Canon's bins (r6_overlays.h) ----
 * Own buffer and FrameNum state (the box above keeps its own; both may read
 * at the same time, the reads are plain loads). */
#if HIST_WIDTH != 128
#error "r6_ml_hist_fill: Canon's 256 bins map to 128 ML bins as pairs"
#endif
static struct r6_mlhist_stats mst = { .set = -1 };
static uint32_t r6m_bins[4][256];
static uint32_t r6m_frame = 0;
static int r6m_t_frame = 0, r6m_seen = 0;

const struct r6_mlhist_stats * r6_mlhist_stats(void)
{
    return &mst;
}

int r6_ml_hist_fill(struct Histogram * h, int rgb)
{
    uint64_t t0 = get_us_clock();
    uint32_t fr = 0, n = 0;
    int set = -1, s = -1;
    mst.calls++;
    /* torn, and (fix round) no DISP set - ov_hist_set also refuses a half
     * published pointer set - are retried, at most 3 reads */
    for (int i = 0; i < 3 && (s < 0 || s == R6H_NO_SET); i++)
    {
        s = r6h_read_bins(r6m_bins, 0, rgb ? 3 : 0, &fr, &n, &set);
        if (s < 0) mst.torn++;
        else if (s == R6H_NO_SET) mst.noset++;
    }
    if (s < 0) s = R6H_TORN;
    mst.set = set;
    int now = get_ms_clock();
    if (s == R6H_SHOWN)
    {
        if (!r6m_seen || fr != r6m_frame)
        {
            r6m_seen = 1;
            r6m_frame = fr;
            r6m_t_frame = now;
        }
        else if (now - r6m_t_frame > R6H_STALE_MS || now < r6m_t_frame)
            s = R6H_STALE;                       /* Canon's LV histogram frozen: nothing shown */
    }
    else if (s != R6H_TORN)
        r6m_seen = 0;
    if (s == R6H_SHOWN)
    {
        memset(h, 0, sizeof(*h));
        h->is_rgb = rgb;
        uint32_t mx = 0;
        for (int i = 0; i < HIST_WIDTH; i++)
        {
            uint32_t y = r6m_bins[0][2 * i] + r6m_bins[0][2 * i + 1];
            h->hist[i] = y;
            if (i && y > mx) mx = y;             /* hist_add_pixel: bin 0 never sets the maximum */
            if (rgb)
            {
                h->hist_r[i] = r6m_bins[1][2 * i] + r6m_bins[1][2 * i + 1];
                h->hist_g[i] = r6m_bins[2][2 * i] + r6m_bins[2][2 * i + 1];
                h->hist_b[i] = r6m_bins[3][2 * i] + r6m_bins[3][2 * i + 1];
            }
        }
        /* fix round: max >= 2 - hist_draw_image's log scale divides by
         * log_length(max), which is 0 for max 1 (a black frame: everything in bin 0) */
        h->max = MAX(mx, 2);
        h->total_px = n;
        mst.ok++;
        mst.numpix = n;
        mst.frame = fr;
    }
    mst.last = s;
    mst.reasons[s]++;
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    mst.us_last = us;
    if (us > mst.us_max) mst.us_max = us;
    return s;
}
#endif /* FEATURE_HISTOGRAM */

static MENU_UPDATE_FUNC(r6h_menu_update)
{
    if (!r6h_mode) return;
    uint32_t s = st.state == R6H_MENU ? st.last_off_menu : st.state;
    if (s != R6H_SHOWN)
        MENU_SET_WARNING(MENU_WARN_INFO, "Not shown: %s.", r6_overlay_reason(s));
    else
        MENU_SET_RINFO("%d draws", (int) st.draws);
}

static struct menu_entry r6h_menu[] = {
    {
        .name    = "Histogram (Canon)",
        .priv    = &r6h_mode,
        .max     = 2,
        .choices = (const char *[]) { "OFF", "Luma", "RGB" },
        .update  = r6h_menu_update,
        .help    = "Canon's own LV histogram bins (RAM reads), box at the top right.",
        .help2   = "LCD shooting screen only (not EVF/HDMI, menus, Q, OLC). Not saved: OFF at boot.",
    },
};

static void r6h_init()
{
    menu_add("Overlay", r6h_menu, COUNT(r6h_menu));
}

INIT_FUNC("r6_overlays", r6h_init);

#endif /* FEATURE_R6_CANON_HISTOGRAM */
