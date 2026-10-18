/*
 * R6.190 M14.1: the astro clock (r6_aclock.c, CONFIG_R6_ACLOCK; spec m14/m141 spec_m141.md Rev. 2, 2.3 / 2.4 / 4.3 / 5.3):
 * Display > "Astro clock" (tonight, now, polar scope, location, time zone, Milky Way criteria) and one optional LV line in
 * the place of ETTR's line (ETTR first; drawn by r6_grid.c). Computed with r6_acore.c from r6_tlog_task's RTC pair and
 * Canon's time-zone struct (Tor 0g, read only); since M15.0 (E-b) in r6_tlog_task itself (r6_ac_step after each poll), no
 * task of its own. No Canon access in the menu or the drawing path.
 */
#ifndef _r6_aclock_h_
#define _r6_aclock_h_

#include <stdint.h>

#define R6AC_TASK_MS       1000     /* the clock's tick */
#define R6AC_TICK_SLACK_MS 100      /* M15.0: a step runs from R6AC_TASK_MS minus this after the last one (r6_tlog_task's poll pace) */
#define R6AC_NOW_MS        10000    /* the "now" values */
#define R6AC_DEBOUNCE_MS   1000     /* a new key must stay this long before the night is computed */
#define R6AC_TZ_MS         10000    /* the time-zone struct is read this often */
#define R6AC_YIELD_MS      10       /* the sleep between two chunks of the computation */
#define R6AC_PAIR_MAX_MS   180000   /* an RTC pair older than this is no clock */
#define R6AC_ZONE_KEEP_MS  30000    /* one failed zone read keeps the last valid zone at most this long */
#define R6AC_Y_MIN         2020
#define R6AC_Y_MAX         2059

/* the clock's state */
enum { R6AC_WAIT, R6AC_OK, R6AC_COMPUTING, R6AC_PENDING, R6AC_CLOCK_BAD, R6AC_RANGE, R6AC_TZ_BAD };

/* M15.0 (E-b): one step of the clock (zone, clock, night, now values, publish); only from r6_tlog_task after its poll, at most
 * one step per R6AC_TASK_MS - R6AC_TICK_SLACK_MS */
void r6_ac_step(void);

/* r6_grid.c (overlay task): 1 = the line is wanted (settings only) */
int r6_ac_lv_wanted(void);
/* r6_grid.c: the line's text from the published result (no computation); short_form 1 = the short one; 0 = no line
 * (Show in LV OFF, ETTR's line wanted, ML's picture runs, no result yet) */
int r6_ac_lv_text(char * buf, int n, int short_form);
/* r6_grid.c: count a draw (1) / erase (0) of the clock's line */
uint32_t r6_ac_lv_count(int draw);

struct r6tl_out;
/* r6tl_ast (snapshot): the ac.* lines, from copies */
void r6_ac_ast(struct r6tl_out * o);

#endif
