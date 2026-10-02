/*
 * R6.190 M7.4: ML's own overlays (FEATURE_GLOBAL_DRAW: zebras, false color,
 * waveform, spotmeter, histogram) on Canon's shared layer 0, see
 * r6_gdraw.c. Used by src/zebra.c, src/falsecolor.c, r6_overlays.c and
 * lvprobe.c, only under CONFIG_R6_GLOBAL_DRAW (platform/R6.190/internals.h).
 */
#ifndef _r6_gdraw_h_
#define _r6_gdraw_h_

#include <stdint.h>

/* ---- layout on the LCD shooting screen (ML coordinates, 720x480 = LCD) ----
 * M5 photo / M7.3: Canon's top row ends at about y 48, the Q icon covers
 * x 631..710 up to y 46, the bottom bar starts at y ~388, left icons x < 95.
 * Zebras and false color only draw in the band R6_GD_Y0..R6_GD_Y1-1 (Canon's
 * top row and bottom bar are never painted over). Inside the band Canon's
 * elements (left icons, AF frame, grid, level) stay on top: the ownership
 * refresh writes ML's pixels only where layer 0 is empty (write-under,
 * r6_layer.c, ML menu closed). The histogram takes the
 * place of Canon's histogram box (r6_overlays.c; the two are mutually
 * exclusive), the small waveform sits above the bottom bar, right of the
 * left icons. */
#define R6_GD_LCD_Y0      50
#define R6_GD_LCD_Y1      386
/* M7.6, EVF: Canon's EVF shows WINSYS origin 1 (720x540 at (152,0) in layer
 * 0); ML's 720x480 area at (152,30) is assumed to be exactly the 1280x854
 * EVF image (centred in the 1280x960 panel), with Canon's EVF info in the
 * 30-row bands above and below it. Zebras / false color / peaking therefore
 * cover the whole 720x480 there (write-under keeps Canon's elements on top).
 * Same coordinates and boxes as on the LCD otherwise. */
#define R6_GD_EVF_Y0      0
#define R6_GD_EVF_Y1      480
extern int r6_gd_evf;               /* r6_gdraw.c: 1 = the last screen check saw the EVF (mask exactly 0x10) */
#define R6_GD_Y0          (r6_gd_evf ? R6_GD_EVF_Y0 : R6_GD_LCD_Y0)
#define R6_GD_Y1          (r6_gd_evf ? R6_GD_EVF_Y1 : R6_GD_LCD_Y1)
#define R6_GD_HIST_X      576       /* multiple of 4 (hist_draw_image aligns), box 576..703 x 54..107 */
#define R6_GD_HIST_Y      54
#define R6_GD_WFM_X       100       /* small waveform 180 x 54: 100..279 x 330..383 */
#define R6_GD_WFM_Y       330
#define R6_GD_WFM_H       54

/* ---- CPU bounds (livev_hiprio_task). A pass requests a refresh only if it
 * changed ML's indexed buffer (zebras, false color: per word; histogram /
 * waveform boxes: hash before / after; spotmeter: skipped while value and
 * box are unchanged); the guard caps ML's own XimrExe at 5 per second in LV,
 * and the conversion cap (bmp.c, r6_gd_conv_hold) keeps the conversions at
 * about 10 % of a core. M7.5: the conversion skips unchanged blocks while
 * Canon is quiet (r6_layer.c), so a small change costs only its blocks and
 * the cap waits correspondingly less; the histogram comes from Canon's bins
 * and the waveform from a 180 x 60 subsample (zebra.c r6_hist_build) ---- */
#define R6_GD_ZEBRA_MS    250       /* zebras recomputed at most 4 x per second (ML default: ~15 x) */
#define R6_GD_FALSE_MS    333       /* M7.5: false color at most 3 x per second (M7.4: 1 x; the lazy conversion,
                                     * r6_layer.c, converts only the blocks that changed; the cap below stays) */
#define R6_GD_PEAK_MS     250       /* M7.6: focus peaking at most 4 x per second (ML default: every 2nd frame);
                                     * ~57600 FaceYuv samples (y step 3, x step 2), 5 uncached reads each */
#define R6_GD_RESHOW_MS   500       /* after Canon repainted layer 0 (VMIX index changed): re-show ML's pixels, at most 2 x per second */
#define R6_GD_CAP_DUTY    10        /* conversion cap: wait 10 x the last conversion time (~10 % of a core) */
#define R6_GD_CAP_MAX_MS  2000      /* ... but at most 2 s */

/* why ML's overlays may not draw on the current screen; R6GD_OK = they may */
enum {
    R6GD_OK = 0,
    R6GD_NO_LV,      /* Canon's PROP_LV_ACTION != 0 (or not seen yet) */
    R6GD_GUI,        /* CURRENT_GUI_MODE != 0 (Canon MENU 0x2003, Q 0xC048, playback, ...) */
    R6GD_GUISTATE,   /* gui_state != GUISTATE_IDLE */
    R6GD_OVL,        /* LV_OVERLAYS_MODE not one of the LV shooting screen INFO variants (6 = settings screen) */
    R6GD_DISP_OFF,   /* DISPLAY_IS_ON == 0 */
    R6GD_NOT_PANEL,  /* display mask neither exactly 0x01 (LCD) nor, M7.6, exactly 0x10 (EVF): HDMI 0x1100, both, pointer invalid */
    R6GD_NO_DIALOG,  /* LiveViewApp dialog 0 */
    R6GD_NO_IMAGE,   /* no verified FaceYuv frame (get_yuv422_vram()->vram == NULL, r6_faceyuv.c) */
    R6GD_MOVIE,      /* movie mode: not reviewed (recording is invisible to ML, PROP_MVR_REC_START denied);
                      * is_movie_mode() (shooting_mode) OR PROP_LV_MOVIE_SELECT == 2 (second, independent indicator) */
    R6GD_NREASONS
};

/* Canon screen state check, RAM reads only (r6_gdraw.c). Called by
 * get_global_draw() on every call. */
int r6_gd_screen_ok(void);
const char * r6_gd_reason(int r);

/* conversion cap (bmp.c refresh_yuv_from_rgb): 1 = skip this refresh (the
 * flag stays set), while the ML menu is closed and the last conversion was
 * less than R6_GD_CAP_DUTY x its own duration ago (at most R6_GD_CAP_MAX_MS);
 * r6_gd_conv_done after every conversion */
int r6_gd_conv_hold(void);
void r6_gd_conv_done(uint32_t us);

/* FNV-1a hash of ML's indexed buffer over a box (ML coordinates, clipped);
 * 0 if the buffer is not there. Change detection for the overlay boxes. */
uint32_t r6_gd_bv_hash(int x, int y, int w, int h);
/* the same over ML memory (histogram bins, waveform data) */
uint32_t r6_gd_bv_hash_mem(const void * p, uint32_t n);

/* counters and timings (zebra.c / falsecolor.c write them, lvprobe.c logs them) */
struct r6_gd_stats
{
    uint32_t checks;                 /* r6_gd_screen_ok calls */
    uint32_t reasons[R6GD_NREASONS];
    uint32_t last, last_outside_menu;   /* R6GD_* of the last check, last check with the ML menu closed */
    uint32_t mask;                   /* last display mask read (0xFFFFFFFF = pointer invalid) */
    uint32_t ovl;                    /* last LV_OVERLAYS_MODE */
    /* passes (count, last and max duration in us, get_us_clock) */
    uint32_t zebra_n, zebra_changed, zebra_us, zebra_us_max, zebra_nolv;
    uint32_t fc_n, fc_us, fc_us_max, fc_nolv;
    uint32_t hist_n, hist_us, hist_us_max, hist_nolv;   /* hist_build (+ waveform accumulation) */
    /* M7.5: hist_build parts (zebra.c r6_hist_build) */
    uint32_t hcanon_n, hcanon_us, hcanon_us_max;        /* ML histogram from Canon's bins (r6_ml_hist_fill) */
    uint32_t hcanon_ok, hcanon_fail;
    uint32_t hcanon_state;           /* R6H_* of the last fill (r6_overlays.h) */
    uint32_t hist_valid;             /* 1 = histogram holds Canon's current bins (else nothing is shown) */
    uint32_t hist_none;              /* box passes with nothing to show (box erased once) */
    uint32_t hist_hold;              /* fix round: failed fills that kept the last good histogram (<= 1 s old) */
    uint32_t wfm_n, wfm_us, wfm_us_max, wfm_samples;    /* subsampled waveform accumulation */
    uint32_t hdraw_n, wdraw_n, draw_us, draw_us_max;    /* histogram / waveform box drawing */
    uint32_t spot_n, spot_us, spot_us_max;
    uint32_t erase_n;                /* ML overlay pixels erased when the screen state ended (r6_gd_erase) */
    uint32_t refresh_req;            /* refreshes requested by the overlays */
    uint32_t refresh_same;           /* passes that changed nothing in ML's buffer: no refresh requested */
    uint32_t spot_same;              /* spotmeter passes skipped: value, position and box unchanged */
    int32_t  spot_y, spot_u, spot_v; /* last spotmeter average (Y 0..255, U / V signed) */
    uint32_t spot_nu, spot_nv;       /* U / V samples in it */
    uint32_t cap_hold;               /* refreshes held back by the conversion cap */
    uint32_t cap_ms;                 /* last cap interval in ms */
    uint32_t resets_seen;            /* r6_layer resets handled by the re-show */
    uint32_t movie_sm, movie_lvms;   /* movie mode blocks by is_movie_mode() / by PROP_LV_MOVIE_SELECT */
    int32_t  shooting_mode, lv_movie_select;   /* last values seen by the gate */
    uint32_t reshow;                 /* of these: after Canon repainted layer 0 (VMIX index change, refresh_guard.c) */
    uint32_t drawn;                  /* bit 0 zebra/false color/peaking (mirror), 1 histogram, 2 waveform, 3 spotmeter */
    /* M7.6 */
    uint32_t evf_ok;                 /* screen checks passed on the EVF */
    uint32_t evf_switch;             /* LCD <-> EVF band changes seen by the overlay task (overlays erased once) */
    uint32_t peak_n, peak_us, peak_us_max, peak_nolv;   /* focus peaking passes (zebra.c draw_zebra_and_focus) */
    uint32_t peak_dots, peak_over, peak_thr;            /* last pass: dots drawn, samples over the threshold, threshold */
    uint32_t peak_changed;           /* passes whose dots differed from the previous pass (refresh requested) */
    uint32_t peak_full;              /* passes that hit MAX_DIRTY_PIXELS (threshold too low, rows cut short) */
    uint32_t peak_off;               /* dots erased because peaking was switched off */
    uint32_t redraw_n;               /* _redraw_do (MLEV_REDRAW) handled as ML-only clrscr instead of dialog_redraw */
};
extern struct r6_gd_stats r6_gd_st;

#define R6GD_DRAWN_MIRROR 1
#define R6GD_DRAWN_HIST   2
#define R6GD_DRAWN_WFM    4
#define R6GD_DRAWN_SPOT   8

#endif
