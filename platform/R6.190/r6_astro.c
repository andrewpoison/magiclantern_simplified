/*
 * R6.190 M12.0 / M12.1: "Astro red" (docs/m12_plan.md Rev. 2; spec m12/m120 spec_m120.md Rev. 2 and spec m12/m121
 * spec_m121.md Rev. 2; user answers of 2026-10-11 / 2026-10-12: E1 a M12.0 first, E2 Tor 0f MMIO read only, E3 a Tor 2
 * the display matrix write, E4 a one write function under ML's lock, E5 pure red, E6 levels 100/50/25/12/150/200 % per
 * output, E8 c (M12.2: in M12.1 a saved Full comes back as ML only), E9 ML's palette by meaning, E13 a only get_cpu_id()
 * checks, no task_create_ex).
 *
 * Display > "Astro red":
 *  - Mode Off / "ML only" / "Full" (saved). "ML only" maps ML's own palette (r6_layer.c r6l_build_lut, once per conversion)
 *    to pure red by meaning (text 200, errors 255, warnings 230..245, the selection bar 75, grey ramp 0.75 x g, ...;
 *    level per output: R = min(255, max(2, (R100 * level + 50) / 100)), G = B = 0). Transparent entries (0, 3) stay
 *    exact (the conversion's word test), black stays 0xFF010101 (Canon's colour key is 0xFF000000: never produced),
 *    the masks 26 / 27 keep their alpha and G = B = 1, the grid line 28 keeps its alpha 0x99.
 *  - "Full" (M12.1, Tor 2): the output matrix "Hrady1" of the output shown (LCD 0xD282B048..5C, EVF 0xD283B048..5C) gets
 *    the red set (G and B rows 0, R row kY = the level of that output, kCb = kCr = 0): the whole display shows its
 *    luminance in red. ML's palette turns grey by meaning (g = max(2, R100), the matrix makes it red: no double dimming).
 *    Canon writes its standard set whenever an output switches on: ML writes red again; every store is judged by a later
 *    read >= 50 ms after it, at most 5 stores per second and output, 3 stores without effect or Canon resets without a
 *    device change within 10 s (or 3 in a row): error off (back to ML only, the standard set, a notice). When Full ends:
 *    Canon's standard set again, by the status bits of each output, also for the output not shown, also later. A saved
 *    Full is not restored at start (M12.1): the astro task sets it to ML only. "Test: 10 s": the LCD in red for 10 s.
 *  - LCD level / EVF level (saved, default 50 %): the level of the output shown (display mask 0x10: EVF).
 *  - The transparent ML menu view (src/menu.c, lv only) is switched off within one task step while the mode is on
 *    (ML functions only; src/menu.c stays unchanged).
 *
 * Debug > LV probe (lvprobe.c): "Display regs" (not saved, OFF at boot) and "Key test".
 *  - Tor 0f (E2, approved 2026-10-11): r6as_mmio_rd is the probe's read function. Per output n (LCD n = 0 at 0xD282xxxx,
 *    EVF n = 1 at 0xD283xxxx) the status word +0x3000 at any time; the 13 words +0x301C, +0xA010, +0xA014, +0xA018,
 *    +0xB020, +0xB040, +0xB044, +0xB048..+0xB05C only while the status of the same output shows the device clock (& 7)
 *    and the output (& 0x30), Canon's display mask selects exactly this output, Canon's display tasks were found on CPU0
 *    (r6as_core_ok, spec 1.1 / 3.5) and this task runs on CPU0 (get_cpu_id), all in one IRQ / FIQ lock; single 32-bit
 *    loads, never +0x3010, never +0x3020..+0x3034, never HDMI 0xD280xxxx, never Hrady0. No MMIO at all once ML shuts down.
 *  - Tor 2 (E3 a, effective with spec m12/m121 Rev. 2): r6as_mmio_wr is the only MMIO write of this file: the 6 matrix
 *    words of the LCD or the EVF, only Canon's standard set or the red set of an E6 level (constant tables, cross-checked
 *    against r6as_addr / r6as_std in the lock), only while that output's status shows & 7 and & 0x30 (stores only at
 *    (s & 0x37) == 0x37), inside ML's IRQ / FIQ lock, on CPU0 with Canon's display tasks there; red only while the display
 *    mask selects the output, 301C bits 16..17 = 0, A014 = A018 = 0, [0x1615C8 + 4 n] = 0, [0x2F76C + 4 n] = n, channel
 *    state 2 and a known set in the matrix. Never +0x302C, +0x301C, HDMI, Hrady0, WB; no Canon call, no Canon RAM write,
 *    no property; nothing once ML shuts down. Called by the astro task only (r6as_t2_step): the red set while Full or the
 *    test is on, the standard set for the restore, and with "Display regs" ON a read-only peek (may = 0) at the output not
 *    shown (E2 rate).
 *  - Rate: the astro task reads both outputs every R6AS_TICK_MS (250 ms, about 4 / s) while "Display regs" is ON; a
 *    change of the display mask, a status edge, a changed +0xB048 or an ML store starts a transition trace: every 20 ms
 *    (50 / s) for exactly 2 s, never extended, at most R6AS_TR_MAXSMP samples; a new trace only after one normal tick
 *    without a trace and at the earliest R6AS_TR_GAP_MS (2 s) after the previous one ended (review R1 L1). A status word
 *    0xFFFFFFFF counts as implausible: status only (review R1 L2, ast.cnt st_bad). While Tor 2 is active its step reads
 *    the output it writes every 20 ms (plan 5.2).
 *  - The log: value sets, status words, counters, the trace ring (heap, allocated at the first ON, never freed), the
 *    latch words and display state in Canon RAM, the task table entries of DispDCtrl / SubDispT (name pointers compared
 *    as numbers, never dereferenced), the Tor 2 state, counters and write ring (ast.t2*, ast.wr, ast.pk) - read only.
 *  - "Key test": for 10 s two 40x40 squares in ML's pixels with the palette values 0xFF010000 / 0xFF020000 (indices
 *    29 / 30, unused by ML), on the LCD shooting screen; the user notes whether Canon's mixer shows them dark.
 *
 * Canon calls: none new. ML functions and the existing stubs msleep, vsnprintf (the AST text), Acquire- /
 * ReleaseRecursiveLock (BMP_LOCK, as r6h_task / r6_gd_erase); ML's NotifyBox for the Tor 2 notices (as r6_propw.c). Not
 * exported to modules.
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
static CONFIG_INT("r6.astro.mode", r6as_mode, 0);    /* 0 Off, 1 ML only, 2 Full (saved; M12.1: a Full without command -> 1) */
static CONFIG_INT("r6.astro.lcd",  r6as_lcd, 1);     /* index into r6as_lvl_pct: 1 = 50 % (E6) */
static CONFIG_INT("r6.astro.evf",  r6as_evf, 1);
static const uint8_t __attribute__((section(".data"))) r6as_lvl_pct[6] = { 100, 50, 25, 12, 150, 200 };   /* .data, see r6as_addr */

#define R6AS_PAL_VER     1          /* the palette below (log ast.cfg) */
#define R6AS_PAL_RED     1          /* r6_astro_pal: bit 0 "ML only" */
#define R6AS_PAL_KEY     2          /*               bit 1 colour-key test */
#define R6AS_PAL_GREY    4          /*               bit 2 "Full" active: grey by meaning (M12.1, E9) */

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
#define R6AS_SW_N    10              /* trace ring (M12.1: 6 -> 10, eval 3.3 7) */
#define R6AS_SW_PTS  9               /* change points per trace (M12.1: 17 fields each; 12 -> 9: the AST worst case fits 32 KB) */

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

/* ---- Tor 2 (E3 a, approved 2026-10-11, effective with spec m12/m121 Rev. 2): the display matrix write (spec 3) ---- */
#define R6AS_NM          6           /* the matrix words B048..B05C */
enum { R6AS_SET_STD = 0, R6AS_SET_RED = 1 };
/* Tor 2 (E3 a, approved 2026-10-11, effective with spec m12/m121 Rev. 2): the ONLY MMIO addresses ML writes besides the
 * card LED: the Hrady1 matrix words B048..B05C of the LCD (0xD282) and the EVF (0xD283). Never +0x302C, +0x301C, HDMI
 * 0xD280xxxx, Hrady0, WB. Own section: only r6as_mmio_wr loads its anchor (astro_gate.sh point 5). */
static const uint32_t __attribute__((section(".rodata.r6as_wa"))) r6as_wa[2][R6AS_NM] = {
    { 0xD282B048, 0xD282B04C, 0xD282B050, 0xD282B054, 0xD282B058, 0xD282B05C },
    { 0xD283B048, 0xD283B04C, 0xD283B050, 0xD283B054, 0xD283B058, 0xD283B05C },
};
/* the ONLY values (word by word): row 0 Canon's standard set (Plan-S1 P10, M12.0 log), rows 1..6 the red set at the E6 levels
 * 100 / 50 / 25 / 12 (= 12.5) / 150 / 200 % (plan 5.2, spec m12/m121 2.2): G and B rows 0, R row kY only */
static const uint32_t __attribute__((section(".rodata.r6as_wv"))) r6as_wv[7][R6AS_NM] = {
    { 0x7A7E1000, 0x00007493, 0x1C5A1000, 0x00000000, 0x00001000, 0x0000166F },
    { 0, 0, 0, 0, 0x00001000, 0 }, { 0, 0, 0, 0, 0x00000800, 0 }, { 0, 0, 0, 0, 0x00000400, 0 },
    { 0, 0, 0, 0, 0x00000200, 0 }, { 0, 0, 0, 0, 0x00001800, 0 }, { 0, 0, 0, 0, 0x00002000, 0 },
};
struct r6as_wr {                       /* filled by r6as_mmio_wr (log, decisions); the only memory it stores to besides sp */
    uint32_t st, dm, cpu, l1615, l2f76, chan;
    uint32_t w[R6AS_NW];               /* the 13 words before (E2 order: 301C, A010, A014, A018, B020, B040, B044, B048..B05C) */
    uint32_t back[R6AS_NM];            /* read back after the store (0 if no store) */
    uint8_t res, why, cls, stored;
};
enum { R6AS_WR_NONE = 0, R6AS_WR_DONE = 1, R6AS_WR_BACK = 2, R6AS_WR_WAIT = 3,           /* in the lock */
       R6AS_WR_ARG = -1, R6AS_WR_SHUT = -2, R6AS_WR_CORE = -3,                         /* refused before the lock */
       R6AS_WR_TAB = -4 };                       /* table check failed: red row form before the lock, cross check in the lock */
enum { R6AS_Y_OK = 0, R6AS_Y_OFF, R6AS_Y_CPU, R6AS_Y_UNK, R6AS_Y_DM, R6AS_Y_301C, R6AS_Y_WB, R6AS_Y_LATCH, R6AS_Y_ST,
       R6AS_Y_MAY, R6AS_Y_TAB };
enum { R6AS_CLS_STD = 0, R6AS_CLS_RED = 1 /* = target */, R6AS_CLS_MIX = 2 /* known, not target */, R6AS_CLS_UNK = 3,
       R6AS_CLS_NA = 4 /* the 13 words were not read */ };
#define R6AS_L_CHAN(l2f76) (R6AS_L1E1B + 4 * ((l2f76) != 0))

/* Tor 2 (E3 a): the only MMIO write of ML's astro code. n: 0 LCD, 1 EVF; set: STD / RED; lvl: the E6 level index (RED);
 * may: 0 = classify only (no store). Everything between cli and sei: status, core, the 13 words, the decision, the table
 * cross check, the six stores, the read back. No call, no other store target than *o and the stack. */
static int __attribute__((noinline, noipa)) r6as_mmio_wr(int n, int set, int lvl, int may, struct r6as_wr * o)
{
    if ((unsigned) n > 1 || (unsigned) set > 1 || (unsigned) lvl > 5 || !o) return R6AS_WR_ARG;
    if (ml_shutdown_requested) return R6AS_WR_SHUT;                  /* no MMIO at all once ML shuts down (as M12.0 F10) */
    if (!r6as_core_ok) return R6AS_WR_CORE;                          /* DispDCtrl / SubDispT on CPU0 (M12.0 F2) */
    const uint32_t * tv = r6as_wv[set == R6AS_SET_RED ? 1 + lvl : 0];
    if (set == R6AS_SET_RED && (tv[0] | tv[1] | tv[2] | tv[3] | tv[5] | (tv[4] & 0xFFFF0000) || tv[4] < 0x200 || tv[4] > 0x2000))
        return R6AS_WR_TAB;                                          /* Plan-S1 B7: the red row is G = B = 0, kCb = kCr = 0 */
    const volatile uint32_t * a = r6as_addr[n];                      /* reads: the E2 table */
    const volatile uint32_t * wa = r6as_wa[n];                       /* the store: the Tor 2 table */
    const uint32_t sel = n ? 0x10 : 0x01, b020 = n ? 0x02 : 0x22;
    for (int i = 0; i < (int) (sizeof(*o) / 4); i++) ((volatile uint32_t *) o)[i] = 0;   /* before the lock; volatile: no memset */
    int why = R6AS_Y_OK, res = R6AS_WR_WAIT, cls = R6AS_CLS_NA, stored = 0;
    uint32_t old = cli();
    uint32_t s = *(volatile uint32_t *) a[0];                        /* status word (Plan-S1 P7) */
    uint32_t cpu = get_cpu_id(), dm = 0xFFFFFFFF, l1 = 0xFFFFFFFF, l2 = 0xFFFFFFFF, ch = 0xFFFFFFFF;
    if (!(s & 7) || !(s & 0x30) || s == 0xFFFFFFFF) why = R6AS_Y_OFF;
    else if (cpu != R6AS_DISP_CPU) why = R6AS_Y_CPU;
    else
    {
        uint32_t dd = *(volatile uint32_t *) R6AS_DISPDEV_PTR;
        if (dd >= 0x1000 && dd <= 0x40000000 - 0x1C && !(dd & 3)) dm = *(volatile uint32_t *) (dd + 0x18);
        l1 = *(volatile uint32_t *) (R6AS_L1615 + 4 * n);            /* Canon RAM, fixed addresses, read only */
        l2 = *(volatile uint32_t *) (R6AS_L2F76 + 4 * n);
        ch = *(volatile uint32_t *) R6AS_L_CHAN(l2);
        for (int i = 0; i < R6AS_NW; i++) o->w[i] = *(volatile uint32_t *) a[1 + i];
        int known = o->w[4] == b020 && o->w[5] == 0 && o->w[6] == 0x0FFF0000, tgt = 1, std = 1;
        for (int i = 0; i < R6AS_NM; i++)
        {
            uint32_t v = o->w[7 + i];
            int k = 0;
            for (int r = 0; r < 7; r++) k |= v == r6as_wv[r][i];    /* standard or red at ANY level, word by word */
            known &= k; tgt &= v == tv[i]; std &= v == r6as_wv[0][i];
        }
        cls = !known ? R6AS_CLS_UNK : tgt ? R6AS_CLS_RED : std ? R6AS_CLS_STD : R6AS_CLS_MIX;
        if (set == R6AS_SET_STD && tgt) cls = R6AS_CLS_STD;
        if (!known) why = R6AS_Y_UNK;
        else if (set == R6AS_SET_RED && dm != sel) why = R6AS_Y_DM;
        else if (set == R6AS_SET_RED && ((o->w[0] >> 16) & 3)) why = R6AS_Y_301C;
        else if (set == R6AS_SET_RED && (o->w[2] || o->w[3])) why = R6AS_Y_WB;
        else if (set == R6AS_SET_RED && (l1 != 0 || l2 != (uint32_t) n || ch != 2)) why = R6AS_Y_LATCH;
        else if (tgt) res = R6AS_WR_NONE;
        else if ((s & 0x37) != 0x37) why = R6AS_Y_ST;                /* S1 F6: stores only at the measured status bits */
        else if (!may) why = R6AS_Y_MAY;
        else
        {
            int ok = 1;                                              /* S1 F3: the tables live in RAM: cross check */
            for (int i = 0; i < R6AS_NM; i++)
            {
                ok &= wa[i] == a[8 + i];                             /* the store address against the E2 table */
                ok &= r6as_wv[0][i] == r6as_std[i];                  /* the standard row against its copy (every set) */
            }
            if (set == R6AS_SET_RED)
                ok &= tv[4] == 0x1000 || tv[4] == 0x800 || tv[4] == 0x400 || tv[4] == 0x200 || tv[4] == 0x1800 || tv[4] == 0x2000;
            if (!ok) { res = R6AS_WR_TAB; why = R6AS_Y_TAB; }
            else
            {
                for (int i = 0; i < R6AS_NM; i++) *(volatile uint32_t *) wa[i] = tv[i];   /* THE store (one instruction) */
                stored = 1;
                asm volatile ("dsb" ::: "memory");
                int eq = 1;
                for (int i = 0; i < R6AS_NM; i++) { o->back[i] = *(volatile uint32_t *) a[8 + i]; eq &= o->back[i] == tv[i]; }
                res = eq ? R6AS_WR_DONE : R6AS_WR_BACK;
            }
        }
    }
    asm volatile ("dsb" ::: "memory");
    sei(old);
    o->st = s; o->dm = dm; o->cpu = cpu; o->l1615 = l1; o->l2f76 = l2; o->chan = ch;
    o->res = res; o->why = why; o->cls = cls; o->stored = stored;
    return res;
}

/* ---- Tor 2: timing (QEMU: R6AS_FLK_WIN_MS, R6AS_TEST_MS, R6AS_TEST_WAIT_MS, R6AS_BNC_WIN_MS are variables; qcheck.sh
 * checks these camera values) ---- */
#define R6AS_T2_STEP_MS   20     /* Tor 2 active: the task step (plan 5.2) */
#define R6AS_WR_N         5      /* budget: at most 5 stores per output in any 1 s (ring of store times, plan 5.2 6) */
#define R6AS_WR_WIN_MS    1000
#define R6AS_FLK_N        3      /* 3 "stores without effect" (also Canon resets without a device change) ... */
#define R6AS_FLK_WIN_MS   10000  /* ... within 10 s: error off (spec 4.6) */
#define R6AS_BACK_MAX     3      /* ... or 3 in a row */
#define R6AS_EVAL_MS      50     /* a store is judged by a later read >= 50 ms after it (S1 F1 / F9) */
#define R6AS_EVAL_MAX_MS  1000   /* ... or dropped without a read after 1 s (ev_lost) */
#define R6AS_TEST_MS      10000  /* "Test: 10 s" from the first verified red ... */
#define R6AS_TEST_WAIT_MS 20000  /* ... after the LCD shooting screen held R6AS_TEST_HOLD_MS, waited for at most 20 s */
#define R6AS_TEST_HOLD_MS 500
#define R6AS_RST_SLOW_MS  250    /* outputs "check later", unknown or tried: read every 250 ms (E2 rate) */
#define R6AS_RST_TRY      3      /* unknown in 3 restore calls: H4 */
#define R6AS_PEEK_MS      250    /* the read-only peek at the output not shown (Display regs ON, E2 rate) */
#define R6AS_BNC_N        10     /* more than 10 Canon resets with a device change in 10 s: no red store for 2 s (S1 F13) */
#define R6AS_BNC_WIN_MS   10000
#define R6AS_BNC_PAUSE_MS 2000
#define R6AS_BNC_RING     16
#define R6AS_WR_LOG       16     /* ast.wr entries (heap, the first Full / Test command) */

/* ---- Tor 2: the state (spec 4); the astro task is its only writer, the menu task writes r6as_mode, the sequence numbers
 * (+ 1 only) and publishes the write log once ---- */
enum { R6AS_T_IDLE = 0, R6AS_T_WAIT, R6AS_T_RED, R6AS_T_REST };
enum { R6AS_H1 = 1, R6AS_H2, R6AS_H3, R6AS_H4E, R6AS_H5, R6AS_H6, R6AS_H4L };
enum { R6AS_C_RED = 0, R6AS_C_REWRITE, R6AS_C_LEVEL, R6AS_C_RESTORE, R6AS_C_RESTORE_CH0, R6AS_C_TEST, R6AS_C_TESTEND, R6AS_C_TRIP,
       R6AS_C_RESET, R6AS_C_NOEFF, R6AS_C_BOUNCE, R6AS_C_TAB };
struct r6as_bk {                         /* per output */
    int t_wr[R6AS_WR_N], t_nb[R6AS_FLK_N], t_bn[R6AS_BNC_RING];   /* budget, "store without effect" and bounce rings (never reset) */
    int t_st, t_ver, t_flank, t_slow, t_pk, t_rd, bnc_until;
    uint32_t st;                         /* the last status word */
    uint8_t i_wr, i_nb, i_bn, pend, ver, ver_lvl, dev, ev, ev_back, run, off_seen, later, unk_n, one, tried, h4, ch0, clsd, key, have,
            cls, why_next, ev_set, ev_lvl;    /* ev_set / ev_lvl: the target of the store being judged (review R1 F1) */
};
struct r6as_t2c {                        /* ast.t2cnt (36 fields) */
    uint32_t calls, stores, done, back, none, w_off, w_cpu, w_core, w_unk, w_dm, w_301c, w_wb, w_latch, w_st, w_may, w_tab, rs_dev,
             rs_nodev, back_late, nb_eff_done, nb_eff_back, ev_lost, bnc, trip_flk, trip_back, trip_unk, trip_tab, rst_stores, rst_done,
             rst_ch0, rst_ch0_ok, rst_ch0_red, tests, shut, peeks, ev_chg;   /* ev_chg: the target changed before the judging call (R1 F1) */
};
struct r6as_wl { int t, dfl, ddm; uint8_t n, call, set, lvl; int8_t res; uint8_t why, cls, cpu; uint32_t st, dm, l1615, l2f76, chan, w[10], back[R6AS_NM]; };
static struct r6as_bk K[2];
static struct r6as_t2c C2;
static struct r6as_wl * volatile r6as_wl = 0;    /* ast.wr (heap; the menu publishes it after its memset) */
static uint32_t r6as_wl_n = 0;
static struct r6as_wr r6as_lo[2];                /* the last call per output (log entries without a call) */
static volatile int r6as_full_seq = 0, r6as_test_seq = 0;   /* the menu: + 1 only (4.1) */
static int r6as_seen_fs = 0, r6as_seen_ts = 0, r6as_t2_init = 0, r6as_t2_fast = 0, r6as_t2_need = 0, r6as_t2n_was = 0;
static volatile uint8_t r6as_full_on = 0, r6as_t2_block = 0, r6as_boot_dn = 0, r6as_tst = R6AS_T_IDLE;
static uint8_t r6as_armed = 0, r6as_t2_tab = 0, r6as_cmd_any = 0, r6as_err_n = 0, r6as_last_err = 0, r6as_lv[2];
static int8_t r6as_tn = -1;
static int r6as_t_t0 = 0, r6as_t_ok = 0, r6as_t_red0 = 0, r6as_t_v10 = 0, r6as_t_mode = 0, r6as_t_dm2 = 0;
static uint32_t r6as_dm2 = 0;
static const char * volatile r6as_test_txt = "idle";
static const char * volatile r6as_t_why = 0;
static const char * volatile r6as_t2_msg = 0;

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
struct r6as_pt { uint16_t dt; uint8_t got0, got1; uint32_t st0, st1, b048_0, b058_0, b048_1, b058_1; uint32_t x[2][4]; };   /* x: lat, A010, A014, A018 per output (M12.1) */
struct r6as_pk { int t; int8_t res; uint8_t why, cls, cpu; uint32_t st, dm, l1615, l2f76, chan, w[R6AS_NW]; };   /* ast.pk (4.9) */
struct r6as_sw { uint32_t t, t_dm, old, nw; uint16_t dur, nsmp, nchg; int16_t on0, on1; uint8_t trig, ext, npts, pad; struct r6as_pt p[R6AS_SW_PTS]; };
struct r6as_tabs {
    struct r6as_vs vs[2][R6AS_VS_N];
    struct r6as_st st[2][R6AS_ST_N];
    struct r6as_sw sw[R6AS_SW_N];
    struct r6as_pk pk[2][2];                     /* M12.1: the peek at the output not shown, first / last */
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
    int mode = r6as_cfg(r6as_mode, 2), key = r6as_key == R6AS_K_SHOW, grey = r6as_full_on;   /* grey: Full active (M12.1, E9) */
    if (!mode && !key && !grey) return 0;
    int lvl = r6_gd_disp_mask() == 0x10 ? r6as_cfg(r6as_evf, 5) : r6as_cfg(r6as_lcd, 5);
    return (grey ? R6AS_PAL_GREY : mode ? R6AS_PAL_RED : 0) | (key ? R6AS_PAL_KEY : 0) | ((uint32_t) r6as_lvl_pct[lvl] << 8);
}

uint32_t r6_astro_map(uint32_t pal, int i, uint32_t v)
{
    if ((pal & R6AS_PAL_KEY) && (i == 29 || i == 30))        /* the colour-key test (2.5), any mode and level */
        return i == 29 ? 0xFF010000 : 0xFF020000;
    if (!(pal & (R6AS_PAL_RED | R6AS_PAL_GREY))) return v;
    uint32_t a = v & 0xFF000000;
    if (!a || i == 2 || i == 26 || i == 27) return v;        /* transparent, black (never 0xFF000000), the masks */
    int k = (i >= 0 && i < 80) ? i : 4;                      /* indexed2rgb: index >= 80 shows entry 4 */
    if (pal & R6AS_PAL_GREY)                                 /* M12.1 Full (5.1): grey g = max(2, R100), no level (the matrix has it) */
    {
        uint32_t g = MAX(2, r6as_r100[k]);
        return a | (g << 16) | (g << 8) | g;
    }
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
    for (int n = 0; n < 2; n++)                              /* M12.1 (6.2): the latch words, 301C bits 16..17 and the WB words */
    {
        uint32_t l1 = r6as_ram(R6AS_L1615 + 4 * n), l2 = r6as_ram(R6AS_L2F76 + 4 * n), ch = r6as_ram(R6AS_L_CHAN(l2));
        uint32_t h = r[n].got == R6AS_GOT_ALL ? (r[n].w[0] >> 16) & 3 : 0xFF;
        p.x[n][0] = (h << 24) | (MIN(l1, 0xFFu) << 16) | (MIN(l2, 0xFFu) << 8) | MIN(ch, 0xFFu);
        for (int j = 1; j < 4; j++) p.x[n][j] = r[n].w[j];
    }
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

/* ---- Tor 2 (spec 4): the state machine of the astro task ---- */
static const char * const __attribute__((section(".data"))) r6as_hn[8] = { "",   /* .data, see r6as_addr; notices <= 98 characters (5.3) */
    "Astro red: Canon resets the display. Full is off.", "Astro red: unknown display values. Full is off.",
    "Astro red: the matrix write is not taken. Full is off.", "Astro red: EVF stays red until the eye leaves the viewfinder.",
    "Astro red: Full blocked until restart.", "Astro red: internal table error. Restart the camera.",
    "Astro red: LCD stays red until the camera is switched off and on." };

static void r6as_note(int h)
{
    r6as_t2_msg = r6as_hn[h];
    if (r6as_tst == R6AS_T_IDLE && !gui_menu_shown()) NotifyBox(5000, "%s", r6as_hn[h]);   /* the test reports in the menu only */
}

/* one ast.wr entry: the fields of the last call on output n (6.2) */
static void r6as_wl_add(int n, int call, int set, int lvl, int res, int now)
{
    struct r6as_wl * L = r6as_wl;
    if (!L) return;
    struct r6as_wl * e = &L[r6as_wl_n % R6AS_WR_LOG];
    const struct r6as_wr * o = &r6as_lo[n];
    r6as_wl_n++;
    e->t = now; e->n = n; e->call = call; e->set = set; e->lvl = lvl; e->res = res; e->why = o->why; e->cls = o->cls; e->cpu = o->cpu;
    e->st = o->st; e->dm = o->dm; e->l1615 = o->l1615; e->l2f76 = o->l2f76; e->chan = o->chan;
    for (int i = 0; i < 4; i++) e->w[i] = o->w[i];
    for (int i = 0; i < R6AS_NM; i++) { e->w[4 + i] = o->w[7 + i]; e->back[i] = o->back[i]; }
    e->dfl = now - K[n].t_flank; e->ddm = now - r6as_t_dm2;
}

/* error off (4.7): Full ends (ML only), the test stops, one restore try per output with a red set; TAB: no further Tor 2
 * access in this session; the second error off blocks Full and the test until restart */
static void r6as_trip(int h, int n, int now)
{
    (&C2.trip_flk)[h == R6AS_H1 ? 0 : h == R6AS_H3 ? 1 : h == R6AS_H2 ? 2 : 3]++;
    r6as_err_n++;
    r6as_last_err = h;
    r6as_armed = 0;
    r6as_full_on = 0;
    if (r6as_mode == 2) r6as_mode = 1;                       /* ML only: ML's pixels stay red (spec 4.7 1, 11.2 question 2) */
    if (r6as_tst == R6AS_T_WAIT || r6as_tst == R6AS_T_RED) { r6as_tst = R6AS_T_REST; r6as_t_why = "stopped: error"; }
    for (int i = 0; i < 2; i++)
    {
        K[i].ver = 0;
        if (K[i].pend) { K[i].one = 1; K[i].tried = 0; }
    }
    if (h == R6AS_H6) r6as_t2_tab = 1;
    if (h == R6AS_H6 || r6as_err_n >= 2) r6as_t2_block = 1;
    r6as_wl_add(n, h == R6AS_H6 ? R6AS_C_TAB : R6AS_C_TRIP, 0, 0, h, now);
    r6as_note(h != R6AS_H6 && r6as_t2_block ? R6AS_H5 : h);  /* review R2 F4: the second error off says "blocked" (H5) at once */
}

static void r6as_h4(int n)
{
    if (K[n].h4) return;
    K[n].h4 = 1;
    r6as_note(n ? R6AS_H4E : R6AS_H4L);
}

/* a Canon reset with a device change (normal): counted, the bounce ring (S1 F13: more than 10 in 10 s pause red stores 2 s) */
static void r6as_bounce(int n, int now)
{
    struct r6as_bk * k = &K[n];
    C2.rs_dev++;
    r6as_wl_add(n, R6AS_C_RESET, R6AS_SET_RED, k->ver_lvl, 1, now);   /* ast.wr: every reset (res 1: with a device change) */
    k->t_bn[k->i_bn] = now;
    k->i_bn = (k->i_bn + 1) % R6AS_BNC_RING;
    if (now - k->t_bn[(k->i_bn + R6AS_BNC_RING - R6AS_BNC_N - 1) % R6AS_BNC_RING] < R6AS_BNC_WIN_MS)
    {
        C2.bnc++;
        k->bnc_until = now + R6AS_BNC_PAUSE_MS;
        r6as_wl_add(n, R6AS_C_BOUNCE, R6AS_SET_RED, k->ver_lvl, 0, now);
    }
}

/* a "store without effect" (kind 0: not in the target >= 50 ms after a store, no device change; kind 1: a Canon reset
 * without a device change after a verified target); the 3rd within R6AS_FLK_WIN_MS: error off (4.6); 1 = tripped */
static int r6as_nb(int n, int kind, int now)
{
    struct r6as_bk * k = &K[n];
    k->t_nb[k->i_nb] = now;
    k->i_nb = (k->i_nb + 1) % R6AS_FLK_N;
    r6as_wl_add(n, kind ? R6AS_C_RESET : R6AS_C_NOEFF, !kind || k->pend, k->ver_lvl, 0, now);
    if (now - k->t_nb[k->i_nb] < R6AS_FLK_WIN_MS) { r6as_trip(kind ? R6AS_H1 : R6AS_H3, n, now); return 1; }
    return 0;
}

/* a status edge (& 7, & 0x30 or 0xFFFFFFFF): a device change; "off" since the last store (4.3 b) */
static void r6as_edge(int n, uint32_t s, int now)
{
    struct r6as_bk * k = &K[n];
    int key = s == 0xFFFFFFFF ? 4 : ((s & 7) ? 1 : 0) | ((s & 0x30) ? 2 : 0);
    if (k->have && key != k->key) { k->dev = 1; k->t_flank = now; if (key != 3) k->off_seen = 1; }
    k->key = key; k->have = 1; k->st = s;
}

/* review R2 F2: output n reported off by its last status word (r6as_edge): its restore is slow (no 20-ms step), the test does
 * not wait for it; Canon's on (its standard set, 4.3 b) or the next call ends it */
static int r6as_bk_off(const struct r6as_bk * k) { return k->have && k->key != 3; }

/* the result of one call on output n (4.3 - 4.7) */
static void r6as_t2_res(int n, int set, int lvl, int r, const struct r6as_wr * o, int now, int call)
{
    struct r6as_bk * k = &K[n];
    C2.calls++;
    r6as_lo[n] = *o;
    if (r == R6AS_WR_SHUT) { C2.shut++; return; }
    if (r == R6AS_WR_CORE) { C2.w_core++; return; }
    if (r == R6AS_WR_TAB) { C2.w_tab++; r6as_trip(R6AS_H6, n, now); return; }   /* no store, no restore from the tables (4.7 3) */
    if (r < 0) return;                                       /* ARG */
    r6as_edge(n, o->st, now);
    if (o->stored)
    {
        C2.stores++;
        if (r == R6AS_WR_DONE) C2.done++; else C2.back++;
        k->t_wr[k->i_wr] = now;                              /* the budget ring (4.6) */
        k->i_wr = (k->i_wr + 1) % R6AS_WR_N;
        k->ev = 1; k->t_st = now; k->ev_back = r == R6AS_WR_BACK;   /* judged by a later call >= R6AS_EVAL_MS after it (4.5) */
        k->ev_set = set; k->ev_lvl = lvl;                    /* review R1 F1: judged against this target only */
        k->dev = k->off_seen = k->clsd = k->later = k->ver = 0;
        if (set == R6AS_SET_RED) { k->pend = 1; k->why_next = R6AS_C_RED; k->one = k->tried = k->h4 = 0; }
        else
        {
            C2.rst_stores++;
            k->ch0 = o->chan != 2;                           /* channel state != 2: likely not taken until it runs (S2 3.3) */
            if (k->ch0) { C2.rst_ch0++; call = R6AS_C_RESTORE_CH0; }
            if (k->one) k->tried = 1;                        /* the one try after an error off: a call with a store (S1 F4) */
        }
        r6as_wl_add(n, call, set, lvl, r, now);
        if (r6as_probe && T)                                 /* the probe: trace trigger 6 / 7, its B048 value follows ML's store */
        {
            uint32_t b = set == R6AS_SET_RED ? 0 : r6as_std[0];
            r6as_trigger(now, 6 + n, r6as_b048[n], b);
            r6as_b048[n] = b;
            r6as_b048_have[n] = 1;
        }
        return;
    }
    if (r == R6AS_WR_NONE) C2.none++;
    else if (o->why) (&C2.w_off)[o->why <= R6AS_Y_CPU ? o->why - 1 : o->why]++;
    if (o->cls == R6AS_CLS_NA)                               /* the words not read (output off, other core) */
    {
        if (k->ev && k->key != 3) { k->ev = 0; r6as_bounce(n, now); }   /* off after a store: a device change (4.5) */
        return;
    }
    k->cls = o->cls;
    if (o->why == R6AS_Y_UNK)
    {
        if (set == R6AS_SET_RED) { r6as_trip(R6AS_H2, n, now); return; }   /* unknown values on the target output (4.7) */
        if (++k->unk_n >= R6AS_RST_TRY) r6as_h4(n);          /* restore: no store; H4 after 3 calls, then every 250 ms (4.3) */
        return;
    }
    k->unk_n = 0;
    k->clsd = 1;
    if (k->h4 && !(k->one && k->tried)) k->h4 = 0;
    int tgt = o->cls == (set == R6AS_SET_RED ? R6AS_CLS_RED : R6AS_CLS_STD);
    if (k->ev && (k->ev_set != set || (set == R6AS_SET_RED && k->ev_lvl != lvl)))
    {
        /* review R1 F1 / R2 F1: the target changed since the store (a level, Full / Off, the test's end): this call is not
         * classified against the store's own target, so the judgement ends without an entry (run unchanged); the new
         * target follows below (4.4 / 4.5 Nachtrag) */
        k->ev = 0;
        C2.ev_chg++;
    }
    if (k->ev)                                               /* the judging call after a store (4.5) */
    {
        if (!tgt)
        {
            if (now - k->t_st < R6AS_EVAL_MS) return;        /* at the earliest 50 ms after the store (S1 F9) */
            k->ev = 0;
            if (k->dev) { r6as_bounce(n, now); k->why_next = R6AS_C_REWRITE; return; }   /* a device change since the store */
            if (k->ev_back) C2.nb_eff_back++; else C2.nb_eff_done++;
            if (set == R6AS_SET_STD && k->one) { r6as_h4(n); return; }   /* the one try after an error off failed */
            k->run++;
            if (!r6as_nb(n, 0, now) && k->run >= R6AS_BACK_MAX) r6as_trip(R6AS_H3, n, now);
            return;
        }
        k->ev = 0;
        if (k->ev_back) C2.back_late++;                      /* a shadow register: the target came later, no entry */
    }
    if (tgt)
    {
        if (set == R6AS_SET_RED)
        {
            if (!k->ver || k->ver_lvl != lvl)
            {
                k->ver = 1; k->ver_lvl = lvl; k->t_ver = now;
                if (r6as_tst == R6AS_T_RED && !n && !r6as_t_v10) r6as_t_v10 = now;   /* the test's 10 s start (4.8) */
            }
            k->dev = 0; k->run = 0;
        }
        else if ((o->l2f76 == (uint32_t) n && o->chan == 2) || k->off_seen)   /* 4.3: (a) the running state, (b) off since the store */
        {
            if (k->ch0) C2.rst_ch0_ok++;
            k->pend = k->later = k->one = k->tried = k->h4 = k->ch0 = 0;
            C2.rst_done++;
        }
        else k->later = 1;                                   /* standard at channel state != 2: "check later" (S2 3.3) */
        return;
    }
    if (set == R6AS_SET_RED)
    {
        if (k->ver && lvl == k->ver_lvl)                     /* a Canon reset after a verified target, also during WAIT (S1 F10) */
        {
            k->ver = 0;
            k->why_next = R6AS_C_REWRITE;
            if (k->dev) r6as_bounce(n, now);
            else { C2.rs_nodev++; r6as_nb(n, 1, now); }
        }
        return;
    }
    if (k->later && o->l2f76 == (uint32_t) n && o->chan == 2) { C2.rst_ch0_red++; k->later = 0; }   /* still red at channel 2 */
    if (!k->later && k->one && k->tried) r6as_h4(n);         /* after an error off: no second store (4.7 2) */
}

/* the read-only peek (4.9): log only */
static void r6as_peek(int n, int r, const struct r6as_wr * o, int now)
{
    C2.peeks++;
    if (r >= 0) r6as_edge(n, o->st, now);
    for (int j = 0; j < 2; j++)
    {
        struct r6as_pk * p = &T->pk[n][j];
        if (!j && p->t) continue;
        p->t = now; p->res = r; p->why = o->why; p->cls = r >= 0 ? o->cls : R6AS_CLS_NA; p->cpu = o->cpu;
        p->st = o->st; p->dm = o->dm; p->l1615 = o->l1615; p->l2f76 = o->l2f76; p->chan = o->chan;
        memcpy(p->w, o->w, sizeof(p->w));
    }
}

/* the states (4.1, 4.8, 5.4): every task step, no MMIO; 1 = Tor 2 needs its step (Full, the test red / restoring, a
 * restore pending) */
static int r6as_t2_state(int now)
{
    int m = r6as_cfg(r6as_mode, 2);
    __sync_synchronize();                                    /* the menu: sequence number, barrier, then the mode */
    int fs = r6as_full_seq, ts = r6as_test_seq;
    if (!r6as_t2_init)                                       /* the rings start "long ago" (S1 F8: never reset) */
    {
        for (int n = 0; n < 2; n++)
        {
            struct r6as_bk * k = &K[n];
            for (int i = 0; i < R6AS_WR_N; i++) k->t_wr[i] = now - 2 * R6AS_WR_WIN_MS;
            for (int i = 0; i < R6AS_FLK_N; i++) k->t_nb[i] = now - 2 * R6AS_FLK_WIN_MS;
            for (int i = 0; i < R6AS_BNC_RING; i++) k->t_bn[i] = now - 2 * R6AS_BNC_WIN_MS;
            k->t_flank = k->bnc_until = now;
            k->cls = R6AS_CLS_NA;
        }
        r6as_dm2 = r6_gd_disp_mask();
        r6as_t_dm2 = now;
        r6as_t2_init = 1;
    }
    /* review R1 F4: a command seen with the old mode (the menu between the task's two reads) stays for the next step */
    if (fs != r6as_seen_fs && m == 2)                        /* a Full command (Mode.select), taken with mode 2 only (4.1 2) */
    {
        r6as_seen_fs = fs;
        if (!r6as_t2_block) { r6as_armed = 1; r6as_cmd_any = 1; r6as_boot_dn = 0; }
    }
    if (m != 2 || r6as_t2_block) r6as_armed = 0;             /* Off / ML only: the restore follows (want_red 0) */
    if (m == 2 && !r6as_armed)                               /* invariant (S1 F7): no Full without a command (config load, start) */
    {
        r6as_mode = m = 1;
        if (!r6as_cmd_any) r6as_boot_dn = 1;
    }
    r6as_full_on = r6as_armed;
    if (ts != r6as_seen_ts)                                  /* a "Test: 10 s" command */
    {
        r6as_seen_ts = ts;
        if (r6as_full_on) r6as_test_txt = "Full is on";
        else if (r6as_t2_block) r6as_test_txt = "blocked";
        else if (r6as_tst != R6AS_T_IDLE) r6as_test_txt = "busy";
        else if (!r6as_wl) r6as_test_txt = "no memory";
        else
        {
            r6as_tst = R6AS_T_WAIT;
            r6as_t_t0 = now; r6as_t_ok = 0; r6as_t_v10 = 0; r6as_t_mode = m; r6as_t_why = 0;
            r6as_cmd_any = 1; r6as_boot_dn = 0;
            C2.tests++;
            r6as_wl_add(0, R6AS_C_TEST, R6AS_SET_RED, r6as_cfg(r6as_lcd, 5), 0, now);
        }
    }
    if ((r6as_tst == R6AS_T_WAIT || r6as_tst == R6AS_T_RED) && (m != r6as_t_mode || ml_shutdown_requested))
    {
        r6as_t_why = ml_shutdown_requested ? "stopped: shutdown" : "stopped: mode";
        r6as_tst = ml_shutdown_requested ? R6AS_T_IDLE : R6AS_T_REST;   /* no restore once ML shuts down (Canon: power off) */
        if (r6as_tst == R6AS_T_IDLE) r6as_test_txt = r6as_t_why;
    }
    if (r6as_tst == R6AS_T_WAIT)                             /* the LCD shooting screen, held 500 ms (4.8) */
    {
        if (lp_shooting(0x01)) { if (!r6as_t_ok) r6as_t_ok = now; }
        else r6as_t_ok = 0;
        if (r6as_t_ok && now - r6as_t_ok >= R6AS_TEST_HOLD_MS) { r6as_tst = R6AS_T_RED; r6as_t_red0 = now; K[0].ver = 0; }
        else if (now - r6as_t_t0 > R6AS_TEST_WAIT_MS) { r6as_tst = R6AS_T_IDLE; r6as_test_txt = "no LCD screen"; }
    }
    else if (r6as_tst == R6AS_T_RED && (r6as_t_v10 ? now - r6as_t_v10 >= R6AS_TEST_MS : now - r6as_t_red0 > 2 * R6AS_TEST_MS))
    {
        if (!r6as_t_v10) r6as_t_why = "no red";
        r6as_tst = R6AS_T_REST;
        r6as_wl_add(0, R6AS_C_TESTEND, R6AS_SET_STD, 0, 0, now);
    }
    if (r6as_tst == R6AS_T_REST && !(K[0].pend && !K[0].h4 && !r6as_bk_off(&K[0])) && !(K[1].pend && !K[1].h4 && !r6as_bk_off(&K[1])))
    {
        r6as_tst = R6AS_T_IDLE;                              /* review R2 F2: an output that is off is restored later (4.3 b) */
        r6as_test_txt = r6as_t_why ? r6as_t_why : (K[0].pend && K[0].h4) || (K[1].pend && K[1].h4) ? "stopped: no restore"
            : K[0].pend || K[1].pend ? "done (restore later)" : "done";
    }
    r6as_lv[0] = r6as_cfg(r6as_lcd, 5);                      /* the levels: one copy per step (S1 F10) */
    r6as_lv[1] = r6as_cfg(r6as_evf, 5);
    r6as_t2_need = !r6as_t2_tab && (r6as_full_on || r6as_tst >= R6AS_T_RED || K[0].pend || K[1].pend);
    return r6as_t2_need;
}

/* Tor 2 per step (4.2): the red set on the output shown, the restore, the peek, the status of the other outputs; every
 * call of r6as_mmio_wr is here */
static void __attribute__((noinline)) r6as_t2_step(int now)
{
    struct r6as_wr o;
    uint32_t dm = r6_gd_disp_mask();
    int fast = 0;
    r6as_w_begin();
    if (dm != r6as_dm2) { K[0].dev = K[1].dev = 1; K[0].clsd = K[1].clsd = 0; r6as_dm2 = dm; r6as_t_dm2 = now; }
    r6as_tn = -1;
    for (int n = 0; n < 2; n++)
    {
        if (r6as_t2_tab) break;                              /* review R1 F3: after a TAB no further access in this step (4.7 3) */
        struct r6as_bk * k = &K[n];
        const uint32_t sel = n ? 0x10 : 0x01;
        int acc = 0, lvl = r6as_lv[n];
        int want = r6as_full_on || (r6as_tst == R6AS_T_RED && !n);
        if (k->ev && now - k->t_st >= R6AS_EVAL_MAX_MS) { k->ev = 0; C2.ev_lost++; }
        if (!want) k->ver = 0;
        if (r6as_t2_need && want && dm == sel)
        {
            /* red on the output shown (4.4): a level change is a new target; a verified target is only read (a reset is
             * seen first, the store follows in the next step); budget, bounce pause, judging call (4.5) */
            if (k->ver && lvl != k->ver_lvl) { k->ver = 0; k->why_next = R6AS_C_LEVEL; }
            int may = !k->ev && !k->ver && now - k->t_wr[k->i_wr] >= R6AS_WR_WIN_MS && now - k->bnc_until >= 0;
            memset(&o, 0, sizeof(o));
            int r = r6as_mmio_wr(n, R6AS_SET_RED, lvl, may, &o);
            r6as_t2_res(n, R6AS_SET_RED, lvl, r, &o, now, k->why_next);
            r6as_tn = n;
            acc = fast = 1;
        }
        else if (r6as_t2_need && want && !n && !r6as_full_on) { C2.w_dm++; fast = 1; }   /* the test: the LCD not shown, red waits (4.8) */
        else if (r6as_t2_need && k->pend && !want)
        {
            /* the restore (4.3): by the status bits, also the output not shown (classified first), also later */
            int slow = r6as_bk_off(k) || k->later || k->h4 || (k->one && k->tried && !k->ev);
            if (!slow || now - k->t_slow >= R6AS_RST_SLOW_MS)
            {
                k->t_slow = now;
                int may = !slow && !k->ev && now - k->t_wr[k->i_wr] >= R6AS_WR_WIN_MS && (dm == sel || k->clsd);
                memset(&o, 0, sizeof(o));
                int r = r6as_mmio_wr(n, R6AS_SET_STD, 0, may, &o);
                r6as_t2_res(n, R6AS_SET_STD, 0, r, &o, now, R6AS_C_RESTORE);
                acc = 1;
            }
            if (!slow || k->ev) fast = 1;
        }
        if (!acc && r6as_probe && T && dm != sel && now - k->t_pk >= R6AS_PEEK_MS)
        {
            /* the read-only peek at the output not shown (4.9, E2: <= 4 / s): may is the literal 0 */
            k->t_pk = now;
            memset(&o, 0, sizeof(o));
            int r = r6as_mmio_wr(n, R6AS_SET_STD, 0, 0, &o);
            r6as_peek(n, r, &o, now);
            acc = 1;
        }
        if (!acc && r6as_t2_need && (dm != sel || now - k->t_rd >= R6AS_RST_SLOW_MS))
        {
            struct r6as_rd rr;                               /* the status of an output not called (4.2 5) */
            k->t_rd = now;
            if (r6as_mmio_rd(n, &rr) >= 0) r6as_edge(n, rr.st, now);
        }
    }
    r6as_t2_fast = fast;
    r6as_w_end();
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

/* "Display regs" switched ON: the probe starts again (its core check comes first, r6as_core_step) */
static void r6as_probe_reset(int now)
{
    r6as_probe_new = 0;
    r6as_tr = 0; r6as_need_tick = 0; r6as_have = 0; r6as_b048_have[0] = r6as_b048_have[1] = 0;
    r6as_dm_last = r6_gd_disp_mask();
    r6as_t_dm = now;
    r6as_core_have = 0;
    r6as_t_tick = now - R6AS_TICK_MS;
    r6as_tr_end = now - R6AS_TR_GAP_MS;
}

/* spec m12/m121 4.2 (S1 F4): only the astro task writes r6as_core_ok. Needed while "Display regs" is ON or Tor 2 needs it;
 * checked anew at every activation before the first access, then every 10 s; not needed: 0 */
static void r6as_core_step(int now, int t2n)
{
    if (t2n && !r6as_t2n_was) r6as_core_have = 0;
    r6as_t2n_was = t2n;
    if (!r6as_probe && !t2n) { r6as_core_ok = 0; r6as_core_have = 0; return; }
    if (!r6as_core_have || now - r6as_t_core >= R6AS_CORE_RECHECK_MS || now < r6as_t_core)
    {
        struct r6as_tk d, s;
        int ok = r6as_task_core(&d, &s);
        r6as_core_ok = ok;
        if (!(r6as_probe || t2n) || r6as_probe_new) r6as_core_ok = 0;   /* review R1 L3: OFF (or OFF + ON) during the scan wins */
        if (r6as_core_have && r6as_core_was && !r6as_core_ok) C.core_lost++;
        r6as_core_was = r6as_core_ok;
        r6as_core_have = 1;
        r6as_t_core = now;
    }
}

static void r6as_probe_step(int now)
{
    struct r6as_rd r[2];
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
        msleep(r6as_t2_fast ? R6AS_T2_STEP_MS : r6as_probe && r6as_tr ? R6AS_TR_STEP_MS : R6AS_STEP_MS);
        int now = get_ms_clock();
        uint32_t ct = r6as_ram(R6AS_CUR_TASK);               /* the astro task itself (log ast.cpu) */
        r6as_my_cpu = get_cpu_id();
        if (r6_fy_ram_ok(ct, R6AS_TASK_SIZE)) { r6as_my_req = r6as_byte(ct + 0x50); r6as_my_asg = r6as_byte(ct + 0x51); }
        /* 2.4: the transparent ML menu view stays off while Astro red is on (ML functions only) */
        if (r6as_cfg(r6as_mode, 2) && menu_active_but_hidden())
        {
            menu_disable_lv_transparent_mode();
            menu_set_dirty();
            C.tr_off++;
        }
        r6as_key_step(now);
        r6as_w_begin();                                      /* review R2 F5: the states under the seqlock too (4.2 6) */
        int t2n = r6as_t2_state(now);                        /* M12.1: Tor 2 states (no MMIO) */
        r6as_w_end();
        if (r6as_probe && T && r6as_probe_new) r6as_probe_reset(now);
        r6as_core_step(now, t2n);
        if (!r6as_t2_tab && (t2n || (r6as_probe && T))) r6as_t2_step(now);   /* M12.1: Tor 2 and the peek */
        else r6as_t2_fast = 0;
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
    if (on && !r6as_probe) r6as_probe_new = 1;               /* M12.1 (S1 F4): r6as_core_ok is the astro task's (r6as_core_step) */
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

/* ---- M12.1 Tor 2 lines (6.2) ---- */
static const char * const __attribute__((section(".data"))) r6as_cn[5] = { "std", "red", "mix", "unk", "na" };   /* .data, see r6as_addr */
static const char * const __attribute__((section(".data"))) r6as_wn[12] = { "red", "rewrite", "level", "restore", "restore_ch0", "test",
    "testend", "trip", "reset", "noeff", "bounce", "tab" };
static int r6as_in(const int * t, int k, int now, int win) { int c = 0; for (int i = 0; i < k; i++) c += now - t[i] < win; return c; }
static uint32_t r6as_ky(int l) { int p = r6as_lvl_pct[l]; return p == 12 ? 0x200 : p * 4096 / 100; }   /* plan 5.2: 12 % = 0x0200 */

static void r6as_t2_text(struct r6as_out * o)
{
    int now = get_ms_clock();
    uint32_t v[16] = { r6as_cfg(r6as_mode, 2), r6as_armed, r6as_full_on, r6as_t2_block | (r6as_t2_tab << 1), r6as_boot_dn, r6as_tst, r6as_tn,
                       r6as_ky(r6as_cfg(r6as_lcd, 5)), r6as_ky(r6as_cfg(r6as_evf, 5)), K[0].pend, K[1].pend, K[0].ver, K[1].ver, r6as_err_n,
                       r6as_last_err, r6as_core_ok };
    r6as_arr(o, "ast.t2", v, 16, 0x180);
    r6as_arr(o, "ast.t2cnt", &C2.calls, sizeof(C2) / 4, 0);
    for (int n = 0; n < 2; n++)
    {
        const struct r6as_bk * k = &K[n];
        r6as_pf(o, "ast.t2b.%s=%s,0x%x,%d,%d,%d,%d,%d,%d,%d,%d\n", r6as_bn[n], r6as_cn[k->cls % 5], (int) k->st, k->t_ver, k->t_st,
            r6as_in(k->t_wr, R6AS_WR_N, now, R6AS_WR_WIN_MS), k->run, r6as_in(k->t_nb, R6AS_FLK_N, now, R6AS_FLK_WIN_MS), k->ev, k->later,
            r6as_in(k->t_bn, R6AS_BNC_RING, now, R6AS_BNC_WIN_MS));
        for (int j = 0; T && j < 2; j++)
        {
            const struct r6as_pk * p = &T->pk[n][j];
            if (!p->t) continue;
            r6as_pf(o, "ast.pk.%s.%s=%d,%d,%d,%s,0x%x,0x%x,%d,%d,%d,%d", r6as_bn[n], j ? "last" : "first", p->t, p->res, p->why,
                r6as_cn[p->cls % 5], (int) p->st, (int) p->dm, p->cpu, (int) p->l1615, (int) p->l2f76, (int) p->chan);
            for (int i = 0; i < R6AS_NW; i++) r6as_pf(o, ",0x%x", (int) p->w[i]);
            r6as_pf(o, "\n");
        }
    }
    const struct r6as_wl * L = r6as_wl;
    for (uint32_t i = r6as_wl_n > R6AS_WR_LOG ? r6as_wl_n - R6AS_WR_LOG : 0; L && i < r6as_wl_n; i++)
    {
        const struct r6as_wl * e = &L[i % R6AS_WR_LOG];
        r6as_pf(o, "ast.wr[%d]=%d,%d,%s,%d,%d,%d,%d,%s,0x%x,0x%x,%d,%d,%d,%d", (int) i, e->t, e->n, r6as_wn[e->call % 12], e->set, e->lvl,
            e->res, e->why, r6as_cn[e->cls % 5], (int) e->st, (int) e->dm, e->cpu, (int) e->l1615, (int) e->l2f76, (int) e->chan);
        for (int j = 0; j < 16; j++) r6as_pf(o, ",0x%x", (int) (j < 10 ? e->w[j] : e->back[j - 10]));
        r6as_pf(o, ",%d,%d\n", e->dfl, e->ddm);
    }
}

/* the tables (seqlock: at most 3 tries, then ast.torn=1) */
static int r6as_tabs_text(struct r6as_out * o)
{
    for (int tr = 0; tr < 3; tr++)
    {
        int l0 = o->len, t0 = o->trunc;
        uint32_t s1 = r6as_seq, v[17];   /* M12.1: 17 fields per trace point */
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
                    for (int x = 0; x < 8; x++) v[9 + x] = p->x[x >> 2][x & 3];
                    r6as_arri(o, "ast", k, j, v, 17, 0x1FFF8);
                }
            }
            r6as_t2_text(o);                                 /* M12.1: Tor 2 under the same seqlock (S1 F15 1) */
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
    r6as_pf(&o, "# ASTPROBE M12.1 (spec m12/m120 4.2, m12/m121 6.2)\n");
    v[0] = r6as_cfg(r6as_mode, 2); v[1] = r6as_lvl_pct[r6as_cfg(r6as_lcd, 5)]; v[2] = r6as_lvl_pct[r6as_cfg(r6as_evf, 5)];
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
    const char * t2 = r6as_t2_block ? "blk" : r6as_full_on ? "full" : r6as_tst ? "test" : K[0].pend || K[1].pend ? "rst" : r6as_err_n ? "err" : "off";
    return snprintf(s, n, "%d,%d,%d,%d,0x%x,%d,%s,%s,%s:%d", r6as_cfg(r6as_mode, 2), r6as_lvl_pct[r6as_cfg(r6as_lcd, 5)],
        r6as_lvl_pct[r6as_cfg(r6as_evf, 5)], r6as_probe, (int) r6_gd_disp_mask(), (int) get_cpu_id(), r6as_cls(0), r6as_cls(1), t2,
        (int) C2.stores);
}

/* ---- menu Display > "Astro red" (2.1; M12.1 5.2): the menu task never touches MMIO ---- */
/* the write log (heap): allocated, cleared, then published (S1 F15 3); 0 = no memory */
static int r6as_wl_alloc(void)
{
    if (r6as_wl) return 1;
    struct r6as_wl * p = malloc(R6AS_WR_LOG * sizeof(struct r6as_wl));
    if (!p) return 0;
    memset(p, 0, R6AS_WR_LOG * sizeof(struct r6as_wl));
    __sync_synchronize();
    r6as_wl = p;
    return 1;
}

static MENU_SELECT_FUNC(r6as_mode_select)
{
    int m = (r6as_cfg(r6as_mode, 2) + (delta < 0 ? 2 : 1)) % 3;
    if (m == 2)
    {
        if (r6as_t2_block) { r6as_mode = delta < 0 ? 1 : 0; r6as_t2_msg = r6as_hn[R6AS_H5]; return; }   /* review R2 F3: on past Full */
        if (!r6as_wl_alloc()) { r6as_mode = delta < 0 ? 1 : 0; r6as_t2_msg = "Astro red: no memory."; return; }
        r6as_t2_msg = 0;
        r6as_full_seq++;                                     /* the command; the task arms on it (4.1) */
        __sync_synchronize();
    }
    r6as_mode = m;
}

static MENU_SELECT_FUNC(r6as_test_select)
{
    if (!r6as_wl_alloc()) { r6as_test_txt = "no memory"; return; }
    r6as_t2_msg = 0;
    r6as_test_seq++;                                         /* the task decides (4.1 6) */
}

static MENU_UPDATE_FUNC(r6as_test_update)
{
    int t = r6as_tst, v = r6as_t_v10;
    if (t == R6AS_T_WAIT) MENU_SET_VALUE("waiting: LCD screen");
    else if (t == R6AS_T_RED && v) MENU_SET_VALUE("red %d s", MAX(0, (R6AS_TEST_MS - (get_ms_clock() - v) + 999) / 1000));
    else if (t == R6AS_T_RED) MENU_SET_VALUE("red ...");
    else if (t == R6AS_T_REST) MENU_SET_VALUE("restoring");
    else MENU_SET_VALUE("%s", r6as_test_txt);
}

static void r6as_bk_txt(char * b, int n)
{
    const struct r6as_bk * k = &K[n];
    if (k->ver) snprintf(b, 12, "red %d%%", r6as_lvl_pct[k->ver_lvl % 6]);
    else if (k->pend && k->later) snprintf(b, 12, "check later");
    else if (k->pend) snprintf(b, 12, "%s", r6as_full_on || r6as_tst == R6AS_T_RED ? "red ..." : "restoring");
    else snprintf(b, 12, "%s", r6as_cn[k->cls % 5]);
}

static MENU_UPDATE_FUNC(r6as_status_update)
{
    char a[12], b[12];
    int now = get_ms_clock();
    uint32_t w = 0;
    for (int i = 0; i < 11; i++) w += (&C2.w_off)[i];
    r6as_bk_txt(a, 0);
    r6as_bk_txt(b, 1);
    if (r6as_t2_block) MENU_SET_VALUE("blocked until restart");
    else if (now - K[0].bnc_until < 0 || now - K[1].bnc_until < 0) MENU_SET_VALUE("eye sensor busy");
    else MENU_SET_VALUE("LCD %s | EVF %s", a, b);
    if (r6as_t2_msg) MENU_SET_WARNING(MENU_WARN_ADVICE, "%s", r6as_t2_msg);
    else if (r6as_boot_dn) MENU_SET_WARNING(MENU_WARN_INFO, "Full is not restored at start (M12.1).");
    else MENU_SET_WARNING(MENU_WARN_INFO, "Writes %d, Canon resets %d / %d, waits %d.", (int) C2.stores, (int) C2.rs_dev, (int) C2.rs_nodev, (int) w);
}

static MENU_UPDATE_FUNC(r6as_pal_update)
{
    uint32_t pal = r6_astro_pal();
    if (pal & R6AS_PAL_GREY) MENU_SET_VALUE("grey (Full)");
    else if (!(pal & R6AS_PAL_RED)) MENU_SET_VALUE("colours");
    else MENU_SET_VALUE("red, %s %d%%", r6_gd_disp_mask() == 0x10 ? "EVF" : "LCD", (int) ((pal >> 8) & 0xFF));
    uint32_t p = pal & R6AS_PAL_RED ? pal : (R6AS_PAL_RED | (100 << 8)), r[3];
    for (int i = 0; i < 3; i++) r[i] = (r6_astro_map(p, ""[i], 0xFF000000) >> 16) & 0xFF;   /* text 1, bar 11, error 8 */
    MENU_SET_WARNING(MENU_WARN_INFO, "Text R %d, bar R %d, error R %d, black stays 1/1/1.", (int) r[0], (int) r[1], (int) r[2]);
}

static struct menu_entry r6as_menu[] = {
    {
        .name   = "Astro red",
        .select = menu_open_submenu,
        .help   = "M12.1: the display in red for night vision. Recordings stay in colour.",
        .children = (struct menu_entry[]) {
            {
                .name    = "Mode",
                .priv    = &r6as_mode,
                .max     = 2,
                .select  = r6as_mode_select,
                .choices = CHOICES("Off", "ML only", "Full"),
                .help    = "Full: the whole display red (LCD / EVF matrix). ML only: ML's pixels only. Recordings stay in colour.",
                .help2   = "Saved; Full returns as ML only at start (M12.1). Colour flash on eye sensor / display wake.",
            },
            {
                .name    = "LCD level",
                .priv    = &r6as_lcd,
                .max     = 5,
                .choices = CHOICES("100%", "50%", "25%", "12%", "150%", "200%"),
                .help    = "Red level on the LCD: Full = whole display (signal: 50 % ~ 20 % light), ML only = ML's pixels.",
                .help2   = "150/200 %: brighter, bright parts look the same.",
            },
            {
                .name    = "EVF level",
                .priv    = &r6as_evf,
                .max     = 5,
                .choices = CHOICES("100%", "50%", "25%", "12%", "150%", "200%"),
                .help    = "Red level in the viewfinder (Full: whole display).",
                .help2   = "150/200 %: brighter, bright parts look the same.",
            },
            {
                .name      = "Test: 10 s",
                .select    = r6as_test_select,
                .update    = r6as_test_update,
                .icon_type = IT_ACTION,
                .help      = "The LCD in red for 10 s, then back. Close the menu: starts on the LCD shooting screen.",
                .help2     = "Not in Full. Keep the eye off the EVF. Nothing is saved.",
            },
            {
                .name      = "Status",
                .update    = r6as_status_update,
                .icon_type = IT_ALWAYS_ON,
                .help      = "Full / test: the matrix state of LCD and EVF, the last notice.",
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
