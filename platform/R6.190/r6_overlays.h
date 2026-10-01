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

#endif
