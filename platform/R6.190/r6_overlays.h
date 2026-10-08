/*
 * R6.190 M7.3: first ML LiveView overlay, "Histogram (Canon)" (r6_overlays.c).
 * Only with FEATURE_R6_CANON_HISTOGRAM (platform/R6.190/features.h).
 */
#ifndef _r6_overlays_h_
#define _r6_overlays_h_

#include <stdint.h>

/* why the box is not shown (state); R6H_SHOWN = drawn */
enum {
    R6H_SHOWN = 0,
    R6H_OFF,         /* menu setting OFF */
    R6H_NO_LV,       /* neither lv nor Canon's PROP_LV_ACTION == 0 */
    R6H_MENU,        /* ML menu open: nothing touched (the menu owns the screen) */
    R6H_GUI,         /* CURRENT_GUI_MODE != 0 (Canon MENU, Q, playback, ...) */
    R6H_OLC,         /* LV_OVERLAYS_MODE == 6 (Canon's settings screen) */
    R6H_DISP_OFF,    /* DISPLAY_IS_ON == 0 */
    R6H_NOT_PANEL,   /* display mask not LCD-only (EVF / HDMI / unknown) */
    R6H_NO_DIALOG,   /* LiveViewApp dialog 0 (no LV shooting screen) */
    R6H_NO_SET,      /* histogram context unreadable or published pointers not DISP set 0/1 */
    R6H_BAD_PTR,     /* a bin array pointer failed the range check */
    R6H_SUM,         /* an array's bin sum != numPix */
    R6H_NO_FRAME,    /* RandomData FrameNum not readable */
    R6H_STALE,       /* FrameNum unchanged for 1 s */
    R6H_MLHIST,      /* M7.4: ML's own histogram (Global Draw) is enabled: same box position */
    R6H_TORN,        /* M7.5 (ML histogram only): the published set changed during every copy attempt */
    R6H_ML_BULB,     /* M9.2: R6 Interval's bulb picture runs (r6_iv_bulb_running): no live image */
    R6H_NREASONS
};

struct r6_overlay_stats
{
    uint32_t mode;           /* 0 OFF, 1 Luma, 2 RGB */
    uint32_t state;          /* R6H_* */
    uint32_t last_off_menu;  /* last state before the ML menu was opened (menu display) */
    uint32_t box;            /* 1 = ML's box is (or may be) in the indexed buffer */
    uint32_t ticks, draws, erases, torn, same_frame, rate;
    uint32_t same_bins;      /* new frame, but the 128 bar heights / clip marks equal the box: no redraw, no refresh */
    uint32_t forced;         /* redraws forced (after the ML menu, box frame no longer intact) */
    uint32_t reasons[R6H_NREASONS];   /* ticks per state */
    int      set;            /* DISP set of the last read (0/1), -1 none */
    uint32_t numpix, frame, peak;     /* last drawn: numPix, FrameNum, max of the 128 bins */
    uint32_t clip[4];        /* bin 255 of the drawn arrays (Y, R, G, B) */
    int      t_draw;         /* ms */
    uint32_t draw_us_last, draw_us_max;   /* read + draw */
};

const struct r6_overlay_stats * r6_overlay_stats(void);
const char * r6_overlay_reason(uint32_t state);

/* M9.1: the setting (0 OFF, 1 Luma, 2 RGB) and the box with its frame in ML
 * coordinates (x1 / y1 exclusive; r6_overlays.c checks them against R6H_*),
 * for the grid's exclusion rectangle (src/zebra.c r6_grid_tick) */
extern int r6h_mode;
#define R6H_BOX_X0  572
#define R6H_BOX_Y0  50
#define R6H_BOX_X1  704
#define R6H_BOX_Y1  118

/* M7.5: ML's own histogram (src/zebra.c hist_build, FEATURE_HISTOGRAM) from
 * Canon's LV histogram bins instead of a pixel loop over FaceYuv. Same reads
 * and checks as the "Histogram (Canon)" box (DISP set 0/1, every used array
 * in range and summing to numPix, set unchanged after the copy, RandomData
 * FrameNum readable and not frozen for 1 s). On R6H_SHOWN h is cleared and
 * filled like hist_build's pixel loop: 128 bins = Canon's bin pairs (ML:
 * Y * 128 >> 8), luma always, R/G/B if rgb (is_rgb = rgb), total_px =
 * numPix, max = highest luma bin except bin 0 (hist_add_pixel ignores bin
 * 0), at least 2 (log scale). Torn reads and "no DISP set" are retried (at
 * most 3 reads). Any other result: a reason, h untouched (the caller keeps
 * or clears it). Caller: one task (livev_loprio_task). RAM reads only. */
struct Histogram;
int r6_ml_hist_fill(struct Histogram * h, int rgb);

struct r6_mlhist_stats
{
    uint32_t calls, ok, torn;        /* torn: copy attempts that saw the set change (retried, at most 3) */
    uint32_t noset;                  /* fix round: reads without a DISP set (retried as well) */
    uint32_t reasons[R6H_NREASONS];  /* results per state */
    uint32_t last;                   /* R6H_* of the last call */
    int      set;                    /* DISP set of the last read, -1 none */
    uint32_t numpix, frame;          /* last good read */
    uint32_t us_last, us_max;        /* read + fill */
};
const struct r6_mlhist_stats * r6_mlhist_stats(void);

#ifdef CONFIG_R6_ETTR
/* M11.0 (R6 ETTR, r6_ettr.c; docs/m11_plan.md 2.1): the reads and checks of the "Histogram (Canon)" box (DISP set 0/1,
 * every array in range and summing to numPix, set / pointers / numPix unchanged afterwards, the RandomData record
 * readable) for all four arrays Y, R, G, B, plus the binding of plan 2.1: the newest RandomData record and its histogram
 * pointer rd[0x5EC] before the first and after the last array, and the record's fields read after the arrays. The arrays
 * are not copied here: visit(arg, c, a, n) reads Canon's array c (256 u32 at a, whole 1 KB range-checked, uncached heap)
 * and returns its sum (the caller copies or computes from it; its results count only if this returns R6H_SHOWN).
 * Returns R6H_SHOWN, R6H_NO_SET / R6H_BAD_PTR / R6H_SUM / R6H_NO_FRAME, or -1 (torn: the set changed during the read).
 * RAM reads only, any task. */
struct r6h_canon {
    int set;                 /* DISP set 0/1, -1 none */
    uint32_t numpix;
    uint32_t p0;             /* the set's Y array (what rd[0x5EC] points to when the record belongs to it) */
    uint32_t rd_b, hp_b;     /* newest RandomData record before the arrays and its rd[0x5EC] (0 = not readable) */
    uint32_t rd_a, hp_a;     /* the same after the arrays */
    uint32_t frame, sim, ae600, m608, fr52c, tv424;   /* rd_a: FrameNum, [0x5FC], [0x600], [0x608], [0x52C], [0x424] */
};
typedef uint32_t (*r6h_visit_fn)(void * arg, int c, const volatile uint32_t * a, uint32_t n);
int r6h_read_canon(r6h_visit_fn visit, void * arg, struct r6h_canon * o);
/* M11.1 (spec m11/m111 1.4, X6): the published DISP set (0 / 1) as r6h_read_canon finds it, without the bins; -1 = none.
 * RAM reads only, any task (R6 ETTR watches it every tick for the LV idle rule) */
int r6h_canon_set(void);
#endif

#endif
