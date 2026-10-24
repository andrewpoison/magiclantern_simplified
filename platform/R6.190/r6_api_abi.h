/*
 * R6.190 M16.1 (spec m16/m161 spec_m161.md Rev. 2a 2.1; plan docs/m16_plan.md E5..E8): the interface between the core facade
 * r6_api.c (CONFIG_R6_SCRIPT) and lua.mo (modules/lua, CONFIG_R6_LUA). Only <stdint.h>; lua.mo includes this file by its relative
 * path ("../../platform/R6.190/r6_api_abi.h"). Every function below is exported (R6_190.sym); nothing else of the facade is
 * (its internal names start with "_", platform/Makefile keeps those out of magiclantern.sym, A10).
 */
#ifndef _r6_api_abi_h_
#define _r6_api_abi_h_

#include <stdint.h>

#define R6API_VER 2

enum { R6SC_NONE, R6SC_IV_START, R6SC_IV_STOP, R6SC_IV_TEST, R6SC_TVW, R6SC_TVBACK, R6SC_MD_START, R6SC_MD_STOP,
       R6SC_ETTR_APPLY, R6SC_AST_MODE, R6SC_AST_TEST, R6SC_SELECT, R6SC_N };          /* r6_script_cmd */
enum { R6SG_MODE, R6SG_PLAY, R6SG_MENU, R6SG_LV_START, R6SG_LV_STOP, R6SG_ZOOM, R6SG_KEY, R6SG_REC, R6SG_N };  /* r6_script_gui */
enum { R6TC_SYNC, R6TC_ASYNC, R6TC_REFUSE };                                     /* r6_script_task_class */

#define R6API_OK         0
#define R6API_CAM        1000     /* + enum r6_iv_cam: refused before the picture */
#define R6API_QUEUED     2000     /* accepted from a GUI task, runs asynchronously (r6_last_shot) */
#define R6API_TASK      (-1)      /* wrong task (PropMgr, r6_iv_task) */
#define R6API_TIMEOUT   (-2)      /* cancelled at the caller's time limit, before the press / write (nothing happened) */
#define R6API_BUSY      (-3)      /* another job is open, a long lock (Interval, Tv back, ETTR, log snapshot, pending restore),
                                     ML picture / Tv write / ETTR run (2.6), or shutting down (SL-10); why says which */
#define R6API_GUITASK   (-4)      /* a waiting command from GuiMainTask / menu_task / menu_redraw_task */
#define R6API_CANCELLED (-5)      /* cancelled before the press / write (script or task end, Interval Start, async expiry) */
#define R6API_EARLY     (-6)      /* Tor-2 command before the start check (5.4) */
#define R6API_UNRES     (-7)      /* R6 menu entry not resolved (fail-closed, 5.2) */
#define R6API_LATE      (-8)      /* the caller's backstop after the press / write hit; the job ends on its own, result in
                                     r6.last_shot / scr.shot / scr.tv (SL-4; never expected) */
#define R6API_NOSET     (-9)      /* r6_script_menu_set: an R6 entry that scripts may not set ("not settable from scripts") */
#define R6API_ARG       (-10)     /* a bad argument (unknown command, key or value); why says which */
#define R6API_STACK     (-11)     /* GuiMainTask with less than 4,096 B of stack left: "stack: use task.create" (SB-11) */
#define R6API_PASS      (-0x7FFFFFFF)  /* r6_script_menu_set: not an R6 entry, take the upstream path (MB-5); != INT_MIN */

/* r6_script_tv: 0 = set and kept / unchanged; else R6API_TV_LOCK | enum r6pw_lock, R6API_TV_RES | R6PW_R_*, or a negative R6API_* */
#define R6API_TV_LOCK    0x100
#define R6API_TV_RES     0x200
#define R6API_TV_R_TOOK_OVER 14   /* = R6PW_R_TOOK_OVER (r6_propw.h): "Tv changed by user (your value stays)"; r6_api.c checks it */
#define R6API_TV_L_OFF   1        /* = R6PW_L_OFF: "Tv writes" is OFF */

struct r6_expo   { int16_t tv, av, sv, comp; uint8_t valid; };                   /* Code768; valid bit 0..3 */
struct r6_shot_res { uint32_t seq, gen, t_req, t_done; int32_t res; uint8_t async, cancelled; char why[48]; };   /* seqlock, SL-13 */
struct r6_sysmem { int32_t ok, largest, free, used, peak; char why[24]; };       /* ok 0: unknown, why says why (C2) */

int r6_take_a_pic_ex(uint32_t gen, int should_af, int timeout_ms, uint32_t * seq, char * why, int n);   /* seq: the job's number (SL-13) */
int r6_script_burst(uint32_t gen, int n_pics, int * done, char * why, int n);
int r6_script_bulb(uint32_t gen, int seconds, char * why, int n);
int r6_script_tv(uint32_t gen, int code768, char * why, int n);   /* 0 = set and kept / unchanged; else 0x100 | lock, 0x200 | result */
int r6_script_expo(struct r6_expo * e);
int r6_script_wait_jobs(int timeout_ms);
int r6_script_cmd(uint32_t gen, int cmd, int arg, char * why, int n);
int r6_script_menu_set(uint32_t gen, const char * menu, const char * entry, const char * str, int ival, char * why, int n);   /* MB-5 */
int r6_script_gui(uint32_t gen, int op, int arg, int timeout_ms, char * why, int n);
int r6_script_task_class(const char * task_name);
int r6_script_sysmem(struct r6_sysmem * m);
int r6_script_prop_state(uint32_t id);            /* 2 denied (K7 list), 3 the core's handler tables full (R2-09), 1 registered at
                                                     start, 0 not registered */
int r6_script_temp(int * deci_c);                 /* 1 = sensor A valid */
int r6_script_dst(void);                          /* 1 / 0 / -1 unknown */
void r6_script_end(uint32_t gen, int sent_rec, int set_tvw);   /* R-4 / A18: only at the real end of the Lua state (SL-2) */
void r6_script_task_end(uint32_t gen);            /* SL-2: a Lua task ends (normally or via the Ue3 anchor): its own open jobs */
int r6_last_shot(struct r6_shot_res * r);         /* copy under the seqlock (SL-13); 0 = none yet, -1 busy */
int r6_lv_running(void);

/* 2.4 (PC test G16): Code768 (1 EV = 768, thirds 256, halves 384) <-> ML's raw values (1 EV = 8, thirds 3 / 5, halves 4).
 * Bases: Tv 56, ISO 72 (c = Sv768 - 3840), Av 8, exposure compensation 0 (signed). floor division in both directions */
static inline int r6_floordiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static inline int r6_c768_to_raw(int c, int base)
{
    int k = r6_floordiv(c, 768), r = c - 768 * k;
    int f = r == 0 ? 0 : r == 256 ? 3 : r == 384 ? 4 : r == 512 ? 5 : (r * 8 + 384) / 768;   /* else the nearest eighth */
    return base + 8 * k + f;
}
/* 0 = ok (*c), 1 = a half step ("1/2 step"), 2 = not on the 1/3 grid */
static inline int r6_raw_to_c768(int raw, int base, int * c)
{
    int d = raw - base, k = r6_floordiv(d, 8), f = d - 8 * k;
    if (f == 0 || f == 3 || f == 5) { *c = 768 * k + (f == 0 ? 0 : f == 3 ? 256 : 512); return 0; }
    return f == 4 ? 1 : 2;
}

#endif
