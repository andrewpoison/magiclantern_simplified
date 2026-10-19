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

void lua_r6_park(const char * why)
{
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
    if (left < 0) return;
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

#endif /* CONFIG_R6_LUA */
