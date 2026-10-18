/* R6.190 M15.0 (spec m15/m150 3.2): the R6 part of src/zebra.c's head - the R6 includes, R6GD_T0 / R6GD_T1 and the
 * prototypes of the static R6 functions in r6_zebra.c that zebra.c calls before r6_zebra.c is included.
 * Included by src/zebra.c only (inside #ifdef CONFIG_R6_GLOBAL_DRAW), not a header of its own module. */
#ifndef _r6_zebra_h_
#define _r6_zebra_h_
#ifndef CONFIG_R6_GLOBAL_DRAW
#error "r6_zebra.h: CONFIG_R6_GLOBAL_DRAW only (included by src/zebra.c)"
#endif


/* R6 M7.4 (platform/R6.190/r6_gdraw.h, internals.h): ML's overlays on
 * Canon's shared layer 0, LV image = Canon's FaceYuv buffer. Every overlay
 * defaults to OFF; Global Draw itself is not saved (OFF at every boot). */
#include "r6_gdraw.h"
#include "refresh_guard.h"      /* idx_changes: Canon repainted layer 0 */
#include "r6_layer.h"           /* resets: layer 0 address changed */
#include "r6_faceyuv.h"         /* M7.7: R6_FY_H / R6_FY_PITCH (waveform row bound) */
#ifdef CONFIG_R6_GRID
#include "r6_grid.h"            /* M9.1: grids & masks */
#ifdef FEATURE_R6_CANON_HISTOGRAM
#include "r6_overlays.h"        /* r6h_mode, R6H_BOX_*: the grid's exclusion rectangle */
#endif
#endif
#ifdef FEATURE_HISTOGRAM
#ifndef FEATURE_R6_CANON_HISTOGRAM
#error "R6 M7.5: ML's histogram needs Canon's bins (r6_overlays.c)"
#endif
#include "r6_overlays.h"        /* M7.5: r6_ml_hist_fill */
#endif
#define R6GD_T0() uint64_t r6gd_t0 = get_us_clock()
#define R6GD_T1(n, us, mx) do { uint32_t _us = (uint32_t) (get_us_clock() - r6gd_t0); \
    r6_gd_st.n++; r6_gd_st.us = _us; if (_us > r6_gd_st.mx) r6_gd_st.mx = _us; } while (0)
/* M15.0: defined in r6_zebra.c (included by zebra.c before lens_display_dirty), called earlier in zebra.c */
#if defined(FEATURE_HISTOGRAM)
static void r6_hist_build(const uint32_t * buf);   /* hist_build */
static void r6_hist_box(void);                     /* draw_histogram_and_waveform */
#endif
#ifdef FEATURE_WAVEFORM
static void r6_wfm_box(void);                      /* draw_histogram_and_waveform */
#endif
#ifdef FEATURE_GLOBAL_DRAW
static MENU_UPDATE_FUNC(r6_gd_menu_info);          /* global_draw_display */
#endif

#endif /* _r6_zebra_h_ */
