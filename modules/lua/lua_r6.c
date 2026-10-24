/*
 * R6.190 M16.0 (spec m16/m160 3.5, 4.5..4.9, 5.2, 5.5, 5.6 F1; plan docs/m16_plan.md M16.0, E11; CONFIG_R6_LUA): lua.mo's part
 * of the Lua / module groundwork.
 *  - lua_r6_start: the five task sites through the core wrapper r6_task_try_create_ex (Canon's task memory checked right before
 *    task_create; a refusal gives the text "no task memory: N < M"); stacks by kind from the core table r6_lua_cfg()
 *  - the property ring: 16 slots, filled in PropMgr without malloc, without a lock and without the Lua API (slots taken with
 *    the core's r6_cas32: ldrex / strex across both cores), handed to lua_prop_task through a 16-message queue
 *  - abort(): an anchor per load_script (setjmp); abort() jumps back to it, else the task sleeps (no busy loop)
 *  - the stack guard: the rest of the running task's stack (core r6_stack_left) at every C recursion point of Lua
 *  - allocations: Canon's AllocateMemory keeps >= 1 MiB free (R6L_AM_FLOOR), the umm heap may be NULL, the fault hook
 *  - file descriptors 3..18 -> FIO handles (upstream accepted only handles 3..16)
 * Values the QEMU tests change live in the core (r6_lua_cfg, r6_lua_fault): this file has no QEMU-only code (QEMU runs the camera lua.mo).
 * M16.1 (spec m16/m161 2.7, 5.6, 8.2 Ue3 / Ue4 / Ue9, 8.3 B1): lua_r6_wait (R-13), the facade calls, the library "r6", the anchors of the
 * Lua tasks (Ue3), the umm lock across both cores (Ue4), "C stack unknown" (Ue9), the task id for the error frames (B1).
 */
#ifdef CONFIG_R6_LUA   /* the whole file (spec m16/m160 8: every change in modules/lua inside CONFIG_R6_LUA) */
#include <dryos.h>
#include <string.h>
#include "lua_common.h"
#include "lua_r6.h"
#include "lua/ldebug.h"
#include "lua/ldo.h"
#include "umm_malloc/umm_malloc.h"

extern void * __mem_malloc(size_t len, unsigned int flags, const char * file, unsigned int line);
extern void __mem_free(void * buf);
extern void console_puts(const char * str);   /* src/console.c (no newline added) */

static void lua_r6_inc(volatile uint32_t * p)
{
    uint32_t v;
    do v = *p; while (!r6_cas32(p, v, v + 1));
}

/* ======== the core table, cached (lua_init, every lua_r6_start) ======== */
static int lua_r6_c[R6L_NKEYS];

static void lua_r6_cfg_load(void)
{
    for (int k = 0; k < R6L_NKEYS; k++) lua_r6_c[k] = r6_lua_cfg(k);
}

int lua_r6_cfg(int key)
{
    return (unsigned) key < R6L_NKEYS ? lua_r6_c[key] : 0;
}

void lua_r6_init(void)
{
    struct r6_lua_stat * st = r6_lua_stat();
    lua_r6_cfg_load();
    for (int k = 0; k < R6LK_N; k++) st->sp_min[k] = -1;
}

/* ======== 3.5: the task sites ======== */
/* a slot per started task: its kind for the stack guard's statistics (sp_min per kind) */
struct lua_r6_ts { volatile uint32_t used; volatile uint32_t tid; int kind; void * fn; void * arg; };
#define LUA_R6_NTS 16
static struct lua_r6_ts lua_r6_ts[LUA_R6_NTS];

static void lua_r6_entry(void * p)
{
    struct lua_r6_ts * s = p;
    void (*fn)(void *) = (void (*)(void *)) s->fn;
    void * arg = s->arg;
    s->tid = get_current_task_id();
    r6_barrier();
    fn(arg);
    s->tid = 0;
    r6_barrier();
    s->used = 0;
}

static int lua_r6_kind(void)
{
    uint32_t tid = get_current_task_id();
    for (int i = 0; tid && i < LUA_R6_NTS; i++)
        if (lua_r6_ts[i].used && lua_r6_ts[i].tid == tid) return lua_r6_ts[i].kind;
    return R6LK_OTHER;
}

/* the refusal text (the core's R6TK_TXT_* formats) */
static void lua_r6_why(const struct r6tk_dec * d, char * b, int n)
{
    if (d->res == R6TK_MEM) snprintf(b, n, R6TK_TXT_MEM, (int) d->largest, (int) d->min);
    else if (d->res == R6TK_UNK) snprintf(b, n, R6TK_TXT_UNK, r6sq_why_name(d->why));
    else if (d->res == R6TK_BUSY) snprintf(b, n, R6TK_TXT_BUSY);
    else snprintf(b, n, R6TK_TXT_ARG, (int) d->prio, (int) d->stack, d->tramp == 2 ? ", no entry" : "");
}

/* kind LOAD / USER / PROP: the stack from the core table; TASK: as given. R6TK_REFUSED: why gets the reason */
uint32_t lua_r6_start(int kind, const char * name, int prio, int stack, void * fn, void * arg, char * why, int n)
{
    lua_r6_cfg_load();
    if (kind == R6LK_LOAD) stack = lua_r6_c[R6L_STACK_LOAD];
    else if (kind == R6LK_USER) stack = lua_r6_c[R6L_STACK_USER];
    else if (kind == R6LK_PROP) stack = lua_r6_c[R6L_STACK_PROP];
    struct lua_r6_ts * s = 0;
    for (int i = 0; i < LUA_R6_NTS; i++)
    {
        if (r6_cas32(&lua_r6_ts[i].used, 0, 1)) { s = &lua_r6_ts[i]; break; }
    }
    if (s)
    {
        s->tid = 0;
        s->kind = kind;
        s->fn = fn;
        s->arg = arg;
        r6_barrier();
    }
    struct r6tk_dec d;
    uint32_t r = r6_task_try_create_ex(name, prio, stack, s ? (void *) lua_r6_entry : fn, s ? (void *) s : arg, &d);
    if (r == R6TK_REFUSED)
    {
        if (s) { r6_barrier(); s->used = 0; }
        if (why && n > 0) lua_r6_why(&d, why, n);
    }
    else if (why && n > 0) why[0] = 0;
    return r;
}

/* ======== 4.5: the property ring ======== */
#define LUA_R6_PS_FREE 0
#define LUA_R6_PS_BUSY 1
#define LUA_R6_PS_FULL 2
struct lua_r6_pslot { volatile uint32_t state; struct lua_r6_pmsg m; };
static struct lua_r6_pslot lua_r6_pr[LUA_R6_PRING];

/* producer (PropMgr, any task): a free slot, the value, then its index (1..16) to the queue; nothing free or the post failed:
 * dropped (counted). No malloc, no Lua API, no lock. */
void lua_r6_prop_post(struct msg_queue * q, unsigned id, const void * addr, unsigned len)
{
    struct r6_lua_stat * st = r6_lua_stat();
    for (int i = 0; q && i < LUA_R6_PRING; i++)
    {
        struct lua_r6_pslot * s = &lua_r6_pr[i];
        if (!r6_cas32(&s->state, LUA_R6_PS_FREE, LUA_R6_PS_BUSY)) continue;
        unsigned n = addr ? (len < LUA_R6_PDATA - 1 ? len : LUA_R6_PDATA - 1) : 0;
        st->cpu_prod |= 1 << r6_cpu_id();            /* INFO (spec 4.5 E6, Ue4): the producer's core */
        s->m.id = id;
        s->m.len = n;
        if (n) memcpy(s->m.data, addr, n);
        s->m.data[n] = 0;
        r6_barrier();
        s->state = LUA_R6_PS_FULL;
        if (msg_queue_post(q, i + 1) == 0) return;
        r6_barrier();
        s->state = LUA_R6_PS_FREE;
        break;
    }
    lua_r6_inc(&st->ring_drop);
}

/* consumer (lua_prop_task): the slot of a queue message, copied out and freed */
int lua_r6_prop_take(int idx, struct lua_r6_pmsg * m)
{
    struct r6_lua_stat * st = r6_lua_stat();
    if (idx < 1 || idx > LUA_R6_PRING || lua_r6_pr[idx - 1].state != LUA_R6_PS_FULL)
    {
        lua_r6_inc(&st->ring_bad);
        return 0;
    }
    struct lua_r6_pslot * s = &lua_r6_pr[idx - 1];
    r6_barrier();
    memcpy(m, &s->m, sizeof(*m));
    r6_barrier();
    s->state = LUA_R6_PS_FREE;
    lua_r6_inc(&st->ring_ok);
    st->cpu_cons |= 1 << r6_cpu_id();                /* INFO (spec 4.5 E6, Ue4): the consumer's core */
    return 1;
}

/* ======== 4.6: abort() ======== */
static struct lua_r6_anchor * lua_r6_al = 0;     /* the anchors (on the stacks of the tasks in load_script) */
static volatile uint32_t lua_r6_alk = 0;         /* their lock word (r6_cas32) */

static void lua_r6_alock(void)
{
    while (!r6_cas32(&lua_r6_alk, 0, 1)) msleep(10);
}

static void lua_r6_aunlock(void)
{
    r6_barrier();
    lua_r6_alk = 0;
}

void lua_r6_anchor_push(struct lua_r6_anchor * a, void * script, const char * fname)
{
    a->tid = get_current_task_id();
    a->script = script;
    a->fname = fname;
    a->L = 0;
    a->sem = 0;
    lua_r6_alock();
    a->next = lua_r6_al;
    lua_r6_al = a;
    lua_r6_aunlock();
}

void lua_r6_anchor_pop(struct lua_r6_anchor * a)
{
    lua_r6_alock();
    for (struct lua_r6_anchor ** p = &lua_r6_al; *p; p = &(*p)->next)
    {
        if (*p == a) { *p = a->next; break; }
    }
    lua_r6_aunlock();
}

static struct lua_r6_anchor * lua_r6_anchor_find(void)
{
    uint32_t tid = get_current_task_id();
    struct lua_r6_anchor * a;
    lua_r6_alock();
    for (a = lua_r6_al; a; a = a->next)
    {
        if (a->tid == tid) break;
    }
    lua_r6_aunlock();
    return a;
}

void lua_r6_anchor_L(lua_State * L)
{
    struct lua_r6_anchor * a = lua_r6_anchor_find();
    if (a) a->L = L;
}

void lua_r6_anchor_sem(int held)
{
    struct lua_r6_anchor * a = lua_r6_anchor_find();
    if (a) a->sem = held;
}

static struct lua_r6_tanchor * lua_r6_tanchor_find(void);

/* M16.1 (spec m16/m161 8.2 Ue3): a Lua task with an anchor ends through it (lua_run_task: powersave_permit, its jobs cancelled,
 * free, the task returns); else the task sleeps for good (M16.0) */
void lua_r6_park(const char * why)
{
    struct lua_r6_tanchor * t = lua_r6_tanchor_find();
    if (t)
    {
        lua_r6_inc(&r6_lua_stat()->dead_exit);
        longjmp(t->jb, 1);
    }
    while (1) msleep(1000);
}

/* an unprotected Lua error (luaD_throw without a handler, after the panic function): back to the anchor of this task's
 * load_script (lua.c cleans up), else the task sleeps for good (another task's frame cannot be entered) */
void lua_r6_abort(void)
{
    struct r6_lua_stat * st = r6_lua_stat();
    struct lua_r6_anchor * a = lua_r6_anchor_find();
    char line[128];
    snprintf(line, sizeof(line), "[Lua] abort in task %s (%s): unprotected Lua error\n", get_current_task_name(),
        a && a->fname ? a->fname : "-");
    console_show();
    console_puts(line);
    if (a)
    {
        lua_r6_anchor_pop(a);
        lua_r6_inc(&st->aborts);
        longjmp(a->jb, 1);
    }
    /* M16.1 (spec m16/m161 8.1 R-3, 8.2 Ue3): in a Lua task: the script is dead (its state may be broken; the semaphore stays with
     * this task, callbacks skip the script, its other tasks end at their next take), the task ends through its anchor */
    struct lua_r6_tanchor * t = lua_r6_tanchor_find();
    if (t)
    {
        lua_r6_mark_dead(t->L);
        lua_r6_inc(&st->aborts);
        longjmp(t->jb, 2);
    }
    lua_r6_inc(&st->park);
    lua_r6_park("abort");
}

/* ======== 4.7: the stack guard ======== */
static int lua_r6_left(void)
{
    int left = r6_stack_left();
    struct r6_lua_stat * st = r6_lua_stat();
    if (left < 0)
    {
        st->sp_unknown++;
        return -1;
    }
    int k = lua_r6_kind();
    if (st->sp_min[k] < 0 || left < st->sp_min[k])
    {
        st->sp_min[k] = left;
        st->cpu_kind[k] |= 1 << r6_cpu_id();         /* INFO (spec 4.7 K6, Ue4): the core at each new low (one MRC per low, not per check) */
    }
    return left;
}

/* luaD_call, the parser: below R6L_SP_HARD LUA_ERRERR (no text, little stack), below R6L_SP_MARGIN "C stack overflow"; unknown
 * (r6_stack_left < 0): only upstream's counted limits */
void lua_r6_stack_check(lua_State * L)
{
    int left = lua_r6_left();
    /* M16.1 (spec m16/m161 8.2 Ue9): the core finds a wrapper task by its trampoline slot first; still unknown: no Lua code here */
    if (left < 0) luaG_runerror(L, "C stack unknown");
    if (left < lua_r6_c[R6L_SP_HARD])
    {
        r6_lua_stat()->sp_hits++;
        luaD_throw(L, LUA_ERRERR);
    }
    if (left < lua_r6_c[R6L_SP_MARGIN])
    {
        r6_lua_stat()->sp_hits++;
        luaG_runerror(L, "C stack overflow");
    }
}

/* resume (ldo.c): 1 = below R6L_SP_MARGIN (the caller raises the error inside the protected resume) */
int lua_r6_stack_low(void)
{
    int left = lua_r6_left();
    if (left >= 0 && left < lua_r6_c[R6L_SP_MARGIN])
    {
        r6_lua_stat()->sp_hits++;
        return 1;
    }
    return 0;
}

/* string matching (lstrlib.c): match() compares its own stack pointer with this floor; 0 = unknown (only MAXCCALLS) */
uint32_t lua_r6_floor(void)
{
    uint32_t sp = lua_r6_sp();
    int left = lua_r6_left();
    if (left < 0) return 0;
    return sp - left + lua_r6_c[R6L_SP_MARGIN];
}

/* ======== 5.2 / 5.5: allocations ======== */
/* Canon's AllocateMemory keeps >= R6L_AM_FLOOR free after a block of Lua's (mem.c's record of this very call; the malloc pool
 * keeps its own reserve); source unknown: kept, counted */
void * lua_r6_core_alloc(size_t size, int flags)
{
    struct r6_lua_stat * st = r6_lua_stat();
    if (!lua_r6_c[R6L_AM_FLOOR]) lua_r6_cfg_load();
    void * p = __mem_malloc(size, flags, "lua", 0);
    if (!p) return 0;
    int left = r6_am_left(p, size);
    if (left == -1)
    {
        lua_r6_inc(&st->am_unk);
        return p;
    }
    if (left < lua_r6_c[R6L_AM_FLOOR])
    {
        __mem_free(p);
        lua_r6_inc(&st->am_refused);
        return 0;
    }
    return p;
}

/* l_alloc (lauxlib.c) instead of the core realloc (src/posix.c copies into NULL); osize 0 for a new block. umm blocks as
 * upstream (ml-lua-shim.c my_realloc); a core block shrinks in place and grows into a new block (NULL: the old one stays) */
void * lua_r6_realloc(void * ptr, size_t osize, size_t nsize)
{
    r6_lua_fault(R6LF_MALLOC);
    r6_lua_stat()->mallocs++;
    if (nsize > osize && r6_lua_fault(R6LF_ALLOC)) return 0;
    if (umm_ptr_in_heap(ptr) || (!ptr && nsize < 1024)) return umm_realloc(ptr, nsize);
    if (nsize <= osize) return ptr;
    void * n = lua_r6_core_alloc(nsize, 0);
    if (!n) return 0;
    if (ptr)
    {
        memcpy(n, ptr, osize);
        __mem_free(ptr);
    }
    return n;
}

void * lua_r6_umm_heap(size_t size)
{
    if (r6_lua_fault(R6LF_UMMHEAP)) return 0;
    return lua_r6_core_alloc(size, 0);
}

void lua_r6_umm_stat(void)
{
    struct r6_lua_stat * st = r6_lua_stat();
    uint32_t f = umm_free_heap_size();
    st->umm_free = f;
    if (f && (!st->umm_min || f < st->umm_min)) st->umm_min = f;
}

/* ======== TCC and dietlibc (QEMU m160 K3): isspace / isalnum in lua.mo ========
 * dietlibc (the archive in lua.mo, built without -mlong-calls) calls them with BL. ML's TCC turns a BL to a Thumb function of the
 * core into BLX but puts the halfword bit into bit 25 instead of bit 24 (tcc/tccelf.c, relocate_section, R_ARM_CALL: x |= h << 24):
 * a core function at an address = 2 mod 4 is entered 2 bytes early. isspace is such a function in the R6 builds (1e6f618de camera
 * build: 0x1A110A; strtof calls it for every float literal of a script). With these two copies dietlibc's calls stay inside lua.mo
 * (ARM to ARM, no BLX); the stage gate checks that no BL of a module reaches a Thumb function of the core. */
int isspace(int c) { return c == 32 || (c >= 9 && c <= 13); }   /* space, \t \n \v \f \r */
int isalnum(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* ======== 5.6 F1: file descriptors ======== */
struct lua_r6_fd { volatile uint32_t h; uint32_t size; };
static struct lua_r6_fd lua_r6_fdt[LUA_R6_NFD];

int lua_r6_fd_add(uint32_t h, uint32_t size)
{
    for (int i = 0; h && i < LUA_R6_NFD; i++)
    {
        if (r6_cas32(&lua_r6_fdt[i].h, 0, h))
        {
            lua_r6_fdt[i].size = size;
            return i + LUA_R6_FD0;
        }
    }
    return -1;
}

uint32_t lua_r6_fd_h(int fd)
{
    unsigned i = fd - LUA_R6_FD0;
    return i < LUA_R6_NFD ? lua_r6_fdt[i].h : 0;
}

uint32_t lua_r6_fd_size(int fd)
{
    unsigned i = fd - LUA_R6_FD0;
    return i < LUA_R6_NFD && lua_r6_fdt[i].h ? lua_r6_fdt[i].size : 0;
}

uint32_t lua_r6_fd_del(int fd)
{
    unsigned i = fd - LUA_R6_FD0;
    if (i >= LUA_R6_NFD) return 0;
    uint32_t h = lua_r6_fdt[i].h;
    if (!h) return 0;
    lua_r6_fdt[i].size = 0;
    r6_barrier();
    lua_r6_fdt[i].h = 0;
    return h;
}

/* ======== M16.1 (spec m16/m161; CONFIG_R6_LUA) ======== */

uint32_t lua_r6_tid(void) { return get_current_task_id(); }

/* ---- 2.7 (R-13): every wait with the script's semaphore given back ---- */
int lua_r6_wait(lua_State * L, int (*fn)(void *), void * arg)
{
    /* only a waiting task gives it back: a GUI task never waits (2.2), PropMgr / r6_iv_task get R6API_TASK from the facade; another
     * task of this script in task.yield, key.wait or such a wait (cant_yield): the semaphore stays held (F10). Review R2-3 (the same
     * class): a task without an anchor (a callback in clock_task, lua_prop_task, shoot_task) keeps it too: its take after the wait
     * would park that task for good if the script died meanwhile */
    if (r6_script_task_class(0) != R6TC_SYNC || lua_get_cant_yield(L) || !lua_r6_may_wait(L)) return fn(arg);
    int t1 = lua_gettop(L);
    lua_set_cant_yield(L, 1);
    lua_give_semaphore(L, NULL);
    int r = fn(arg);
    lua_take_semaphore(L, 0, NULL);          /* a dead script: lua_r6_park (Ue3) */
    lua_set_cant_yield(L, 0);
    if (lua_gettop(L) != t1) luaL_error(L, "Lua stack changed while waiting (another task of this script)");
    return r;
}

static int lua_r6_do_fn(void * p)
{
    struct lua_r6_call * c = p;
    int n = sizeof(c->why);
    switch (c->op)
    {
        case LR6_SHOOT:    c->r = r6_take_a_pic_ex(c->gen, c->a, 0, &c->seq, c->why, n); break;
        case LR6_BURST:    c->r = r6_script_burst(c->gen, c->a, &c->done, c->why, n); break;
        case LR6_BULB:     c->r = r6_script_bulb(c->gen, c->a, c->why, n); break;
        case LR6_WAITJOBS: c->r = r6_script_wait_jobs(c->a); break;
        case LR6_TV:       c->r = r6_script_tv(c->gen, c->a, c->why, n); break;
        case LR6_CMD:      c->r = r6_script_cmd(c->gen, c->a, c->b, c->why, n); break;
        case LR6_GUI:      c->r = r6_script_gui(c->gen, c->a, c->b, c->c, c->why, n); break;
        case LR6_MENUSET:  c->r = r6_script_menu_set(c->gen, c->s1, c->s2, c->s3, c->a, c->why, n); break;
        case LR6_LVWAIT:   c->r = wait_lv_frames(c->a) ? R6API_OK : R6API_TIMEOUT; break;
        default:           c->r = R6API_ARG; snprintf(c->why, n, "unknown call"); break;
    }
    return c->r;
}

int lua_r6_do(lua_State * L, struct lua_r6_call * c)
{
    c->gen = lua_r6_gen(L);
    c->why[0] = 0;
    return lua_r6_wait(L, lua_r6_do_fn, c);
}

/* ---- Ue3: the anchors of the Lua tasks ---- */
#define LUA_R6_NTA 16
static struct lua_r6_tanchor * volatile lua_r6_ta[LUA_R6_NTA];
static volatile uint32_t lua_r6_ta_tid[LUA_R6_NTA];

int lua_r6_tanchor_set(struct lua_r6_tanchor * a, lua_State * L)
{
    uint32_t tid = get_current_task_id();
    a->L = L;
    for (int i = 0; tid && i < LUA_R6_NTA; i++)
    {
        if (r6_cas32(&lua_r6_ta_tid[i], 0, tid))
        {
            lua_r6_ta[i] = a;
            r6_barrier();
            return 1;
        }
    }
    return 0;
}

void lua_r6_tanchor_clear(struct lua_r6_tanchor * a)
{
    for (int i = 0; i < LUA_R6_NTA; i++)
    {
        if (lua_r6_ta[i] == a)
        {
            lua_r6_ta[i] = 0;
            r6_barrier();
            lua_r6_ta_tid[i] = 0;
            return;
        }
    }
}

static struct lua_r6_tanchor * lua_r6_tanchor_find(void)
{
    uint32_t tid = get_current_task_id();
    for (int i = 0; tid && i < LUA_R6_NTA; i++)
    {
        struct lua_r6_tanchor * a = lua_r6_ta[i];
        if (lua_r6_ta_tid[i] == tid && a) return a;
    }
    return 0;
}

/* review R2-3: 1 = this task may wait with the semaphore given back: an anchor of L ends the wait if the script dies meanwhile (the
 * main chunk: load_script's anchor; a task.create task: its task anchor); a callback (GuiMainTask, menu_task, clock_task, lua_prop_task,
 * shoot_task) has none and would park for good */
int lua_r6_may_wait(lua_State * L)
{
    struct lua_r6_tanchor * t = lua_r6_tanchor_find();
    if (t) return t->L == L;
    struct lua_r6_anchor * a = lua_r6_anchor_find();
    return a && a->L == L;
}

/* review R2-8: umm_init once across both cores (umm_malloc.c): 0 free, 1 building, 2 built; the loser of the CAS finds umm_heap NULL */
static volatile uint32_t lua_r6_umm_once = 0;
int lua_r6_umm_once_begin(void) { return r6_cas32(&lua_r6_umm_once, 0, 1); }
void lua_r6_umm_once_end(int built)
{
    r6_barrier();                                    /* umm_heap (published by umm_init) before the state */
    lua_r6_umm_once = built ? 2 : 0;                 /* no block: a later call tries again */
}

/* ---- Ue4 (SB-01, SL-12): the umm critical section. umm's macros nest (_umm_realloc calls _umm_free / _umm_malloc inside it):
 * cli, then the owner word: 0 = free, else the core's id + 1. The same core nests (depth + 1); the other core spins until the owner
 * lets go (it holds the lock for one umm operation, nothing in it blocks). EXIT: depth - 1, at 0 a barrier and the owner 0, then sei ---- */
static volatile uint32_t lua_r6_umm_own = 0;
static volatile uint32_t lua_r6_umm_depth = 0;
_Static_assert(sizeof(lua_r6_umm_own) == 4, "the umm owner word is one word for r6_cas32");
#define LUA_R6_UMM_FREE 0u
_Static_assert(LUA_R6_UMM_FREE == 0u, "a .bss word 0 must mean free (SL-12: never 'CPU0 owns it')");

uint32_t lua_r6_umm_enter(void)
{
    uint32_t old = cli();
    uint32_t me = (uint32_t) r6_cpu_id() + 1;
    if (lua_r6_umm_own == me)
    {
        uint32_t d = ++lua_r6_umm_depth;
        if (d > r6_lua_stat()->umm_depth_max) r6_lua_stat()->umm_depth_max = d;
        return old;
    }
    if (!r6_cas32(&lua_r6_umm_own, LUA_R6_UMM_FREE, me))
    {
        struct r6_lua_stat * st = r6_lua_stat();
        do st->umm_spin++; while (!r6_cas32(&lua_r6_umm_own, LUA_R6_UMM_FREE, me));
    }
    lua_r6_umm_depth = 1;
    if (1 > r6_lua_stat()->umm_depth_max) r6_lua_stat()->umm_depth_max = 1;
    return old;
}

void lua_r6_umm_exit(uint32_t old)
{
    if (lua_r6_umm_depth && --lua_r6_umm_depth == 0)
    {
        r6_barrier();
        lua_r6_umm_own = LUA_R6_UMM_FREE;
    }
    sei(old);
}

/* ---- 5.6: the library "r6" ---- */
static const struct { const char * name; int cmd; } lua_r6_cmds[] = {
    { "iv_start", R6SC_IV_START }, { "iv_stop", R6SC_IV_STOP }, { "iv_test", R6SC_IV_TEST }, { "tv_writes", R6SC_TVW },
    { "tv_back", R6SC_TVBACK }, { "md_start", R6SC_MD_START }, { "md_stop", R6SC_MD_STOP }, { "ettr_apply", R6SC_ETTR_APPLY },
    { "astro_mode", R6SC_AST_MODE }, { "astro_test", R6SC_AST_TEST },
};

/* r6.cmd(name, arg) -> ok, text, code */
static int luaCB_r6_cmd(lua_State * L)
{
    LUA_PARAM_STRING(name, 1);
    int arg = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : (int) luaL_optinteger(L, 2, 0);
    int cmd = -1;
    for (unsigned i = 0; i < sizeof(lua_r6_cmds) / sizeof(lua_r6_cmds[0]); i++)
        if (!strcmp(name, lua_r6_cmds[i].name)) cmd = lua_r6_cmds[i].cmd;
    if (cmd < 0) return luaL_error(L, "r6.cmd: unknown command '%s'", name);
    struct lua_r6_call c = { .op = LR6_CMD, .a = cmd, .b = arg };
    int r = lua_r6_do(L, &c);
    if (r == R6API_OK && cmd == R6SC_TVW) lua_r6_flag(L, LUA_R6F_TVW, arg != 0);
    lua_pushboolean(L, r == R6API_OK);
    lua_pushstring(L, c.why);
    lua_pushinteger(L, r);
    return 3;
}

static void lua_r6_setint(lua_State * L, const char * k, int v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); }
static void lua_r6_setbool(lua_State * L, const char * k, int v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); }
static void lua_r6_setstr(lua_State * L, const char * k, const char * v) { lua_pushstring(L, v); lua_setfield(L, -2, k); }

/* r6.last_shot() -> {seq, res, text, t, t_req, gen, async, cancelled}, or nil and "none" / "busy" (SL-13) */
static int luaCB_r6_last_shot(lua_State * L)
{
    struct r6_shot_res r;
    int k = r6_last_shot(&r);
    if (k <= 0)
    {
        lua_pushnil(L);
        lua_pushstring(L, k < 0 ? "busy" : "none");
        return 2;
    }
    lua_newtable(L);
    lua_r6_setint(L, "seq", r.seq);
    lua_r6_setint(L, "res", r.res);
    lua_r6_setstr(L, "text", r.why);
    lua_r6_setint(L, "t", r.t_done);
    lua_r6_setint(L, "t_req", r.t_req);
    lua_r6_setint(L, "gen", r.gen);
    lua_r6_setbool(L, "async", r.async);
    lua_r6_setbool(L, "cancelled", r.cancelled);
    return 1;
}

/* r6.expo() -> {tv, av, iso, ec} in Code768 (iso: Canon's Sv768), or nil */
static int luaCB_r6_expo(lua_State * L)
{
    struct r6_expo e;
    if (!r6_script_expo(&e)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_r6_setint(L, "tv", e.tv);
    lua_r6_setint(L, "av", e.av);
    lua_r6_setint(L, "iso", e.sv);
    lua_r6_setint(L, "ec", e.comp);
    return 1;
}

/* r6.sysmem() -> {ok, why, largest, free, used, peak} (C2: ok false with the reason, no zeros as values) */
static int luaCB_r6_sysmem(lua_State * L)
{
    struct r6_sysmem m;
    int ok = r6_script_sysmem(&m);
    lua_newtable(L);
    lua_r6_setbool(L, "ok", ok);
    lua_r6_setstr(L, "why", m.why);
    if (ok)
    {
        lua_r6_setint(L, "largest", m.largest);
        lua_r6_setint(L, "free", m.free);
        lua_r6_setint(L, "used", m.used);
        lua_r6_setint(L, "peak", m.peak);
    }
    return 1;
}

/* r6.tasks() -> {created, mem, unknown, busy, arg, ended, ring = { {name, prio, stack, res, largest, min, t}, ... }} */
static int luaCB_r6_tasks(lua_State * L)
{
    uint32_t c[6];
    r6_task_counts(c);
    lua_newtable(L);
    lua_r6_setint(L, "created", c[0]);
    lua_r6_setint(L, "mem", c[1]);
    lua_r6_setint(L, "unknown", c[2]);
    lua_r6_setint(L, "busy", c[3]);
    lua_r6_setint(L, "arg", c[4]);
    lua_r6_setint(L, "ended", c[5]);
    lua_newtable(L);
    struct r6tk_dec d;
    int n = r6_task_ring(0, &d);
    for (int i = 0; i < n && i < R6TK_RING; i++)
    {
        if (r6_task_ring(i, &d) <= i) break;
        char nm[17];
        memcpy(nm, d.name, 16);
        nm[16] = 0;
        lua_newtable(L);
        lua_r6_setstr(L, "name", nm);
        lua_r6_setint(L, "prio", d.prio);
        lua_r6_setint(L, "stack", d.stack);
        lua_r6_setint(L, "res", d.res);
        lua_r6_setint(L, "largest", d.largest);
        lua_r6_setint(L, "min", d.min);
        lua_r6_setint(L, "t", d.t_ms);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "ring");
    return 1;
}

static const luaL_Reg r6lib[] =
{
    { "cmd", luaCB_r6_cmd },
    { "last_shot", luaCB_r6_last_shot },
    { "expo", luaCB_r6_expo },
    { "sysmem", luaCB_r6_sysmem },
    { "tasks", luaCB_r6_tasks },
    { NULL, NULL }
};

int luaopen_r6(lua_State * L)
{
    lua_newtable(L);
    luaL_setfuncs(L, r6lib, 0);
    lua_pushinteger(L, R6API_VER);
    lua_setfield(L, -2, "version");
    return 1;
}

#endif /* CONFIG_R6_LUA */
