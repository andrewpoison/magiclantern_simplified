/*
 * R6.190 M9.3: motion trigger "R6 Motion" (r6_motion.c, ML menu Shoot > R6 Motion).
 * Only with CONFIG_R6_MOTION (platform/R6.190/internals.h). The detector runs
 * inside r6_interval.c's task; r6_interval.c is the only presser. Used by
 * r6_interval.c (task, menu), r6_grid.c (display) and lvprobe.c (log md.*).
 */
#ifndef _r6_motion_h_
#define _r6_motion_h_

#include <stdint.h>

#define R6MD_W 32                /* blocks per row (20 px each) */
#define R6MD_H 24                /* block rows (17 rows each, FaceYuv rows 9..416) */
#define R6MD_N (R6MD_W * R6MD_H)
#define R6MD_TICK_MS 20          /* r6_iv_task period while a motion run is active (r6_interval.c r6_iv_tick_ms) */

enum { R6MD_OFF, R6MD_DELAY, R6MD_SETTLE, R6MD_BASE, R6MD_ARMED, R6MD_PIC, R6MD_WAIT, R6MD_NSTATES };
/* gates, in the order they are checked (r6_motion.c); the first failing one is the reason */
enum { R6MD_G_OK, R6MD_G_LV, R6MD_G_MENU, R6MD_G_GUI, R6MD_G_MAGNIFY, R6MD_G_DISPLAY, R6MD_G_SETTINGS, R6MD_G_FY,
       R6MD_G_STALE, R6MD_NGATES };
/* r6_fy_locate_done refusals (md.nr[]) */
enum { R6MD_NR_B048, R6MD_NR_RINGF, R6MD_NR_RECNEW, R6MD_NR_RECDONE, R6MD_NR_SAMEBUF, R6MD_NR_HANDOUT,
       R6MD_NR_SIZE, R6MD_NR_OTHER, R6MD_NNR };
/* why a background was measured (md.rebase[]) */
enum { R6MD_RB_START, R6MD_RB_GATE, R6MD_RB_GLOBAL, R6MD_RB_PIC, R6MD_NRB };
/* r6_md_after result: VIEW (display only), SHOT (iv_shoot ran), DROP + enum r6_iv_canon (Canon not ready, no picture) */
enum { R6MD_R_VIEW = 0, R6MD_R_SHOT = 1, R6MD_R_DROP = 2 };
#define R6MD_TRIGGER 1

/* the task's view of R6 Interval's gates (r6_interval.c iv_tick_md) */
struct r6_md_in { int now; int lv_ok; int menu_ok; };

/* r6_iv_task only (r6_interval.c) */
int r6_md_begin(int now, int shoot);           /* 0 ok, -1 no memory for the detector */
void r6_md_end(void);                          /* frees the buffers, state OFF (idempotent); stats kept */
int r6_md_step(const struct r6_md_in * in);    /* R6MD_TRIGGER: a confirmed trigger now (state PIC) */
void r6_md_after(int now, int result);         /* the trigger's outcome -> WAIT */
int r6_md_active(void);                        /* state != OFF */
/* settings (r6_interval.c reads them at Start) */
int r6_md_shoot_setting(void);                 /* 1 = Mode "Release" */
int r6_md_count(void);                         /* 1..50 */
int r6_md_maxdur_ms(void);                     /* 0 = no limit */

/* display (r6_grid.c, overlay task): rectangles in ML coordinates, half-open */
struct r6_md_rect { int16_t x0, y0, x1, y1; uint8_t col; uint8_t pad[3]; };
#define R6MD_NRECT 8
int r6_md_disp_wanted(void);                   /* Display ON and a run active */
int r6_md_shape(struct r6_md_rect * r, int max, int evf);   /* number of rects; 0 = nothing (also while a picture runs) */

struct r6_md_last { int32_t t; uint32_t f2; int16_t state, score, hits, run, result; int16_t pad; };
#define R6MD_RING 16
/* counters and the last values; the int32 fields up to r6_md_stats.nr_last are logged by name (r6_md_knames) */
struct r6_md_stats
{
    int32_t state, release, gate_last, t_state, t_start;
    int32_t cfg, count, wait_s, delay_s, maxdur_s;
    int32_t ax0, ay0, ax1, ay1, minblk;
    int32_t frames, analysed, fps_x10, ana_x10, fstep, retry;
    int32_t us_last, us_avg, us_max, torn, torn_w, stale;
    int32_t global_light, global_move, unstable, base_ms_last;
    int32_t triggers, shots, dropped, dropped_mask;
    int32_t score_last, score_max, hits_all_last, hits_all_max, val, thr, run;
    int32_t noise_avg_q4, gain_all_q8, gain_min_q8, gain_max_q8;
    int32_t lat_conf_ms_last, lat_sw1_ms_last, frame_age_ms_last;
    int32_t stop, n_done, n_target;            /* of the last run (copied from R6 Interval at the end) */
    int32_t ymax, ymax_last;                   /* review F1: largest sampled luma since boot / of the last analysed frame */
    const char * nr_last;                      /* r6_fy_locate_done reason of the last refusal */
    uint32_t nr[R6MD_NNR];
    uint32_t gate[R6MD_NGATES];
    uint32_t rebase[R6MD_NRB];
    uint32_t nlast;                            /* ring entries written since boot */
    struct r6_md_last last[R6MD_RING];
};
const struct r6_md_stats * r6_md_stats(void);
const char * r6_md_knames(int * n);            /* the int32 fields' names, NUL-separated, in struct order */
const char * r6_md_state_name(int s);
const char * r6_md_gate_name(int g);
void r6_md_status(char * buf, int n);          /* menu status line */

#endif
