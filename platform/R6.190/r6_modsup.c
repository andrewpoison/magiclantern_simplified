/*
 * R6.190 M16.0 (spec m16/m160 2, 3, 4.1, 4.7..4.9; plan docs/m16_plan.md M16.0, rule 13; CONFIG_R6_MODSUP):
 * the core of the Lua / module groundwork.
 *
 *  - r6_task_try_create(): every task of a module (task_create in R6_190.sym points here, r6_symalias.py), of lua.mo and of
 *    run_in_separate_task. Right before Canon's task_create it reads Canon's task memory sys_mem (Tor 0h, lp_sys_query in
 *    lvprobe.c: the M14.1c walk, read only) and creates the task only if one free block >= align8(stack + 8) + 4,104 B
 *    exists at that moment; "unknown" refuses; priority 0 or > 0x20 and entry 0 refuse (Canon's task_create asserts there:
 *    line 191, Err 70). One lock serialises check and create, also for the snapshot task (lvprobe.c lp_snap_start). It
 *    refuses when the memory is missing at creation time; a task created early is not covered against Canon's later needs
 *    (spec 3.1 A1). Refusal: R6TK_REFUSED (odd), a console line, through the plain entry also a NotifyBox.
 *  - a trampoline per created task (16 slots) records the task's end and tskmon's stack high-water mark (end ring)
 *  - the five symbols modules import that the R6 build lacks (spec 2)
 *  - Lua's limits r6_lua_cfg(), the stack rest r6_stack_left(), the fault hook r6_lua_fault() (camera: 0), the counters
 *    r6_lua_stat(), the atomics r6_cas32() / r6_barrier(), r6_cpu_id()
 *
 * Canon: task_create (one call), create_named_semaphore / take_semaphore / give_semaphore, msleep, the data stub
 * current_task (read only, as tskmon / get_current_task_id). No Canon write, no new Canon address, no new stub.
 */
#include "dryos.h"
#include "tasks.h"
#include "propvalues.h"
#include "shoot.h"
#include "state-object.h"
#include "screenshot.h"

#ifdef CONFIG_R6_MODSUP
#ifndef CONFIG_R6_TASKMEM
#error "r6_modsup.c: CONFIG_R6_MODSUP needs CONFIG_R6_TASKMEM (lp_sys_query: the M14.1c sys_mem read)"
#endif
#ifndef CONFIG_R6_SNAPMEM
#error "r6_modsup.c: CONFIG_R6_MODSUP needs CONFIG_R6_SNAPMEM (r6_am_left: mem.c's record)"
#endif

#include "r6_faceyuv.h"
#include "r6_interval.h"
#include "r6_modsup.h"

extern void tskmon_stack_check(uint32_t task_id);                    /* src/tskmon.c (ML) */
extern void tskmon_stack_get_max(uint32_t task_id, uint32_t * used, uint32_t * free);
extern int r6_iv_interval_idx;                                       /* r6_interval.c (CONFIG_INT, saved) */

/* ======== 2: the five symbols (prototypes: src/shoot.h, src/state-object.h, src/screenshot.h) ======== */

/* a copy of iv_intervals[] (r6_interval.c), equality checked by the stage gate */
static const int r6mod_iv_s[] = { 3, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800, 3600, 5400, 7200 };
#define R6MOD_IV_N ((int) (sizeof(r6mod_iv_s) / sizeof(r6mod_iv_s[0])))
_Static_assert(R6MOD_IV_N == 18, "r6mod_iv_s: the 18 intervals of r6_interval.c");

/* pictures of Interval's current / last series ("Test: 1 picture" and the motion trigger: 0) */
int get_interval_count()
{
    const struct r6_iv_stats * s = r6_iv_stats();
    return s->mode == R6IV_MODE_INTERVAL && !s->test ? s->n_done : 0;
}

/* seconds of the interval set in the menu */
int get_interval_time()
{
    int i = r6_iv_interval_idx;
    if (i < 0) i = 0;
    if (i > R6MOD_IV_N - 1) i = R6MOD_IV_N - 1;
    return r6mod_iv_s[i];
}

/* the smallest interval >= s, like a turn in the menu (saved with the config); a running series keeps its own (fixed at
 * its start, r6_interval.c), so nothing changes while one runs */
void set_interval_time(int seconds)
{
    if (r6_iv_running())
    {
        printf("[R6] set_interval_time(%d): Interval runs, unchanged\n", seconds);
        return;
    }
    int i = 0;
    while (i < R6MOD_IV_N - 1 && r6mod_iv_s[i] < seconds) i++;
    r6_iv_interval_idx = i;
}

/* wait_lv_frames: FaceYuv's frame counter (the newest ring entry, r6_fy_locate; read only) every 20 ms */
#define R6MOD_WLV_POLL_MS  20
#define R6MOD_WLV_BASE_MS  500       /* give up after 500 + 100 * n ms ... */
#define R6MOD_WLV_PER_MS   100
#define R6MOD_WLV_MAX_MS   30000     /* ... at most 30 s */

/* noinline: the QEMU tests replace the frame source here */
static int __attribute__((noinline)) r6mod_fy_frame(uint32_t * frame)
{
    struct r6_fy_loc l;
    if (!r6_fy_locate(&l)) return 0;
    *frame = l.ring_frame;
    return 1;
}

/* 1 = the counter moved by >= n (upstream: 1 ok, 0 failure); 0 = LV off, LV stopped or no frames in time */
int wait_lv_frames(int n)
{
    if (n <= 0) n = 1;
    if (!lv) return 0;
    int limit = n >= (R6MOD_WLV_MAX_MS - R6MOD_WLV_BASE_MS) / R6MOD_WLV_PER_MS ? R6MOD_WLV_MAX_MS
        : R6MOD_WLV_BASE_MS + R6MOD_WLV_PER_MS * n;
    uint32_t f0 = 0, f = 0;
    int have = r6mod_fy_frame(&f0);
    int t0 = get_ms_clock();
    while (1)
    {
        msleep(R6MOD_WLV_POLL_MS);
        if (!lv) return 0;
        if (r6mod_fy_frame(&f))
        {
            if (!have) { f0 = f; have = 1; }
            else if (f - f0 >= (uint32_t) n) return 1;
        }
        if (get_ms_clock() - t0 >= limit) return 0;
    }
}

/* not in the R6 build yet (FEATURE_SCREENSHOT): upstream's failure value */
int take_screenshot(char * filename, uint32_t mode)
{
    printf("[R6] take_screenshot: not available yet (M16.2)\n");
    return 0;
}

/* ======== 4.9: atomics, CPU ======== */

/* noinline (both): ldrex / strex / dmb stay in these two functions (stage gate G5) */
int __attribute__((noinline)) r6_cas32(volatile uint32_t * p, uint32_t old, uint32_t neu)
{
    return __sync_bool_compare_and_swap(p, old, neu);      /* dmb; ldrex / strex loop; dmb */
}

void __attribute__((noinline)) r6_barrier(void)
{
    __sync_synchronize();                                  /* dmb */
}

int r6_cpu_id(void)
{
    return get_cpu_id();                                   /* MPIDR & 3 (src/arm-mcr.h) */
}

static void r6tk_inc(volatile uint32_t * p)
{
    uint32_t v;
    do v = *p; while (!r6_cas32(p, v, v + 1));
}

/* ======== 3.3.1: the loader's alias check (written by src/module.c) ======== */
int r6_alias_seen = 0, r6_alias_ok = 0;

/* ======== 3.1: the task model ======== */

static struct semaphore * r6tk_sem = 0;
static struct r6tk_dec r6tk_ring[R6TK_RING];        /* decisions (written under the lock) */
static uint32_t r6tk_ring_n = 0;                    /* decisions since boot that are in the ring */
static struct r6tk_end r6tk_endr[R6TK_RING];        /* trampoline ends (a slot per end, claimed with r6_cas32) */
static volatile uint32_t r6tk_end_n = 0;
static volatile uint32_t r6tk_cnt[6];               /* created, MEM, UNK, BUSY, ARG, ended */
static uint16_t r6tk_seq = 0;                       /* running number (under the lock) */
static int r6tk_nb_t = 0, r6tk_nb_any = 0;          /* NotifyBox rate: at most one per 2 s */

/* a trampoline slot: tid = the task (set at its start), so a task maps to its decision (seq) */
struct r6tk_tr { void (*entry)(void *); void * arg; uint32_t stack; volatile uint32_t tid; uint16_t seq; volatile uint8_t used; char name[16]; };
static struct r6tk_tr r6tk_trs[R6TK_TRAMP];

int __attribute__((noinline)) r6tk_lock(int ms)
{
    if (!r6tk_sem) return 0;
    return take_semaphore(r6tk_sem, ms) == 0;
}

void __attribute__((noinline)) r6tk_unlock(void)
{
    give_semaphore(r6tk_sem);
}

/* review R1-04: the lock exists (without it the wrapper creates nothing: BUSY, so lp_snap_start has no rival then) */
int __attribute__((noinline)) r6tk_have_sem(void)
{
    return r6tk_sem != 0;
}

static void r6tk_name(char * d, const char * s)
{
    int i = 0;
    if (s) for (; i < 15 && s[i]; i++) d[i] = s[i];
    for (; i < 16; i++) d[i] = 0;
}

/* the task's own frame: entry(arg), then its stack high-water mark (tskmon, as lp_snap_entry) into the end ring */
static void r6tk_tramp(void * p)
{
    struct r6tk_tr * r = p;
    struct r6tk_end e;
    void (*entry)(void *) = r->entry;
    void * arg = r->arg;
    r->tid = get_current_task_id();
    e.t0 = get_ms_clock();
    e.stack = r->stack;
    e.seq = r->seq;
    memcpy(e.name, r->name, sizeof(e.name));
    entry(arg);
    uint32_t used = 0, fr = 0;
    int id = get_current_task_id();
    tskmon_stack_check(id);
    msleep(20);
    tskmon_stack_get_max(id, &used, &fr);
    e.t1 = get_ms_clock();
    e.used = used;
    e.free = fr;
    uint32_t k;
    do k = r6tk_end_n; while (!r6_cas32(&r6tk_end_n, k, k + 1));
    r6tk_endr[k % R6TK_RING] = e;
    r6tk_inc(&r6tk_cnt[5]);
    r->tid = 0;
    r6_barrier();
    r->used = 0;
}

/* the reason of a refusal as text (the R6TK_TXT_* formats of r6_modsup_abi.h; lua.mo writes the same texts) */
static void r6tk_why(const struct r6tk_dec * d, char * b, int n)
{
    if (d->res == R6TK_MEM) snprintf(b, n, R6TK_TXT_MEM, (int) d->largest, (int) d->min);
    else if (d->res == R6TK_UNK) snprintf(b, n, R6TK_TXT_UNK, r6sq_why_name(d->why));
    else if (d->res == R6TK_BUSY) snprintf(b, n, R6TK_TXT_BUSY);
    else snprintf(b, n, R6TK_TXT_ARG, (int) d->prio, (int) d->stack, d->tramp == 2 ? ", no entry" : "");
}

/* spec 3.1: check right before task_create, under the lock; *out (optional) gets the decision */
uint32_t __attribute__((noinline)) r6_task_try_create_ex(const char * name, uint32_t prio, uint32_t stack, void * entry, void * arg,
    struct r6tk_dec * out)
{
    struct r6tk_dec d;
    uint32_t ret = R6TK_REFUSED;
    memset(&d, 0, sizeof(d));
    r6tk_name(d.name, name);
    d.stack = stack;
    d.prio = prio > 0xFF ? 0xFF : prio;
    d.need = R6TK_NEED(stack);
    d.min = d.need + R6TK_RESERVE;
    d.res = R6TK_ARG;
    if (!entry) d.tramp = 2;                          /* only for the text ("no entry"); no task */
    int arg_ok = entry && stack <= R6TK_STACK_MAX && prio != 0 && prio <= R6TK_PRIO_MAX;   /* else Canon's ASSERT 191 */
    if (!r6tk_lock(1000))
    {
        d.t_ms = get_ms_clock();
        d.res = arg_ok ? R6TK_BUSY : R6TK_ARG;        /* not in the ring (written under the lock only) */
        r6tk_inc(&r6tk_cnt[d.res]);
        goto refused;
    }
    d.seq = ++r6tk_seq;
    if (arg_ok)
    {
        struct r6_sysq q;
        int ok = lp_sys_query(&q);                    /* Tor 0h, read only: right before task_create */
        d.t_ms = get_ms_clock();
        d.largest = q.largest;
        d.free = q.free;
        d.used = q.used;
        d.why = q.why;
        if (!ok)
            d.res = R6TK_UNK;                         /* "unknown" refuses (rule 13; unlike the M14.1c snapshot) */
        else if (q.largest < d.min)
            d.res = R6TK_MEM;
        else
        {
            struct r6tk_tr * r = 0;
            for (int i = 0; i < R6TK_TRAMP; i++)
                if (!r6tk_trs[i].used) { r = &r6tk_trs[i]; break; }
            if (r)
            {
                r->used = 1;
                r->tid = 0;
                r->entry = entry;
                r->arg = arg;
                r->stack = stack;
                r->seq = d.seq;
                memcpy(r->name, d.name, sizeof(r->name));
                d.tramp = 1;
            }
            d.res = R6TK_OK;
            /* the only task_create of the wrapper (one call site) */
            ret = task_create(name, prio, stack, r ? (void *) r6tk_tramp : entry, r ? (void *) r : arg);
            if ((ret & 1) && r) r->used = 0;          /* not created (Canon would assert instead) */
        }
    }
    else
        d.t_ms = get_ms_clock();
    r6tk_ring[r6tk_ring_n % R6TK_RING] = d;
    r6tk_ring_n++;
    r6tk_inc(&r6tk_cnt[d.res]);
    r6tk_unlock();
    if (d.res == R6TK_OK) goto done;
refused:
    {
        char why[48];
        r6tk_why(&d, why, sizeof(why));
        printf("[R6] task %s refused: %s\n", d.name, why);
    }
done:
    if (out) *out = d;
    return ret;
}

/* the plain entry (= task_create for modules through the R6_190.sym alias, run_in_separate_task): also a NotifyBox */
uint32_t r6_task_try_create(const char * name, uint32_t prio, uint32_t stack, void * entry, void * arg)
{
    struct r6tk_dec d;
    uint32_t ret = r6_task_try_create_ex(name, prio, stack, entry, arg, &d);
    if (ret == R6TK_REFUSED)
    {
        int now = get_ms_clock();
        if (!r6tk_nb_any || now - r6tk_nb_t >= 2000)
        {
            char why[48];
            r6tk_why(&d, why, sizeof(why));
            r6tk_nb_any = 1;
            r6tk_nb_t = now;
            NotifyBox(3000, "%s: %s", d.name, why);
        }
    }
    return ret;
}

int r6_task_ring(int i, struct r6tk_dec * e)
{
    int lk = r6tk_lock(1000);
    uint32_t total = r6tk_ring_n;
    int n = total < R6TK_RING ? (int) total : R6TK_RING;
    if (e && i >= 0 && i < n) *e = r6tk_ring[(total - n + i) % R6TK_RING];
    if (lk) r6tk_unlock();
    return n;
}

int r6_task_end_ring(int i, struct r6tk_end * e)
{
    uint32_t total = r6tk_end_n;
    int n = total < R6TK_RING ? (int) total : R6TK_RING;
    if (e && i >= 0 && i < n) *e = r6tk_endr[(total - n + i) % R6TK_RING];
    return n;
}

void r6_task_counts(uint32_t c[6])
{
    for (int i = 0; i < 6; i++) c[i] = r6tk_cnt[i];
}

/* ======== 4.1: Lua's limits (camera: constants; measured in QEMU, spec 11 / Rev. 3, m16/m160 meas_m160.md: stack S = the next 4 KB
 * step >= max(1.5 x U, U + margin + 10 x 448 B per pcall level); U = natural need LOAD / USER 7,224, PROP 2,120, TASK 4,244 B;
 * margin = the next 256 B step >= 1.5 x 1,152 B (deepest C stack below the last check, gsub) + hard) ======== */
static const int r6l_cfg[R6L_NKEYS] = {
    [R6L_STACK_LOAD]     = 0x4000,
    [R6L_STACK_USER]     = 0x4000,
    [R6L_STACK_PROP]     = 0x3000,
    [R6L_STACK_TASK_DEF] = 0x3000,
    [R6L_STACK_TASK_MIN] = 0x2000,  /* rule 4: base 720 + margin + 10 x 448 B -> 8 KB (E11 said 4 KB) */
    [R6L_STACK_TASK_MAX] = 0x4000,  /* E11 */
    [R6L_PRIO_MIN]       = 0x1C,
    [R6L_PRIO_MAX]       = 0x1F,    /* spec 4.3: one below Canon's 0x20 (QEMU m160 E2: idle 0x21, but PowerMgr / ClockSaverTask at 0x20
                                     * kept a 0x20 Lua task from ever running; the rule's reason, impl_notes.md) */
    [R6L_SP_MARGIN]      = 2816,
    [R6L_SP_HARD]        = 1024,    /* the rest after LUA_ERRERR stayed >= 856 B (rule 2: +512 only below 512 B) */
    [R6L_AM_FLOOR]       = 0x100000,
};

int r6_lua_cfg(int key)
{
    return (unsigned) key < R6L_NKEYS ? r6l_cfg[key] : 0;
}

/* ======== 4.8: the fault hook (camera: never fails, counts nothing) ======== */
int __attribute__((noinline)) r6_lua_fault(int site)
{
    return 0;
}

/* ======== 4.9: the counters ======== */
static struct r6_lua_stat r6l_stat;

struct r6_lua_stat * r6_lua_stat(void)
{
    return &r6l_stat;
}

/* ======== 4.7: the rest of the running task's stack (as tskmon: stackStartAddr / stackSize) ======== */
int __attribute__((noinline)) r6_stack_left(void)
{
    uint32_t sp;
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    struct task * t = current_task;                   /* data stub 0x1028, read only */
    if (!r6_fy_ram_ok((uint32_t) t, 0x24)) return -1; /* the record up to stackSize */
    uint32_t lo = t->stackStartAddr, n = t->stackSize;
    if (sp < lo || sp > lo + n) return -1;            /* not this task's stack (also current_task of the other core) */
    return sp - lo;
}

static void r6_modsup_init()
{
    r6tk_sem = create_named_semaphore("r6tk_sem", SEM_CREATE_UNLOCKED);
}

INIT_FUNC("r6_modsup", r6_modsup_init);

#endif /* CONFIG_R6_MODSUP */
