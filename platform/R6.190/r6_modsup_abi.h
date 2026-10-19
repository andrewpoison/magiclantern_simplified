/*
 * R6.190 M16.0 (spec m16/m160 3.1, 4.1, 4.8, 4.9; plan docs/m16_plan.md, rule 13): the interface between the core
 * (r6_modsup.c / lvprobe.c, CONFIG_R6_MODSUP) and lua.mo (modules/lua/lua_r6.c, CONFIG_R6_LUA).
 *
 * Only <stdint.h> types, no other include: modules build without the platform include path (modules/Makefile) and
 * include this file by its relative path ("../../platform/R6.190/r6_modsup_abi.h"). There is no second copy of these
 * structures, keys or values in lua.mo.
 */
#ifndef _r6_modsup_abi_h_
#define _r6_modsup_abi_h_

#include <stdint.h>

/* ---- the task model (spec 3.1) ---- */
#define R6TK_REFUSED   0xFFFFFFFFu   /* odd: upstream checks (ret & 1) (lua_task.c); task_create returns id << 1 (ROM 0xE07B9408) */
#define R6TK_HDR       8             /* = LP_SYS_HDR (lvprobe.c): a stack of n bytes takes align8(n + 8) from sys_mem */
#define R6TK_RESERVE   0x1008        /* = LP_SYS_RESERVE (lvprobe.c): one Canon default stack left over; final after R-1 (M16.1) */
#define R6TK_DEFSTACK  0x1000        /* stack 0 -> [0x6954+4] = 0x1000 (ROM 0xE07B93E6, QEMU RAM) */
#define R6TK_PRIO_MAX  0x20          /* [0x6954+8]; prio 0 or > 0x20: ASSERT line 191 = Err 70 (ROM 0xE07B93CA..0xE07B93DE) */
#define R6TK_STACK_MAX 0x100000      /* a larger request is refused as a bad argument */
#define R6TK_NEED(n)   ((((n) ? (uint32_t) (n) : (uint32_t) R6TK_DEFSTACK) + R6TK_HDR + 7) & ~7u)
#define R6TK_RING      16            /* decision ring and end ring */
#define R6TK_TRAMP     16            /* trampoline slots (a task's stack high-water mark after its end) */

enum { R6TK_OK, R6TK_MEM, R6TK_UNK, R6TK_BUSY, R6TK_ARG };   /* r6tk_dec.res */

/* why a sys_mem read was not ok (r6tk_dec.why for R6TK_UNK; the LP_SYS_* codes of lvprobe.c, checked there) */
enum { R6SQ_OK, R6SQ_LAYOUT, R6SQ_NODE, R6SQ_ORDER, R6SQ_MANY, R6SQ_SUM, R6SQ_BUSY };
static inline const char * r6sq_why_name(unsigned w)
{
    return w == R6SQ_LAYOUT ? "layout" : w == R6SQ_NODE ? "node" : w == R6SQ_ORDER ? "order" : w == R6SQ_MANY ? "many" :
        w == R6SQ_SUM ? "sum" : w == R6SQ_BUSY ? "busy" : w == R6SQ_OK ? "ok" : "?";
}

/* the refusal texts (r6_modsup.c and lua.mo write the same; bytes, %d: DryOS has no %u) */
#define R6TK_TXT_MEM   "no task memory: %d < %d"     /* largest, min */
#define R6TK_TXT_UNK   "task memory unknown (%s)"    /* r6sq_why_name(why) */
#define R6TK_TXT_BUSY  "task memory busy"
#define R6TK_TXT_ARG   "bad args (prio 0x%x, stack 0x%x%s)"   /* prio, stack, "" or ", no entry" */

/* one decision of the wrapper (t_ms: ML ms clock; stack: as requested; need = R6TK_NEED(stack); min = need + R6TK_RESERVE;
 * largest / free / used: sys_mem as read right before; seq: running number (0: not in the ring); why: R6SQ_* for R6TK_UNK;
 * tramp: 1 = started through a trampoline slot, 2 = refused for entry 0) */
struct r6tk_dec { uint32_t t_ms, stack, need, min, largest, free, used; uint16_t seq; uint8_t prio, res, why, tramp; char name[16]; };
/* the end of a trampoline task: t0 / t1 start / end (ms), its stack and tskmon's high-water mark (used, free) */
struct r6tk_end { uint32_t t0, t1, stack, used, free; uint16_t seq; char name[16]; };
/* one sys_mem read (lp_sys_query, lvprobe.c): free = sum of the free list; largest = free = 0 if not ok */
struct r6_sysq { uint32_t largest, free, used, peak, count; int32_t why; };

uint32_t r6_task_try_create(const char * name, uint32_t prio, uint32_t stack, void * entry, void * arg);  /* = task_create */
uint32_t r6_task_try_create_ex(const char * name, uint32_t prio, uint32_t stack, void * entry, void * arg, struct r6tk_dec * out);
int  r6_task_ring(int i, struct r6tk_dec * e);       /* i = 0 oldest; returns the count (<= R6TK_RING) */
int  r6_task_end_ring(int i, struct r6tk_end * e);
void r6_task_counts(uint32_t c[6]);                  /* created, MEM, UNK, BUSY, ARG, ended (trampoline) */

/* ---- Lua's limits (spec 4.1): r6_lua_cfg(key); camera: constants ---- */
enum {
    R6L_STACK_LOAD,         /* lua_load_task */
    R6L_STACK_USER,         /* lua_user_load_task */
    R6L_STACK_PROP,         /* lua_prop_task */
    R6L_STACK_TASK_DEF,     /* task.create without a stack size */
    R6L_STACK_TASK_MIN,
    R6L_STACK_TASK_MAX,
    R6L_PRIO_MIN,           /* task.create priority range (E11; the upper end is Canon's maximum) */
    R6L_PRIO_MAX,
    R6L_SP_MARGIN,          /* stack guard: below this rest "C stack overflow" */
    R6L_SP_HARD,            /* stack guard: below this rest LUA_ERRERR without a text */
    R6L_AM_FLOOR,           /* Lua's allocations keep >= this free in Canon's AllocateMemory heap */
    R6L_NKEYS
};
int r6_lua_cfg(int key);

/* ---- the fault hook (spec 4.8): camera always 0; QEMU: fails / counts by site ---- */
enum {
    R6LF_ALLOC = 1,         /* a growing Lua allocation (lua_r6_realloc): != 0 -> NULL */
    R6LF_UMMHEAP,           /* before the 256 KB umm heap block: != 0 -> NULL */
    R6LF_MALLOC,            /* count only: the shim's malloc / realloc entry */
    R6LF_MARK_1,            /* load_script phase marks (count only): after luaL_newstate */
    R6LF_MARK_2,            /* after the requiref calls */
    R6LF_MARK_3,            /* after _PRELOAD */
    R6LF_MARK_4,            /* after createargtable */
    R6LF_MARK_5,            /* before luaL_loadfile */
    R6LF_MARK_6,            /* before docall */
    R6LF_MARK_7             /* after docall */
};
int r6_lua_fault(int site);

/* ---- counters written by lua.mo, read by the QEMU tests now and by the LVP log in M16.1 (spec 4.9) ---- */
enum { R6LK_LOAD, R6LK_USER, R6LK_PROP, R6LK_TASK, R6LK_OTHER, R6LK_N };   /* task kinds */
struct r6_lua_stat
{
    uint32_t ring_ok, ring_drop, ring_bad;      /* property ring: delivered; dropped (full / no queue / post failed); bad index */
    uint32_t aborts, abort_close, dead, park;   /* abort(): to an anchor; lua_close after it; scripts left dead; no anchor (parked) */
    uint32_t sp_hits, sp_unknown;               /* stack guard: errors raised; r6_stack_left() < 0 */
    int32_t  sp_min[R6LK_N];                    /* smallest stack rest seen per task kind (-1 never) */
    uint32_t umm_free, umm_min;                 /* umm heap free: last, smallest (0 never) */
    uint32_t am_refused, am_unk;                /* AllocateMemory floor: refused; source unknown, kept */
    uint32_t mallocs;                           /* shim malloc / realloc calls */
    uint32_t prio_clamped, stack_clamped;       /* task.create requests changed */
    uint32_t cpu_prod, cpu_cons;                /* property ring: cores seen (bit 1 << r6_cpu_id()) by producer / consumer */
    uint32_t cpu_kind[R6LK_N];                  /* cores seen by the stack guard per task kind at each new low of sp_min */
    uint32_t prop_skip;                         /* property events passed over an entry without an active handler of a living
                                                 * script (closed: LUA_NOREF, dead; review R2-01 / R2-04) */
};
struct r6_lua_stat * r6_lua_stat(void);

/* ---- small core helpers ---- */
int  r6_stack_left(void);                                      /* rest of the running task's stack in bytes, -1 unknown */
int  r6_cas32(volatile uint32_t * p, uint32_t old, uint32_t neu); /* 1 = *p was old and is neu now (ldrex / strex, barriers) */
void r6_barrier(void);                                         /* dmb */
int  r6_cpu_id(void);                                          /* MPIDR & 3 */
int  r6_am_left(const void * p, uint32_t size);                /* AllocateMemory free after p (malloc pool: INT_MAX; -1 unknown) */

#endif
