/*
 * R6.190 M12.0: "Astro red", mode "ML only", and the read-only display probe "Tor 0f" (docs/m12_plan.md Rev. 2,
 * spec m12/m120 spec_m120.md Rev. 2; user answers of 2026-10-11: E1 a M12.0 first, E2 Tor 0f MMIO read only, E5 pure
 * red, E6 levels 100/50/25/12/150/200 % per output, E8 c saved / restored at start (for "ML only": no Canon access),
 * E9 ML's palette by meaning, E13 a only get_cpu_id() checks, no task_create_ex).
 *
 * Display > "Astro red":
 *  - Mode Off / "ML only" (saved). "ML only" maps ML's own palette (r6_layer.c r6l_build_lut, once per conversion)
 *    to pure red by meaning (text 200, errors 255, warnings 230..245, the selection bar 75, grey ramp 0.75 x g, ...;
 *    level per output: R = min(255, max(2, (R100 * level + 50) / 100)), G = B = 0). Transparent entries (0, 3) stay
 *    exact (the conversion's word test), black stays 0xFF010101 (Canon's colour key is 0xFF000000: never produced),
 *    the masks 26 / 27 keep their alpha and G = B = 1, the grid line 28 keeps its alpha 0x99. Canon's display stays in
 *    colour (M12.1 adds "Full"). Only ML's pixels change; nothing of Canon's is written.
 *  - LCD level / EVF level (saved, default 50 %): the level of the output shown (display mask 0x10: EVF).
 *  - The transparent ML menu view (src/menu.c, lv only) is switched off within one task step while the mode is on
 *    (ML functions only; src/menu.c stays unchanged).
 *
 * Debug > LV probe (lvprobe.c): "Display regs" (not saved, OFF at boot) and "Key test".
 *  - Tor 0f (E2, approved 2026-10-11): r6as_mmio_rd is the only MMIO access of this file and of ML's display probe.
 *    Per output n (LCD n = 0 at 0xD282xxxx, EVF n = 1 at 0xD283xxxx) the status word +0x3000 at any time; the 13 words
 *    +0x301C, +0xA010, +0xA014, +0xA018, +0xB020, +0xB040, +0xB044, +0xB048..+0xB05C only while the status of the same
 *    output shows the device clock (& 7) and the output (& 0x30), Canon's display mask selects exactly this output,
 *    Canon's display tasks were found on CPU0 (r6as_core_ok, spec 1.1 / 3.5) and this task runs on CPU0 (get_cpu_id),
 *    all in one IRQ / FIQ lock; single 32-bit loads, never +0x3010, never +0x3020..+0x3034, never HDMI 0xD280xxxx,
 *    never Hrady0. No MMIO write of any kind, no MMIO at all once ML shuts down.
 *  - Rate: the astro task reads both outputs every R6AS_TICK_MS (250 ms, about 4 / s) while "Display regs" is ON; a
 *    change of the display mask, a status edge or a changed +0xB048 starts a transition trace: every 20 ms (50 / s)
 *    for exactly 2 s, never extended, at most R6AS_TR_MAXSMP samples; a new trace only after one normal tick without a
 *    trace and at the earliest R6AS_TR_GAP_MS (2 s) after the previous one ended (review R1 L1: on average at most about
 *    27 reads / s per output, whatever keeps triggering). A status word 0xFFFFFFFF counts as implausible: status only
 *    (review R1 L2, ast.cnt st_bad).
 *  - The log: value sets, status words, counters, the trace ring (heap, allocated at the first ON, never freed), the
 *    latch words and display state in Canon RAM, the task table entries of DispDCtrl / SubDispT (name pointers compared
 *    as numbers, never dereferenced) - all read only, every pointer checked.
 *  - "Key test": for 10 s two 40x40 squares in ML's pixels with the palette values 0xFF010000 / 0xFF020000 (indices
 *    29 / 30, unused by ML), on the LCD shooting screen; the user notes whether Canon's mixer shows them dark.
 *
 * Canon calls: none new. ML functions and the existing stubs msleep, vsnprintf (the AST text), Acquire- /
 * ReleaseRecursiveLock (BMP_LOCK, as r6h_task / r6_gd_erase). Not exported to modules.
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "property.h"
#include "propvalues.h"
#include "tasks.h"
#include <stdarg.h>
#include "r6_faceyuv.h"
#include "r6_gdraw.h"
#include "r6_astro.h"

#ifdef CONFIG_R6_ASTRO

#if !defined(CONFIG_R6_LAYER_OWNERSHIP) || !defined(CONFIG_R6_GLOBAL_DRAW) || !defined(FEATURE_LV_BUFFER_PROBE)
#error "r6_astro.c: needs the R6 layer ownership, CONFIG_R6_GLOBAL_DRAW and the LV probe"
#endif

/* ---- settings (E8 c: saved; "ML only" needs no Canon access, so it is on again right at boot) ---- */
static CONFIG_INT("r6.astro.mode", r6as_mode, 0);    /* 0 Off, 1 ML only (E8 c: saved; ML only needs no Canon access) */
static CONFIG_INT("r6.astro.lcd",  r6as_lcd, 1);     /* index into r6as_lvl_pct: 1 = 50 % (E6) */
static CONFIG_INT("r6.astro.evf",  r6as_evf, 1);
static const uint8_t __attribute__((section(".data"))) r6as_lvl_pct[6] = { 100, 50, 25, 12, 150, 200 };   /* .data, see r6as_addr */

#define R6AS_PAL_VER     1          /* the palette below (log ast.cfg) */
#define R6AS_PAL_RED     1          /* r6_astro_pal: bit 0 "ML only" */
#define R6AS_PAL_KEY     2          /*               bit 1 colour-key test */

/* R100: the red level of each palette index at 100 % (spec 2.2; Rev. 2 F8, implementation: the selection bars 9 / 11
 * at 75, see impl_notes: with the real grey ramp R(60) = 107 the four contrast rules hold at every level with 75, not
 * with 80); 0 for the entries that stay exact (0 / 3 transparent, 2 black, 26 / 27 masks) */
static const uint8_t __attribute__((section(".data"))) r6as_r100[80] = {                              /* .data, see r6as_addr */
      0, 200,   0,   0,  90, 170, 150, 150, 255,  75,     /*  0..9: text 1, black 2, unknown 4, hint 5, ON 6/7, error 8, bar 9 */
    120,  75, 230,  60, 140, 235, 140, 190,  60, 245,     /* 10..19: bar 11 (MENU_BAR_COLOR), warning 12/15/19 */
     70,  70,  70,  70,  70,  70,   0,   0, 200, 140,     /* 20..29: dark variants, masks 26/27, grid line 28 (alpha kept) */
    140, 140, 140, 140, 140, 140, 140, 140,   7,  14,     /* 30..39: placeholders, grey ramp from 38: max(2, (3 g + 2) / 4) */
     20,  27,  31,  35,  38,  41,  44,  48,  52,  55,     /* 40..49 (42 = header / footer) */
     62,  69,  76,  83,  88,  89,  93,  97, 100, 104,     /* 50..59 (54 = OFF) */
    107, 110, 114, 117, 121, 124, 128, 131, 134, 138,     /* 60..69 (60 = MENU_FONT_GRAY of a selected entry) */
    141, 145, 149, 152, 155, 158, 162, 166, 169, 173,     /* 70..79 */
};

/* ---- Tor 0f (E2): the probe ---- */
#define R6AS_NW          13
#define R6AS_DISP_CPU    0          /* spec m12/m120 1: DispDCtrl, SubDispT and the VD ISRs run on CPU0 */
#define R6AS_DISPDEV_PTR 0xEA40     /* 0xE01694C8: display mask = [[0xEA40]+0x18] */
/* Tor 0f (E2, approved 2026-10-11): the only MMIO addresses of ML's display probe, read only. Index 0 = status word. */
/* the only table of this file in .rodata (the other tables are put into .data): GCC's section anchor of .rodata is then
 * this table itself, and only r6as_mmio_rd loads its address (astro_gate.sh point 5) */
static const uint32_t r6as_addr[2][1 + R6AS_NW] = {
    { 0xD2823000, 0xD282301C, 0xD282A010, 0xD282A014, 0xD282A018, 0xD282B020, 0xD282B040,
      0xD282B044, 0xD282B048, 0xD282B04C, 0xD282B050, 0xD282B054, 0xD282B058, 0xD282B05C },
    { 0xD2833000, 0xD283301C, 0xD283A010, 0xD283A014, 0xD283A018, 0xD283B020, 0xD283B040,
      0xD283B044, 0xD283B048, 0xD283B04C, 0xD283B050, 0xD283B054, 0xD283B058, 0xD283B05C },
};
struct r6as_rd { uint32_t st, dm; uint8_t cpu, got, pad[2]; uint32_t w[R6AS_NW]; };
#define R6AS_W_B048      7          /* w[i] = the word at r6as_addr[n][1 + i]: B048 is w[7], B058 w[11] */
#define R6AS_W_B058      11
enum { R6AS_GOT_OFF = 0, R6AS_GOT_ALL = 1, R6AS_GOT_CORE = 2 };   /* status only (off / not selected / core unknown), all 14, wrong core */
/* Rev. 2 (F2): 1 = DispDCtrl and SubDispT found in Canon's task table with Task+0x50 == R6AS_DISP_CPU (r6as_task, 3.5) */
static volatile uint8_t r6as_core_ok = 0;
extern int ml_shutdown_requested;

static int __attribute__((noinline, noipa)) r6as_mmio_rd(int n, struct r6as_rd * r)
{
    if ((unsigned) n > 1 || !r) return -1;
    if (ml_shutdown_requested) return -1;                    /* Rev. 2 (F10): no MMIO at all once ML shuts down */
    const volatile uint32_t * a = r6as_addr[n];              /* volatile: every address is loaded from the table */
    const uint32_t sel = n ? 0x10 : 0x01;                    /* the display mask that selects this output */
    for (int i = 0; i < R6AS_NW; i++) ((volatile uint32_t *) r->w)[i] = 0;   /* Rev. 2: no stale words; volatile: no memset call */
    uint32_t old = cli();
    uint32_t s = *(volatile uint32_t *) a[0];                /* status word: readable at any time (S1 P7) */
    uint32_t dd = *(volatile uint32_t *) R6AS_DISPDEV_PTR, dm = 0xFFFFFFFF;
    if (dd >= 0x1000 && dd <= 0x40000000 - 0x1C && !(dd & 3)) dm = *(volatile uint32_t *) (dd + 0x18);
    uint32_t cpu = get_cpu_id();
    int got = R6AS_GOT_OFF;
    if ((s & 7) && (s & 0x30) && s != 0xFFFFFFFF && dm == sel && r6as_core_ok)   /* clock on, output on, plausible (R1 L2), shown, core known */
    {
        if (cpu == R6AS_DISP_CPU)
        {
            for (int i = 0; i < R6AS_NW; i++) r->w[i] = *(volatile uint32_t *) a[1 + i];
            got = R6AS_GOT_ALL;
        }
        else got = R6AS_GOT_CORE;
    }
    asm volatile ("dsb" ::: "memory");
    sei(old);
    r->st = s; r->dm = dm; r->cpu = cpu; r->got = got;
    return got;
}

/* ---- timing (QEMU: variables, qcheck.sh checks these camera values) ---- */
#define R6AS_STEP_MS          50     /* task step (transparent-menu block, key test, palette change, display mask) */
#define R6AS_TICK_MS          250    /* E2: about 4 reads / s per output */
#define R6AS_TR_STEP_MS       20     /* E2: the transition trace 50 / s ... */
#define R6AS_TR_LEN_MS        2000   /* ... for 2 s after its trigger, never extended (Rev. 2 F1) */
#define R6AS_TR_GAP_MS        2000   /* review R1 L1: a new trace at the earliest 2 s after the previous one ended */
#define R6AS_TR_MAXSMP        201    /* review R1 L1: a trace ends after at most 2 x 2000 / 20 + 1 samples, whatever the clock does */
#define R6AS_KEY_MS           10000  /* the colour-key squares are shown 10 s */
#define R6AS_KEY_WAIT_MS      20000  /* ... after the LCD shooting screen held 500 ms, waited for at most 20 s */
#define R6AS_KEY_HOLD_MS      500
#define R6AS_CORE_RECHECK_MS  10000  /* Rev. 2 (F2): the task table check before the first read and every 10 s */

#define R6AS_VS_N    6               /* value sets per output */
#define R6AS_ST_N    8               /* status words per output */
#define R6AS_SW_N    6               /* trace ring */
#define R6AS_SW_PTS  12              /* change points per trace */

/* Canon RAM, read only (existing read approval, spec 3.5) */
#define R6AS_TASK_TAB    0x40E0      /* task table pointer, 0x5C per task (tasks.h CONFIG_TASK_STRUCT_V2_SMP) */
#define R6AS_TASK_NUM    200         /* Rev. 2: ids up to 199 */
#define R6AS_TASK_SIZE   0x5C
#define R6AS_CUR_TASK    0x1028      /* current_task */
#define R6AS_NAME_DISP   0xE016A66C  /* "DispDCtrl" (task_create via CreateTaskClass 0xE07C0A4C in 0xE016A47E), compared as a number */
#define R6AS_NAME_SUB    0xE0182D80  /* "SubDispT" (adr before task_create at 0xE0182C76), compared as a number */
#define R6AS_L1615       0x1615C8    /* Hrady1 update mode per output (0xE020902C) */
#define R6AS_L2F76       0x2F76C     /* per output (0xE0209DCA) */
#define R6AS_L1E1B       0x1E1B4     /* [0x1E1B4 + 4 ([0x2F76C + 4 n] != 0)] */
#define R6AS_IRQ_1C6     0xFBCA4     /* IRQ 0x1C6 core mask word, bits 12..13 (K12, meaning assumed) */
#define R6AS_IRQ_1E6     0xFBCAC     /* IRQ 0x1E6 */
#define R6AS_MARV_L1     0x23868A00  /* layer 1 MARV (plan 5.1): +4 bitmap, +0x1C width, +0x20 height */

static const uint32_t __attribute__((section(".data"))) r6as_std[6] = {   /* .data, see r6as_addr */ 0x7A7E1000, 0x00007493, 0x1C5A1000, 0x00000000, 0x00001000, 0x0000166F };   /* B048..B05C */

static int r6as_rd32(uint32_t a, uint32_t * v)
{
    if (!r6_fy_ram_ok(a, 4)) { *v = 0; return 0; }
    *v = *(volatile uint32_t *) a;
    return 1;
}
static uint32_t r6as_ram(uint32_t a) { uint32_t v; r6as_rd32(a, &v); return v; }
static uint32_t r6as_byte(uint32_t a)
{
    uint32_t v;
    if (!r6as_rd32(a & ~3u, &v)) return 0xFFFFFFFF;
    return (v >> (8 * (a & 3))) & 0xFF;
}

/* ---- the probe's tables (heap: allocated at the first "Display regs" ON, never freed; spec 0.4) ---- */
struct r6as_vs { uint32_t t_first, t_last, count, gui, dm; uint32_t w[R6AS_NW + 3]; };
struct r6as_st { uint32_t v, count, t_first, t_last; };
struct r6as_pt { uint16_t dt; uint8_t got0, got1; uint32_t st0, st1, b048_0, b058_0, b048_1, b058_1; };
struct r6as_sw { uint32_t t, t_dm, old, nw; uint16_t dur, nsmp, nchg; int16_t on0, on1; uint8_t trig, ext, npts, pad; struct r6as_pt p[R6AS_SW_PTS]; };
struct r6as_tabs {
    struct r6as_vs vs[2][R6AS_VS_N];
    struct r6as_st st[2][R6AS_ST_N];
    struct r6as_sw sw[R6AS_SW_N];
    uint8_t nvs[2], nst[2];
};
struct r6as_cnt { uint32_t ticks, rd, rd_all, rd_off, on_unsel, core_bad, core_unk, core_lost, tr_off, sw_n, sw_ext, sw_skip, vs_ovf, st_ovf, st_bad; };
struct r6as_tk { int n; uint32_t id, req, asg, state; };

static struct r6as_tabs * T = 0;
static struct r6as_cnt C;
static volatile uint32_t r6as_seq = 0;           /* odd while the astro task writes T / C */
static volatile int r6as_probe = 0;              /* "Display regs" (not saved) */
static volatile int r6as_probe_new = 0;          /* switched ON: the task starts again (core check first) */
static const char * volatile r6as_probe_txt = "";   /* "no memory" or "" */
static uint32_t r6as_lat[6];                     /* latch words of the last tick */
static volatile uint32_t r6as_my_cpu = 0xFF, r6as_my_req = 0xFFFFFFFF, r6as_my_asg = 0xFFFFFFFF;   /* the astro task */

/* key test (2.5) */
enum { R6AS_K_IDLE = 0, R6AS_K_WAIT, R6AS_K_SHOW };
static volatile int r6as_key = R6AS_K_IDLE;
static const char * volatile r6as_key_txt = "";

static int r6as_cfg(int v, int hi) { return COERCE(v, 0, hi); }

/* ---- palette (2.2 / 2.3) ---- */
uint32_t r6_astro_pal(void)
{
    int mode = r6as_cfg(r6as_mode, 1), key = r6as_key == R6AS_K_SHOW;
    if (!mode && !key) return 0;
    int lvl = r6_gd_disp_mask() == 0x10 ? r6as_cfg(r6as_evf, 5) : r6as_cfg(r6as_lcd, 5);
    return (mode ? R6AS_PAL_RED : 0) | (key ? R6AS_PAL_KEY : 0) | ((uint32_t) r6as_lvl_pct[lvl] << 8);
}

uint32_t r6_astro_map(uint32_t pal, int i, uint32_t v)
{
    if ((pal & R6AS_PAL_KEY) && (i == 29 || i == 30))        /* the colour-key test (2.5), any mode and level */
        return i == 29 ? 0xFF010000 : 0xFF020000;
    if (!(pal & R6AS_PAL_RED)) return v;
    uint32_t a = v & 0xFF000000;
    if (!a || i == 2 || i == 26 || i == 27) return v;        /* transparent, black (never 0xFF000000), the masks */
    int k = (i >= 0 && i < 80) ? i : 4;                      /* indexed2rgb: index >= 80 shows entry 4 */
    uint32_t r = (r6as_r100[k] * ((pal >> 8) & 0xFF) + 50) / 100;
    if (r > 255) r = 255;
    if (r < 2) r = 2;
    return a | (r << 16);
}

/* ---- the tables ---- */
static void r6as_w_begin(void) { r6as_seq++; __sync_synchronize(); }
static void r6as_w_end(void) { __sync_synchronize(); r6as_seq++; }

static void r6as_count(int n, const struct r6as_rd * r, int got)
{
    C.rd++;
    if (got == R6AS_GOT_ALL) C.rd_all++;
    else if (got == R6AS_GOT_CORE) C.core_bad++;
    else if (r->st == 0xFFFFFFFF) C.st_bad++;                 /* review R1 L2: implausible status word, the words not read */
    else if (!((r->st & 7) && (r->st & 0x30))) C.rd_off++;
    else if (r->dm != (n ? 0x10u : 0x01u)) C.on_unsel++;
    else C.core_unk++;
}

static void r6as_st_add(int n, uint32_t v, int now)
{
    struct r6as_st * s = T->st[n];
    for (int k = 0; k < T->nst[n]; k++)
        if (s[k].v == v) { s[k].count++; s[k].t_last = now; return; }
    if (T->nst[n] >= R6AS_ST_N) { C.st_ovf++; return; }
    s = &s[T->nst[n]++];
    s->v = v; s->count = 1; s->t_first = s->t_last = now;
}

static void r6as_vs_add(int n, const struct r6as_rd * r, int now)
{
    uint32_t w[R6AS_NW + 3];
    for (int i = 0; i < R6AS_NW; i++) w[i] = r->w[i];
    w[R6AS_NW] = r6as_lat[n]; w[R6AS_NW + 1] = r6as_lat[2 + n]; w[R6AS_NW + 2] = r6as_lat[4 + n];
    struct r6as_vs * s = T->vs[n];
    for (int k = 0; k < T->nvs[n]; k++)
        if (!memcmp(s[k].w, w, sizeof(w))) { s[k].count++; s[k].t_last = now; return; }
    if (T->nvs[n] >= R6AS_VS_N) { C.vs_ovf++; return; }
    s = &s[T->nvs[n]++];
    memcpy(s->w, w, sizeof(w));
    s->count = 1; s->t_first = s->t_last = now; s->gui = CURRENT_GUI_MODE; s->dm = r->dm;
}

/* latch words (3.5): [0x1615C8 + 4 n], [0x2F76C + 4 n], [0x1E1B4 + 4 ([0x2F76C + 4 n] != 0)] */
static void r6as_latch(void)
{
    for (int n = 0; n < 2; n++)
    {
        uint32_t l2 = r6as_ram(R6AS_L2F76 + 4 * n);
        r6as_lat[n] = r6as_ram(R6AS_L1615 + 4 * n);
        r6as_lat[2 + n] = l2;
        r6as_lat[4 + n] = r6as_ram(R6AS_L1E1B + 4 * (l2 != 0));
    }
}

/* Canon's task table (3.5, Rev. 2 F2): DispDCtrl and SubDispT by their name pointer (a number, never dereferenced);
 * 1 = exactly one of each, both with Task+0x50 == R6AS_DISP_CPU */
static int r6as_task_core(struct r6as_tk * d, struct r6as_tk * s)
{
    memset(d, 0, sizeof(*d)); memset(s, 0, sizeof(*s));
    uint32_t b = r6as_ram(R6AS_TASK_TAB);
    if (!r6_fy_ram_ok(b, R6AS_TASK_NUM * R6AS_TASK_SIZE)) return 0;
    for (int i = 0; i < R6AS_TASK_NUM; i++)
    {
        uint32_t t = b + i * R6AS_TASK_SIZE, nm = *(volatile uint32_t *) (t + 0x24);
        struct r6as_tk * k = nm == R6AS_NAME_DISP ? d : nm == R6AS_NAME_SUB ? s : 0;
        if (!k || r6as_byte(t + 0x49) == 0xFF) continue;
        k->n++;
        k->id = *(volatile uint32_t *) (t + 0x40);
        k->req = r6as_byte(t + 0x50); k->asg = r6as_byte(t + 0x51); k->state = r6as_byte(t + 0x49);
    }
    return d->n == 1 && s->n == 1 && d->req == R6AS_DISP_CPU && s->req == R6AS_DISP_CPU;
}

/* ---- the trace (3.4) ---- */
static int r6as_tr = 0, r6as_tr_t0 = 0, r6as_tr_i = 0, r6as_need_tick = 0;
static int r6as_tr_end = 0;                      /* review R1 L1: the end of the last trace (the gap R6AS_TR_GAP_MS) */
static int r6as_t_dm = 0;
static uint32_t r6as_dm_last = 0;
static int r6as_have = 0;                        /* bit n: r6as_last_on / b048 of output n valid */
static int r6as_last_on[2];
static uint32_t r6as_b048[2];
static int r6as_b048_have[2];
static struct r6as_pt r6as_prev;
static int r6as_prev_have = 0;

static void r6as_trigger(int now, int trig, uint32_t old, uint32_t nw)
{
    if (r6as_tr) { T->sw[r6as_tr_i].ext++; C.sw_ext++; return; }   /* in a trace: counted only (never extended, F1) */
    if (r6as_need_tick || now - r6as_tr_end < R6AS_TR_GAP_MS) { C.sw_skip++; return; }   /* one normal tick and 2 s after a trace (R1 L1) */
    r6as_tr_i = C.sw_n % R6AS_SW_N;
    C.sw_n++;
    struct r6as_sw * e = &T->sw[r6as_tr_i];
    memset(e, 0, sizeof(*e));
    e->t = now; e->t_dm = r6as_t_dm; e->trig = trig; e->old = old; e->nw = nw;
    e->on0 = e->on1 = -1;
    r6as_tr = 1;
    r6as_tr_t0 = now;
    r6as_prev_have = 0;
}

/* one read of both outputs (tick or trace): counters, status table */
static void r6as_read2(int now, struct r6as_rd * r)
{
    for (int n = 0; n < 2; n++)
    {
        int g = r6as_mmio_rd(n, &r[n]);
        if (g < 0) { memset(&r[n], 0, sizeof(r[n])); continue; }
        r6as_count(n, &r[n], g);
        r6as_st_add(n, r[n].st, now);
    }
}

static void r6as_edges(int now, const struct r6as_rd * r)
{
    for (int n = 0; n < 2; n++)
    {
        int on = ((r[n].st & 7) ? 1 : 0) | ((r[n].st & 0x30) ? 2 : 0);
        if ((r6as_have & (1 << n)) && on != r6as_last_on[n]) r6as_trigger(now, 2 + n, r6as_last_on[n], on);
        r6as_last_on[n] = on;
        r6as_have |= 1 << n;
        if (r[n].got == R6AS_GOT_ALL)
        {
            if (r6as_b048_have[n] && r[n].w[R6AS_W_B048] != r6as_b048[n]) r6as_trigger(now, 4 + n, r6as_b048[n], r[n].w[R6AS_W_B048]);
            r6as_b048[n] = r[n].w[R6AS_W_B048];
            r6as_b048_have[n] = 1;
        }
    }
}

static void r6as_tr_sample(int now, const struct r6as_rd * r)
{
    struct r6as_sw * e = &T->sw[r6as_tr_i];
    int dt = now - (int) e->t;
    e->nsmp++;
    e->dur = MIN(dt, 0xFFFF);
    if (r[0].got == R6AS_GOT_ALL && e->on0 < 0) e->on0 = MIN(dt, 0x7FFF);
    if (r[1].got == R6AS_GOT_ALL && e->on1 < 0) e->on1 = MIN(dt, 0x7FFF);
    struct r6as_pt p = { .dt = MIN(dt, 0xFFFF), .got0 = r[0].got, .got1 = r[1].got, .st0 = r[0].st, .st1 = r[1].st,
                         .b048_0 = r[0].w[R6AS_W_B048], .b058_0 = r[0].w[R6AS_W_B058], .b048_1 = r[1].w[R6AS_W_B048], .b058_1 = r[1].w[R6AS_W_B058] };
    struct r6as_pt q = p;
    q.dt = 0;
    if (!r6as_prev_have || memcmp(&q, &r6as_prev, sizeof(q)))
    {
        if (e->npts < R6AS_SW_PTS) e->p[e->npts++] = p;
        else e->nchg++;
    }
    r6as_prev = q;
    r6as_prev_have = 1;
}

/* ---- the astro task (3.3) ---- */
static int r6as_t_tick = 0, r6as_t_core = 0, r6as_core_have = 0;
static uint8_t r6as_core_was = 0;
static int r6as_k_t0 = 0, r6as_k_ok = 0, r6as_k_show = 0, r6as_k_draw = 0;
static int r6as_k_vis = 0;                       /* review R2 L1 / R3 F2: the squares may be in ML's buffer */

static void r6as_key_squares(int on)
{
    BMP_LOCK(
        bmp_fill(on ? 29 : COLOR_EMPTY, 220, 220, 40, 40);
        bmp_fill(on ? 30 : COLOR_EMPTY, 460, 220, 40, 40);
    )
    ml_refresh_display_needed = 1;
}

static void r6as_key_step(int now)
{
    if (r6as_key == R6AS_K_WAIT)
    {
        if (lp_shooting(0x01)) { if (!r6as_k_ok) r6as_k_ok = now; }
        else r6as_k_ok = 0;
        if (r6as_k_ok && now - r6as_k_ok >= R6AS_KEY_HOLD_MS)
        {
            r6as_key = R6AS_K_SHOW;                          /* r6_astro_pal: bit 1 from now on */
            r6as_k_show = now;
            r6as_k_draw = 0;
            r6as_k_vis = 0;
            r6as_key_txt = "showing";
        }
        else if (now - r6as_k_t0 > R6AS_KEY_WAIT_MS)
        {
            r6as_key = R6AS_K_IDLE;
            r6as_key_txt = "no LCD screen";
        }
        return;
    }
    if (r6as_key != R6AS_K_SHOW) return;
    if (now - r6as_k_show >= R6AS_KEY_MS)
    {
        if (r6as_k_vis) r6as_key_squares(0);                 /* once (also under the ML menu: 29 / 30 lose their key colours now) */
        r6as_k_vis = 0;
        r6as_key = R6AS_K_IDLE;
        ml_refresh_display_needed = 1;
        r6as_key_txt = "done (10 s)";
        return;
    }
    if (lp_shooting(0x01))                                   /* the LCD shooting screen (never while the ML menu is shown) */
    {
        if (!r6as_k_vis || now - r6as_k_draw >= R6AS_KEY_HOLD_MS)
        {
            r6as_k_draw = now;
            r6as_k_vis = 1;
            r6as_key_squares(1);
        }
    }
    else if (r6as_k_vis && !gui_menu_shown())                /* another screen: erased once (R2 L1 / R3 F2); the ML menu draws */
    {                                                        /* its own pixels: not into it, once after it closes */
        r6as_k_vis = 0;
        r6as_key_squares(0);
    }
}

static void r6as_probe_step(int now)
{
    struct r6as_rd r[2];
    if (r6as_probe_new)
    {
        r6as_probe_new = 0;
        r6as_tr = 0; r6as_need_tick = 0; r6as_have = 0; r6as_b048_have[0] = r6as_b048_have[1] = 0;
        r6as_dm_last = r6_gd_disp_mask();
        r6as_t_dm = now;
        r6as_core_have = 0;
        r6as_t_tick = now - R6AS_TICK_MS;
        r6as_tr_end = now - R6AS_TR_GAP_MS;
    }
    if (!r6as_core_have || now - r6as_t_core >= R6AS_CORE_RECHECK_MS || now < r6as_t_core)
    {
        struct r6as_tk d, s;
        int ok = r6as_task_core(&d, &s);
        r6as_core_ok = ok;
        if (!r6as_probe || r6as_probe_new) r6as_core_ok = 0;   /* review R1 L3: OFF (or OFF + ON) during the scan wins */
        if (r6as_core_have && r6as_core_was && !r6as_core_ok) C.core_lost++;
        r6as_core_was = r6as_core_ok;
        r6as_core_have = 1;
        r6as_t_core = now;
    }
    r6as_w_begin();
    uint32_t dm = r6_gd_disp_mask();
    if (dm != r6as_dm_last)                                  /* Rev. 2 (F12): within one step (<= 50 ms) */
    {
        r6as_t_dm = now;
        r6as_trigger(now, 1, r6as_dm_last, dm);
        r6as_dm_last = dm;
    }
    if (r6as_tr)
    {
        /* exactly 2 s after the trigger (F1); review R1 L1: also at R6AS_TR_MAXSMP samples or if the clock went back */
        if (now - r6as_tr_t0 > R6AS_TR_LEN_MS || now < r6as_tr_t0 || T->sw[r6as_tr_i].nsmp >= R6AS_TR_MAXSMP)
        {
            r6as_tr = 0;
            r6as_need_tick = 1;
            r6as_t_tick = now;
            r6as_tr_end = now;
        }
        else
        {
            r6as_read2(now, r);
            r6as_edges(now, r);
            r6as_tr_sample(now, r);
        }
    }
    else if (now - r6as_t_tick >= R6AS_TICK_MS || now < r6as_t_tick)
    {
        int late = now - r6as_t_tick;                       /* about 4 / s on average (E2): the next tick on the grid */
        r6as_t_tick = (late >= 0 && late < 2 * R6AS_TICK_MS) ? r6as_t_tick + R6AS_TICK_MS : now;
        C.ticks++;
        r6as_latch();
        r6as_read2(now, r);
        for (int n = 0; n < 2; n++) if (r[n].got == R6AS_GOT_ALL) r6as_vs_add(n, &r[n], now);
        r6as_edges(now, r);
        if (r6as_tr) r6as_tr_sample(now, r);                 /* the trace's first sample: this read */
        else r6as_need_tick = 0;                             /* a normal tick without a trace */
    }
    r6as_w_end();
}


static void r6as_task(void * unused)
{
    uint32_t pal_last = 0;
    TASK_LOOP
    {
        msleep(r6as_probe && r6as_tr ? R6AS_TR_STEP_MS : R6AS_STEP_MS);
        int now = get_ms_clock();
        uint32_t ct = r6as_ram(R6AS_CUR_TASK);               /* the astro task itself (log ast.cpu) */
        r6as_my_cpu = get_cpu_id();
        if (r6_fy_ram_ok(ct, R6AS_TASK_SIZE)) { r6as_my_req = r6as_byte(ct + 0x50); r6as_my_asg = r6as_byte(ct + 0x51); }
        /* 2.4: the transparent ML menu view stays off while Astro red is on (ML functions only) */
        if (r6as_cfg(r6as_mode, 1) && menu_active_but_hidden())
        {
            menu_disable_lv_transparent_mode();
            menu_set_dirty();
            C.tr_off++;
        }
        r6as_key_step(now);
        uint32_t pal = r6_astro_pal();
        if (pal != pal_last) { pal_last = pal; ml_refresh_display_needed = 1; }   /* the next conversion maps anew */
        if (r6as_probe && T && !ml_shutdown_requested) r6as_probe_step(now);
        else if (!r6as_probe) r6as_tr = 0;
    }
}

TASK_CREATE("r6_astro_task", r6as_task, 0, 0x1e, 0x1000);

/* ---- menu interface (lvprobe.c) ---- */
void r6_astro_probe_enable(int on)
{
    if (on && !T)
    {
        struct r6as_tabs * t = malloc(sizeof(struct r6as_tabs));
        if (!t) { r6as_probe_txt = "no memory"; r6as_probe = 0; return; }
        memset(t, 0, sizeof(*t));
        T = t;
    }
    if (on && !r6as_probe) r6as_probe_new = 1;
    if (!on) r6as_core_ok = 0;                               /* F2: a new check before the next read */
    r6as_probe = on ? 1 : 0;
    r6as_probe_txt = "";
}
int r6_astro_probe_on(void) { return r6as_probe; }
const char * r6_astro_probe_msg(void) { return r6as_probe_txt; }

int r6_astro_key_start(void)
{
    if (r6as_key != R6AS_K_IDLE) { r6as_key_txt = "busy"; return 0; }
    r6as_k_t0 = get_ms_clock();
    r6as_k_ok = 0;
    r6as_key_txt = "waiting: LCD screen";
    r6as_key = R6AS_K_WAIT;
    return 1;
}
const char * r6_astro_key_msg(void) { return r6as_key_txt; }

/* ---- the AST text (4.2), snapshot capture: read only. Format of every line: spec m12/m120 4.2 (keys), numbers comma
 * separated, a 0x prefix marks a hex field ---- */
struct r6as_out { char * p; int len, cap, trunc; };
static void r6as_pf(struct r6as_out * o, const char * fmt, ...)
{
    if (o->len >= o->cap - 1) { o->trunc = 1; return; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    o->p[o->cap - 1] = 0;
    o->len += strlen(o->p + o->len);
    if (o->len >= o->cap - 1) o->trunc = 1;
}

/* "key=v0,v1,...\n"; bit i of hx: element i in hex */
static void r6as_arr(struct r6as_out * o, const char * key, const uint32_t * v, int n, uint32_t hx)
{
    r6as_pf(o, "%s=", key);
    for (int i = 0; i < n; i++) r6as_pf(o, (hx >> i) & 1 ? "%s0x%x" : "%s%d", i ? "," : "", (int) v[i]);
    r6as_pf(o, "\n");
}

/* the same with the key "<k1>.<k2>[i]" (i < 0: "<k1>.<k2>") */
static void r6as_arri(struct r6as_out * o, const char * k1, const char * k2, int i, const uint32_t * v, int n, uint32_t hx)
{
    char k[24];
    if (i < 0) snprintf(k, sizeof(k), "%s.%s", k1, k2);
    else snprintf(k, sizeof(k), "%s.%s[%d]", k1, k2, i);
    r6as_arr(o, k, v, n, hx);
}

static const char * const __attribute__((section(".data"))) r6as_bn[2] = { "lcd", "evf" };   /* .data, see r6as_addr */
static struct r6as_rd r6as_now[2];                       /* the fresh read of the last capture (main log line) */
static int r6as_now_ok = 0;                              /* 1 = read ("Display regs" ON), else "na" */

static const char * r6as_cls(int n)
{
    if (!r6as_now_ok) return "na";
    const struct r6as_rd * r = &r6as_now[n];
    if (r->got == R6AS_GOT_CORE) return "cpu";
    if (r->got != R6AS_GOT_ALL)
        return ((r->st & 7) && (r->st & 0x30) && r->dm == (n ? 0x10u : 0x01u) && !r6as_core_ok) ? "unk" : "off";
    int std = r->w[4] == (n ? 0x02u : 0x22u) && r->w[5] == 0 && r->w[6] == 0x0FFF0000;
    for (int i = 0; i < 6; i++) std &= r->w[7 + i] == r6as_std[i];
    return std ? "std" : "dif";
}

/* layer statistics (3.5): 36 bands of 30 rows, rows 30 b + 5 / 15 / 25, columns 0..2047 step 32; key.ne / .col per band,
 * key.top = the 6 most frequent non-empty values (value:count) */
static void r6as_layer(struct r6as_out * o, const char * key, uint32_t m)
{
    uint32_t hd[5] = { m, 0, 0, 0, 0 }, bm = 0;
    int ok = r6_fy_ram_ok(m, 0x24) && r6as_rd32(m + 4, &bm) && r6as_rd32(m + 0x1C, &hd[1]) && r6as_rd32(m + 0x20, &hd[2]);
    if (!ok || hd[1] != 2048 || hd[2] < 1080 || !r6_fy_ram_ok(bm, 2048 * 1080 * 4))
    {
        r6as_pf(o, "%s=0x%x,%d,%d,na\n", key, (int) m, (int) hd[1], (int) hd[2]);
        return;
    }
    uint32_t ne[36], col[36], tv[16], tn[16];
    int nt = 0;
    for (int b = 0; b < 36; b++)
    {
        ne[b] = col[b] = 0;
        for (int k = 0; k < 3; k++)
        {
            const volatile uint32_t * row = (const volatile uint32_t *) (bm + (30 * b + 5 + 10 * k) * 2048 * 4);
            for (int x = 0; x < 2048; x += 32)
            {
                uint32_t v = row[x];
                if (!(v & 0xFF000000) || v == 0xFF000000) continue;
                ne[b]++;
                uint32_t R = (v >> 16) & 0xFF, G = (v >> 8) & 0xFF, B = v & 0xFF;
                if ((v >> 24) >= 0x80 && (R != G || G != B)) col[b]++;
                int j = 0;
                while (j < nt && tv[j] != v) j++;
                if (j < nt) tn[j]++;
                else if (nt < 16) { tv[nt] = v; tn[nt] = 1; nt++; }
            }
        }
        hd[3] += ne[b]; hd[4] += col[b];
    }
    r6as_arr(o, key, hd, 5, 1);
    r6as_arri(o, key, "ne", -1, ne, 36, 0);
    r6as_arri(o, key, "col", -1, col, 36, 0);
    r6as_pf(o, "%s.top=", key);
    for (int k = 0; k < 6 && k < nt; k++)
    {
        int best = k;
        for (int j = k + 1; j < nt; j++) if (tn[j] > tn[best]) best = j;
        uint32_t a = tv[k], c = tn[k]; tv[k] = tv[best]; tn[k] = tn[best]; tv[best] = a; tn[best] = c;
        r6as_pf(o, "%s0x%08x:%d", k ? "," : "", (int) tv[k], (int) tn[k]);
    }
    r6as_pf(o, "\n");
}

static void r6as_tk_line(struct r6as_out * o, const char * k2, const struct r6as_tk * k)
{
    if (k->n != 1) r6as_pf(o, "ast.task.%s=%s\n", k2, k->n ? "many" : "none");
    else r6as_arri(o, "ast.task", k2, -1, &k->id, 4, 0);
}

/* the tables (seqlock: at most 3 tries, then ast.torn=1) */
static int r6as_tabs_text(struct r6as_out * o)
{
    for (int tr = 0; tr < 3; tr++)
    {
        int l0 = o->len, t0 = o->trunc;
        uint32_t s1 = r6as_seq, v[17];
        __sync_synchronize();
        if (!(s1 & 1))
        {
            const struct r6as_cnt c = C;
            r6as_arr(o, "ast.cnt", &c.ticks, sizeof(c) / 4, 0);
            r6as_pf(o, "ast.sw_n=%d\n", (int) c.sw_n);
            for (int n = 0; T && n < 2; n++)
            {
                char k[8];
                snprintf(k, sizeof(k), "vs.%s", r6as_bn[n]);
                for (int i = 0; i < T->nvs[n] && i < R6AS_VS_N; i++)
                    r6as_arri(o, "ast", k, i, &T->vs[n][i].t_first, 5 + R6AS_NW + 3, 0x1FFFF8);
                snprintf(k, sizeof(k), "sts.%s", r6as_bn[n]);
                for (int i = 0; i < T->nst[n] && i < R6AS_ST_N; i++)
                    r6as_arri(o, "ast", k, i, &T->st[n][i].v, 4, 1);
            }
            int n0 = c.sw_n > R6AS_SW_N ? c.sw_n - R6AS_SW_N : 0;
            for (int i = n0; T && i < (int) c.sw_n; i++)
            {
                const struct r6as_sw * e = &T->sw[i % R6AS_SW_N];
                v[0] = e->t; v[1] = e->t_dm; v[2] = e->trig; v[3] = e->old; v[4] = e->nw; v[5] = e->dur; v[6] = e->nsmp;
                v[7] = e->ext; v[8] = e->on0; v[9] = e->on1; v[10] = e->nchg;
                r6as_arri(o, "ast", "sw", i, v, 11, 0x18);
                for (int j = 0; j < e->npts && j < R6AS_SW_PTS; j++)
                {
                    const struct r6as_pt * p = &e->p[j];
                    char k[16];
                    snprintf(k, sizeof(k), "sw[%d].p", i);
                    v[0] = p->dt; v[1] = p->got0; v[2] = p->got1; v[3] = p->st0; v[4] = p->st1;
                    v[5] = p->b048_0; v[6] = p->b058_0; v[7] = p->b048_1; v[8] = p->b058_1;
                    r6as_arri(o, "ast", k, j, v, 9, 0x1F8);
                }
            }
            __sync_synchronize();
            if (r6as_seq == s1) return 0;
        }
        o->len = l0; o->trunc = t0;                          /* torn: again */
        if (o->len < o->cap) o->p[o->len] = 0;
        msleep(5);
    }
    return 1;
}

static const uint32_t __attribute__((section(".data"))) r6as_ram_a[6] = {   /* .data, see r6as_addr */ 0x18608, 0x2AF10, 0x2AF14, 0x2AF44, 0xEB04, 0xEB08 };   /* ast.ram (3.5) */

int r6_astro_ast(char * p, int cap, int * trunc)
{
    struct r6as_out o = { .p = p, .len = 0, .cap = cap, .trunc = 0 };
    uint32_t v[17];
    if (!p || cap < 2) { if (trunc) *trunc = 1; return 0; }
    p[0] = 0;
    r6as_pf(&o, "# ASTPROBE M12.0 (spec m12/m120 4.2)\n");
    v[0] = r6as_cfg(r6as_mode, 1); v[1] = r6as_lvl_pct[r6as_cfg(r6as_lcd, 5)]; v[2] = r6as_lvl_pct[r6as_cfg(r6as_evf, 5)];
    v[3] = R6AS_PAL_VER; v[4] = r6as_probe; v[5] = r6as_key; v[6] = r6as_core_ok;
    r6as_arr(&o, "ast.cfg", v, 7, 0);
    /* the fresh read (F11: only while "Display regs" is ON; review R1 L3: not before the task's core check after an ON) */
    r6as_now_ok = 0;
    memset(r6as_now, 0, sizeof(r6as_now));
    if (r6as_probe && !r6as_probe_new)
    {
        int g0 = r6as_mmio_rd(0, &r6as_now[0]), g1 = r6as_mmio_rd(1, &r6as_now[1]);
        r6as_now_ok = g0 >= 0 && g1 >= 0;
    }
    for (int n = 0; n < 2; n++)
    {
        const struct r6as_rd * r = &r6as_now[n];
        if (!r6as_now_ok) { r6as_pf(&o, "ast.now.%s=na\n", r6as_bn[n]); continue; }
        v[0] = r->st; v[1] = r->dm; v[2] = r->cpu; v[3] = r->got;
        memcpy(&v[4], r->w, sizeof(r->w));
        r6as_arri(&o, "ast.now", r6as_bn[n], -1, v, r->got == R6AS_GOT_ALL ? 4 + R6AS_NW : 4, 0x1FFF3);
    }
    r6as_pf(&o, "ast.cls=%s,%s\n", r6as_cls(0), r6as_cls(1));
    int torn = r6as_tabs_text(&o);
    /* latch words (3.5): [0x1615C8 / CC], [0x2F76C / 70], [0x1E1B4 / B8], cond0, cond1, 301C bits 16..17 per output */
    for (int n = 0; n < 2; n++)
    {
        v[n] = r6as_ram(R6AS_L1615 + 4 * n);
        v[2 + n] = r6as_ram(R6AS_L2F76 + 4 * n);
        v[4 + n] = r6as_ram(R6AS_L1E1B + 4 * n);
        v[8 + n] = r6as_now_ok && r6as_now[n].got == R6AS_GOT_ALL ? (r6as_now[n].w[0] >> 16) & 3 : (uint32_t) -1;
    }
    for (int n = 0; n < 2; n++) v[6 + n] = v[n] == 1 || v[4 + (v[2 + n] != 0)] != 2;
    r6as_arr(&o, "ast.latch", v, 10, 0x3F);
    for (int i = 0; i < 6; i++) v[i] = r6as_ram(r6as_ram_a[i]);
    v[6] = CURRENT_GUI_MODE; v[7] = gui_state; v[8] = lv_dispsize; v[9] = r6_gd_disp_mask();
    r6as_arr(&o, "ast.ram", v, 10, 0x27F);
    uint32_t ct = r6as_ram(R6AS_CUR_TASK);
    v[0] = get_cpu_id(); v[1] = v[2] = 0xFFFFFFFF;
    if (r6_fy_ram_ok(ct, R6AS_TASK_SIZE)) { v[1] = r6as_byte(ct + 0x50); v[2] = r6as_byte(ct + 0x51); }
    v[3] = r6as_my_cpu; v[4] = r6as_my_req; v[5] = r6as_my_asg;
    r6as_arr(&o, "ast.cpu", v, 6, 0);
    struct r6as_tk d, s;
    r6as_task_core(&d, &s);
    r6as_tk_line(&o, "disp", &d);
    r6as_tk_line(&o, "sub", &s);
    v[0] = (r6as_ram(R6AS_IRQ_1C6) >> 12) & 3; v[1] = (r6as_ram(R6AS_IRQ_1E6) >> 12) & 3;
    r6as_arr(&o, "ast.irq", v, 2, 3);
    r6as_layer(&o, "ast.l0", (uint32_t) rgb_vram_info);
    r6as_layer(&o, "ast.l1", R6AS_MARV_L1);
    r6as_pf(&o, "ast.torn=%d\n", torn);
    if (trunc) *trunc = o.trunc;
    return o.len;
}

int r6_astro_main(char * s, int n)
{
    return snprintf(s, n, "%d,%d,%d,%d,0x%x,%d,%s,%s", r6as_cfg(r6as_mode, 1), r6as_lvl_pct[r6as_cfg(r6as_lcd, 5)],
        r6as_lvl_pct[r6as_cfg(r6as_evf, 5)], r6as_probe, (int) r6_gd_disp_mask(), (int) get_cpu_id(), r6as_cls(0), r6as_cls(1));
}

/* ---- menu Display > "Astro red" (2.1) ---- */
static MENU_UPDATE_FUNC(r6as_pal_update)
{
    uint32_t pal = r6_astro_pal();
    if (!(pal & R6AS_PAL_RED)) MENU_SET_VALUE("colours");
    else MENU_SET_VALUE("red, %s %d%%", r6_gd_disp_mask() == 0x10 ? "EVF" : "LCD", (int) ((pal >> 8) & 0xFF));
    uint32_t p = pal & R6AS_PAL_RED ? pal : (R6AS_PAL_RED | (100 << 8)), r[3];
    for (int i = 0; i < 3; i++) r[i] = (r6_astro_map(p, ""[i], 0xFF000000) >> 16) & 0xFF;   /* text 1, bar 11, error 8 */
    MENU_SET_WARNING(MENU_WARN_INFO, "Text R %d, bar R %d, error R %d, black stays 1/1/1.", (int) r[0], (int) r[1], (int) r[2]);
}

static struct menu_entry r6as_menu[] = {
    {
        .name   = "Astro red",
        .select = menu_open_submenu,
        .help   = "M12.0: ML's own pixels in red. Canon's display stays in colour.",
        .children = (struct menu_entry[]) {
            {
                .name    = "Mode",
                .priv    = &r6as_mode,
                .max     = 1,
                .choices = CHOICES("Off", "ML only"),
                .help    = "ML only: ML's own pixels (menu, overlays, status lines) in red. Canon's display stays in colour.",
                .help2   = "Saved. M12.1 adds Full. The transparent menu view is blocked.",
            },
            {
                .name    = "LCD level",
                .priv    = &r6as_lcd,
                .max     = 5,
                .choices = CHOICES("100%", "50%", "25%", "12%", "150%", "200%"),
                .help    = "Red level of ML's pixels on the LCD (signal, not light: 50 % is ~20 % light).",
                .help2   = "150/200 %: text, warnings and errors look the same.",
            },
            {
                .name    = "EVF level",
                .priv    = &r6as_evf,
                .max     = 5,
                .choices = CHOICES("100%", "50%", "25%", "12%", "150%", "200%"),
                .help    = "Red level of ML's pixels in the viewfinder.",
                .help2   = "150/200 %: text, warnings and errors look the same.",
            },
            {
                .name      = "Palette",
                .update    = r6as_pal_update,
                .icon_type = IT_ALWAYS_ON,
                .help      = "Red levels of the output shown.",
            },
            MENU_EOL
        },
    },
};

static void r6as_init()
{
    menu_add("Display", r6as_menu, COUNT(r6as_menu));
}

INIT_FUNC("r6_astro", r6as_init);

#endif /* CONFIG_R6_ASTRO */
