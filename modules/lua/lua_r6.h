/*
 * R6.190 M16.0 (spec m16/m160 3.5, 4, 5; CONFIG_R6_LUA): lua.mo's part of the Lua / module groundwork (lua_r6.c).
 * The core interface (task model, limits, fault hook, counters, atomics) is platform/R6.190/r6_modsup_abi.h.
 * M16.1 (spec m16/m161 2, 5.6, 8.2, 8.3): the camera control goes only through the core facade r6_api_abi.h (pictures and Tv as
 * jobs of r6_iv_task, R6 menu commands in GuiMainTask, E8 in the caller's task); every wait gives the script's semaphore back.
 */
#ifndef _lua_r6_h_
#define _lua_r6_h_

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include "lua/lua.h"
#include "../../platform/R6.190/r6_modsup_abi.h"
#include "../../platform/R6.190/r6_api_abi.h"

struct msg_queue;

/* ---- 3.5: the five task sites, through the core wrapper (R6L_STACK_* by kind; kind R6LK_TASK: stack as given) ---- */
uint32_t lua_r6_start(int kind, const char * name, int prio, int stack, void * fn, void * arg, char * why, int n);
int  lua_r6_cfg(int key);               /* the core table, cached at lua_init and at each lua_r6_start */
void lua_r6_init(void);                 /* lua_init: cache, counters */

/* ---- 4.5: the property ring (no malloc, no cli, several producers) ---- */
#define LUA_R6_PRING   16               /* slots = queue capacity */
#define LUA_R6_PDATA   256              /* bytes per value (at most 255 used, NUL terminated) */
struct lua_r6_pmsg { uint32_t id, len; uint8_t data[LUA_R6_PDATA]; };
void lua_r6_prop_post(struct msg_queue * q, unsigned id, const void * addr, unsigned len);  /* producer (PropMgr) */
int  lua_r6_prop_take(int idx, struct lua_r6_pmsg * m);    /* consumer: 1 = *m filled and the slot free again */

/* ---- 4.6: abort() ends the load (anchor in load_script) ---- */
struct lua_r6_anchor
{
    jmp_buf jb;
    struct lua_r6_anchor * next;
    uint32_t tid;                       /* get_current_task_id() of the task that runs load_script */
    void * script;                      /* struct lua_script * (lua.c) */
    const char * fname;                 /* the script's file name, for the abort line */
    lua_State * L;                      /* from luaL_newstate on */
    int sem;                            /* 1 = this task holds the script's semaphore (first run, until its give) */
};
void lua_r6_anchor_push(struct lua_r6_anchor * a, void * script, const char * fname);
void lua_r6_anchor_pop(struct lua_r6_anchor * a);
void lua_r6_anchor_L(lua_State * L);    /* load_lua_state: the new state of this task's anchor */
void lua_r6_anchor_sem(int held);       /* load_script: this task holds / gave the script's semaphore */
void lua_r6_abort(void) __attribute__((noreturn));   /* abort(): longjmp to the anchor, else sleep forever */
void lua_r6_park(const char * why) __attribute__((noreturn));
int  lua_r6_once(lua_State * L, int bit);   /* lua.c: 1 the first time for the script of L (hints once per script) */
int  lua_r6_alive(lua_State * L);       /* lua.c (review R2-01 / R2-04): 1 a living script has L, 0 a dead one, -1 none (no ASSERT) */
void lua_r6_prop_init(void);            /* lua_property.c (review R2-02): the property queue, once, from lua_init */

/* ---- 4.7: the stack guard ---- */
void lua_r6_stack_check(lua_State * L); /* below R6L_SP_MARGIN "C stack overflow", below R6L_SP_HARD LUA_ERRERR */
int  lua_r6_stack_low(void);            /* 1 = below R6L_SP_MARGIN (resume: resume_error) */
uint32_t lua_r6_floor(void);            /* string matching: the lowest stack pointer allowed (0: unknown) */
static inline uint32_t lua_r6_sp(void)
{
    uint32_t sp;
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    return sp;
}

/* ---- 5.2 / 5.5: allocations ---- */
void * lua_r6_core_alloc(size_t size, int flags);           /* __mem_malloc, refused if AllocateMemory would keep < 1 MiB */
void * lua_r6_realloc(void * ptr, size_t osize, size_t nsize);  /* l_alloc (osize 0 for a new block) */
void * lua_r6_umm_heap(size_t size);                       /* the 256 KB umm heap block */
void   lua_r6_umm_stat(void);                              /* umm free into r6_lua_stat */

/* ---- 5.6 F1: file descriptors 3..18 -> FIO handles ---- */
#define LUA_R6_NFD     16
#define LUA_R6_FD0     3
int      lua_r6_fd_add(uint32_t h, uint32_t size);          /* -1: table full */
uint32_t lua_r6_fd_h(int fd);                               /* 0: not open */
uint32_t lua_r6_fd_size(int fd);
uint32_t lua_r6_fd_del(int fd);                             /* the handle (0: not open) */

/* ======== M16.1 (spec m16/m161 2.7, 5.6, 6, 8.2 Ue3 / Ue4 / Ue10 / Ue11, 8.3 B1) ======== */
/* lua.c: the script of a state */
#define LUA_R6F_REC    1                /* the script sent movie REC (6.3): r6_script_end's hint (R-4) */
#define LUA_R6F_TVW    2                /* the script switched "Tv writes" ON (K4): r6_script_end's hint */
#define LUA_R6F_BLOCK  4                /* menu.block(true) is set: cleared at the real end (R-4) */
uint32_t lua_r6_gen(lua_State * L);     /* the generation of the script of L (Ue11; 0: no living script) */
void lua_r6_flag(lua_State * L, int flag, int on);  /* LUA_R6F_* of the script of L */
int  lua_r6_psave(lua_State * L, int d);    /* a task's powersave_prohibit (+1) / powersave_permit (-1): R-4 permits the open ones;
                                         * review R2-10: 1 = counted (-1: only while > 0, so a task anchor after R-4 permits nothing twice) */
void lua_r6_mark_dead(lua_State * L);   /* an unprotected Lua error in a task of the script: no more Lua code on L (Ue3) */
/* the bits of lua_r6_once (one console line per script) */
enum { LUA_R6_ONCE_PRIO_UP, LUA_R6_ONCE_PRIO_DOWN, LUA_R6_ONCE_EXPO, LUA_R6_ONCE_REC, LUA_R6_ONCE_DISPLAY, LUA_R6_ONCE_PSAVE };

/* 2.7 (R-13): fn(arg) with the script's semaphore given back as task.yield does (only in a waiting task, R6TC_SYNC, and only if no
 * other task of this script yields: else it stays held, F10); the Lua stack must be unchanged after it */
int  lua_r6_wait(lua_State * L, int (*fn)(void *), void * arg);
/* one facade call through lua_r6_wait */
enum { LR6_SHOOT, LR6_BURST, LR6_BULB, LR6_WAITJOBS, LR6_TV, LR6_CMD, LR6_GUI, LR6_MENUSET, LR6_LVWAIT };
struct lua_r6_call
{
    int op, a, b, c;                    /* the arguments of op (see lua_r6_do_fn) */
    const char * s1, * s2, * s3;        /* menu.set: menu, entry, the string value (or NULL) */
    uint32_t gen, seq;                  /* the script's generation (set by lua_r6_do); a picture's job number */
    int done, r;                        /* pictures made (burst); the result */
    char why[64];                       /* the facade's text */
};
int  lua_r6_do(lua_State * L, struct lua_r6_call * c);   /* returns c->r */
int  luaopen_r6(lua_State * L);         /* the library "r6" (5.6) */
extern int r6_iv_running(void);         /* the core (r6_interval.c): an Interval series / Motion run is running or queued */

/* Ue4 (SB-01, SL-12): the umm critical section across both cores, nesting on one core (umm_malloc_cfg.h) */
uint32_t lua_r6_umm_enter(void);
void lua_r6_umm_exit(uint32_t old);

/* Ue3: the anchor of a Lua task (lua_task.c lua_run_task): lua_r6_park and lua_r6_abort end the task through it */
struct lua_r6_tanchor { jmp_buf jb; lua_State * L; };
int  lua_r6_tanchor_set(struct lua_r6_tanchor * a, lua_State * L);   /* 1 = registered (16 slots by task id), 0 = table full */
void lua_r6_tanchor_clear(struct lua_r6_tanchor * a);
int  lua_r6_may_wait(lua_State * L);    /* review R2-3: 1 = an anchor of L in this task (main chunk, task.create task): it may give back */
int  lua_r6_umm_once_begin(void);       /* review R2-8 (umm_malloc.c umm_init): 1 = this caller builds the heap */
void lua_r6_umm_once_end(int built);    /* after umm_init published umm_heap (built 1) or found no block (0: a later call tries again) */

/* B1 (lua/ldo.c): the task that runs this code */
uint32_t lua_r6_tid(void);

#endif
