/*
 * R6.190 M14.1: the camera temperature (r6_temp.c, CONFIG_R6_TEMP; spec m14/m141 spec_m141.md Rev. 2, 2.2 / 4.2 / 5.2):
 * Display > "Temperature" (sensor A as in EXIF, Canon's 8 sensors in 0.1 degC, Canon's status) and a small box on the LCD
 * and (M15.0) EVF shooting screen (drawn by r6_grid.c). Everything from r6_tlog_task's newest poll (Tor 0g, read only). No
 * Canon access here.
 */
#ifndef _r6_temp_h_
#define _r6_temp_h_

#include <stdint.h>

#define R6TB_X1        704      /* the box: right edge (exclusive), under Canon's / ML's histogram (LCD and EVF, M15.0) */
#define R6TB_Y0        122      /* its top row */
#define R6TB_STALE_MS  5000     /* a poll older than this is no value */

/* r6_tlog_task, once per poll (its only caller): the warning state from sensor A (t16, read ok) and Canon's status; ML's
 * own arithmetic on temp.warn; *hyst = the threshold state (0.5 degC hysteresis); returns the warning (threshold or status) */
int r6_temp_poll(int t16, int ok, uint32_t st, int st_ok, int * hyst);

/* r6_grid.c (overlay task): 1 = the box is wanted (settings only) */
int r6_temp_lv_wanted(void);
/* r6_grid.c: the box text ("A 37" SYM_DEGREE "C", ...) and its warning colour; 0 = no box (Show in LV OFF, a picture runs) */
int r6_temp_lv_text(char * buf, int n, int * warn);
/* r6_grid.c: count a draw (1) / erase (0) of the box */
uint32_t r6_temp_lv_count(int draw);

/* the whole degrees of t16 as Canon rounds them for EXIF: floor((t16 + 8) / 16) */
int r6_temp_deg(int t16);

struct r6tl_out;
/* r6tl_ast (snapshot): the tb.* lines, from copies */
void r6_temp_ast(struct r6tl_out * o);

#endif
