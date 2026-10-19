/*
 * R6.190 M16.0 (spec m16/m160 3.5, 4, 5; CONFIG_R6_LUA): lua.mo's part of the Lua / module groundwork (lua_r6.c).
 * The core interface (task model, limits, fault hook, counters, atomics) is platform/R6.190/r6_modsup_abi.h.
 */
#ifndef _lua_r6_h_
#define _lua_r6_h_

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include "lua/lua.h"
#include "../../platform/R6.190/r6_modsup_abi.h"

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

#endif
