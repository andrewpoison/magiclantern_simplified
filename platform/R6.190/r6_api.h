/*
 * R6.190 M16.1 (spec m16/m161 spec_m161.md Rev. 2a 2, 5, 6, 7.1): the core part of the script facade (r6_api.c, CONFIG_R6_SCRIPT).
 * The module-visible part is r6_api_abi.h. Every name here starts with "_" (not exported, A10) or is a constant.
 * r6_interval.c (CONFIG_R6_IV_SCRIPT) includes it for the result codes and the shared time limits.
 */
#ifndef _r6_api_h_
#define _r6_api_h_

#include "r6_api_abi.h"

/* time limits (spec 2.5, 3.2.4, 4.1, 5.4, 6; qcheck.sh pins the camera values, QEMU turns some of them into variables) */
#define R6API_ASYNC_MAX_MS   30000   /* an asynchronous picture (from a GUI task) expires this long after it was posted (SL-1) */
#define R6API_GUI_SETTLE_MS  500     /* after an E8 mode switch: no ML press / arm for this long (intent mark, SL-6) */
#define R6API_TV_MS          15000   /* a Tv assignment: limit until ACTIVE (from t_req), and the backstop after it (SL-4) */
#define R6API_GUI_MS         3000    /* a GUI job: limit until RUNNING, the backstop after it, the wait for "not busy" (MB-9) */
#define R6API_E8_MS          5000    /* an E8 mode switch: the wait for "not busy" (SL-11); lv.start / lv.stop: live view within */
#define R6API_T2_BOOT_MS     20000   /* Tor 2 commands from scripts / FAKE keys only after this (start check, 5.4, MB-2) */
#define R6API_STACK_MIN      4096    /* GuiMainTask runs an R6 command directly only with this much stack left (SB-11) */
#define R6API_POLL_MS        20      /* the waits of the facade */

/* the origin mark of the GUI event being handled (src/gui-common.c, spec 2.3); GuiMainTask only */
enum { R6GO_NONE = 0, R6GO_KEY, R6GO_FAKE, R6GO_SCRIPT };
#define MLEV_R6_SCRIPT (-40)         /* ML event: run the posted GUI job (obj = its seq); never reaches Canon (src/gui.c) */

struct menu_entry;
struct event;
/* src/gui-common.c (CONFIG_R6_SCRIPT) */
int  _r6_gui_org(void);                          /* the mark of the event being handled (R6GO_*) */
int  _r6_gui_org_set(int org);                   /* sets it, returns the previous one (GuiMainTask only) */
/* src/menu.c (CONFIG_R6_SCRIPT) */
struct menu_entry * _r6_menu_entry(const char * menu, const char * entry);   /* entry_find_by_name (menu_sem not needed) */
uint32_t _r6_menu_sel_seq(void);                 /* select_menu_by_name / menu_open calls (MB-3) */
/* src/property.c (CONFIG_R6_SCRIPT) */
int  _r6_prop_state(uint32_t id);                /* 2 denied, 3 tables full (R2-09), 1 registered at start, 0 not registered */
/* r6_aclock.c (CONFIG_R6_SCRIPT) */
int  _r6_ac_dst(void);                           /* 1 / 0 daylight saving time by Canon's zone, -1 unknown */
/* r6_api.c: the GUI event hook (gui-common.c), the menu fall-back guard (menu.c), the seconds hook (shoot.c) */
int  _r6api_gui_event(struct event * event);     /* MLEV_R6_SCRIPT: runs the posted GUI job; 1 = it was one (consumed) */
int  _r6api_menu_r6(struct menu_entry * e);      /* 1 = e lies in one of the nine R6 menus (menu.set fall-back guard, 5.2) */
void _r6api_second(void);                        /* seconds_clock_task: CBR_INTERVALOMETER / CBR_POST_SHOOT (7.1) */
void _r6api_org_note(int what);                  /* r6_interval.c iv_menu_caller: an R6 action in GuiMainTask (scr.org) */
uint32_t _r6api_ast_ref(void);                   /* Astro hulls: refusals (scr.job ast_ref) */
/* r6_api.c for lvprobe.c (LVP lines, spec 11) */
struct r6api_org { uint32_t t; uint8_t task, org, cmd, pad; int32_t arg, res; };
int  _r6api_org_ring(int i, struct r6api_org * e);   /* i = 0 oldest; returns the count (<= 32) */
void _r6api_counts(uint32_t c[8]);               /* scr.cnt: menu, script, fake, key, refused; late, gui_jobs, tvw_end */
int  _r6api_tvw_end(void);                       /* "Tv writes" at the last script end (-1 none) */
void _r6api_tids(uint32_t t[3]);                 /* task ids of GuiMainTask, clock_task, r6_iv_task as seen by the hooks (0 = not yet) */

#endif
