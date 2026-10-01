/*
 * R6.190 M7.0: read-only LiveView census probe.
 *
 * Debug > "LV probe":
 *  - "LV flag follows Canon": lv = !PROP_LV_ACTION (propvalues.c,
 *    CONFIG_LV_FOLLOW_TOGGLE). Default off and not saved, so every boot
 *    starts with lv = 0 as in M6.
 *  - "Status line": a task prints 7 short lines (M7.2a: plus 6 small ones)
 *    with bmp_printf into ML's own pixels of Canon's layer 0 (as approved in
 *    M6), also while DISPLAY_IS_ON == 0, with a running counter. M7.2a: the
 *    text is composed once per second and only changed lines are printed
 *    (all lines every 5 s and after the ML menu was open), so an unchanged
 *    status line causes no ML refresh. "ON" leaves the refresh to
 *    redraw_task (only while DISPLAY_IS_ON); "ON, refresh" also runs ML's
 *    refresh_yuv_from_rgb (ML's XimrExe, as menu_redraw_flood does) while
 *    DISPLAY_IS_ON == 0, but only while Canon's display mask reports the EVF
 *    output as active (bit 4) and no shutdown is in progress (EVF check;
 *    not for the power-save display-off step).
 *  - "Log snapshot": closes the menu, waits 4 s, then writes two samples
 *    0.5 s apart and a 1 s VMIX poll to a new file ML/LOGS/LVPnnnnn.LOG
 *    (plain text, one key=value per line). One snapshot at a time.
 *
 * READ-ONLY. Plain RAM reads of Canon globals. A pointer taken from a Canon
 * structure is dereferenced only if it is non-zero, 4-byte aligned and the
 * whole access lies inside 0x1000..0x3FFFFFFF (cached RAM) or, only if
 * Canon's memif RAM copy is filled and windows 4..7 equal windows 0..3,
 * 0x40001000..0x7FFFFFFF (uncached alias). Page 0 is unmapped on R6 (L2[0] = 0
 * in the TTBR0 tables), so a NULL dereference is a data abort, not junk.
 * Raw pointer values are always logged. 64-bit values are read twice and
 * must match (Canon's lock 0xEAC4 is not taken).
 * One explicit exception to the RAM range: three TTBR1 L1 words at
 * 0xDFFC0000 + (va >> 20) * 4 (RAM, normal memory, read only).
 *
 * M7.2a extension (lp_census_m72a, more status lines): Canon's LV
 * histogram (context, published pointers; only if they form one of Canon's
 * four buffer sets: first and last 8 bins, 16 coarse buckets, sum and mean
 * bin of each array, 32-bit loads from Canon's uncached heap after the whole
 * 1 KB passed the range check), RandomData, FaceYuv pointers and headers (no
 * pixels), stage-0 preconditions of the dynamic memif window and of GS /
 * I2iDmac, VMIX/WINSYS flags (also sampled in the 1 s VMIX poll) and the
 * refresh guard counters (refresh_guard.c, only with CONFIG_R6_REFRESH_GUARD).
 * Same rules; the L1 exception now also covers the L1 words for VA
 * 0xC6000000 (0xDFFC3180) and VA 0xDFF00000 (0xDFFC37FC, the section that
 * holds the L1 table itself). The PSR mask word 0xDFFC4FA8 (not an L1 word)
 * is NOT read unless LP_READ_PSR_MASK is defined (needs the user's approval),
 * and then only if the L1 descriptor of its section says Normal memory.
 *
 * No Canon calls beyond what ML already uses: FIO through the ML wrappers,
 * msleep, task_create, vsnprintf, property handler registration via
 * PROP_HANDLER. No new MMIO by ML; existing M6 accesses: the DIGIC timer
 * 0xC0242014 (get_ms_clock/get_us_clock, read more often in this build) and
 * Canon's XimrExe (also from the status task in "ON, refresh").
 * No property writes, no EDMAC, no LV pixel reads, no memif window lookups
 * through Canon code.
 *
 * Addresses: ROM0 1.9.0. Evidence per field: see the comments below and
 * the M7.0 report.
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "property.h"
#include "propvalues.h"
#include "vram.h"
#include "fio-ml.h"
#include "mem.h"
#include "shoot.h"
#include "tasks.h"
#include "version.h"
#include <stdarg.h>
#ifdef CONFIG_R6_REFRESH_GUARD
#include "refresh_guard.h"
#endif

/* CONFIG_LIVEVIEW compiles in force_liveview (fake_simple_button(BGMT_LV) in
 * a loop while lv == 0) and the LV AF frame code. With lv pinned to 0 by
 * default, these features must stay off on R6.190 until LV is understood. */
#ifdef CONFIG_LV_FOLLOW_TOGGLE
#if defined(FEATURE_FORCE_LIVEVIEW) || defined(FEATURE_MOTION_DETECT) || defined(FEATURE_FOCUS_STACKING) \
 || defined(FEATURE_GHOST_IMAGE) || defined(FEATURE_LV_ZOOM_SETTINGS) || defined(FEATURE_LV_FOCUS_BOX_SNAP)
#error "R6.190: LV features that call force_liveview / move the AF frame are not approved (M7.0)"
#endif
#endif

#ifdef FEATURE_LV_BUFFER_PROBE

#ifdef LV_PROBE_DUMP_IBUS
#error "LV_PROBE_DUMP_IBUS (WriteIBus card dump) is not part of M7.0"
#endif

#if !defined(CONFIG_LIVEVIEW) || !defined(CONFIG_LV_FOLLOW_TOGGLE)
#error "lvprobe.c needs CONFIG_LIVEVIEW and CONFIG_LV_FOLLOW_TOGGLE (platform/R6.190/internals.h)"
#endif

/* propvalues.c (CONFIG_LV_FOLLOW_TOGGLE) */
extern int lv_follow_canon_get(void);
extern void lv_follow_canon_set(int on);
extern int lv_action_raw_get(uint32_t * value, int * len);

/* ---- Canon RAM addresses, R6 1.9.0 ------------------------------------- */
#define LP_GUI_MODE      0x8D70    /* CURRENT_GUI_MODE, GetGUIMode 0xE079A692 (consts.h) */
#define LP_DISPLAY_IS_ON 0xEF48    /* DispOperator_PropertyMasterSetDisplayTurnOffOn 0xE0751460 (consts.h) */
#define LP_LVAPP_DIALOG  0x1624C   /* LiveViewApp_dialog (stubs.S, zebra.c liveview_display_idle) */
#define LP_LEVEL_DIALOG  0x2138C   /* StartLiveViewLevelApp 0xE0AD4F52 stores its dialog here */
#define LP_SILENT_DIALOG 0x20734   /* StartLiveViewSilentShutterApp 0xE0AD278A stores its dialog here */
#define LP_LV_OVERLAYS   0x20F70   /* LV_OVERLAYS_MODE, LvInfoToggle_Update 0xE08C1B90 (consts.h) */
#define LP_DISPDEV_PTR   0xEA40    /* 0xE01694C8: return [[0xEA40]+0x18] = display mask (bit0 PNL, bit4 EVF, 0x1100 LINE) */
#define LP_MASK_EVF      0x10
#define LP_GUI_PTR       0x16954   /* GUI_SetLvAction 0xE07AA730: str value -> [[0x16950+4]+0x1828] */
#define LP_DVC_PTR       0xEA60    /* DISP_GetImageAddress 0xE07B1054 -> 0xE025DA2A, ...Ready -> 0xE025DB74 */
#define LP_VRAMMGR_MODE  0xEA94    /* DispVramMgr mode (literal in 0xE0184872, names via 0xE01845D2) */
#define LP_VRAMMGR_IDX   0xEAE0    /* 0xEA94+0x4C: 4 RscMgr2 entry indices (0xE01848E6) */
#define LP_IMG_STRUCTS   0x1239E0  /* 5 x ptr to 0x70-byte output state (DispVramState 0xE01857B2) */
#define LP_RSC_HANDLE    0x123A68  /* 0x1239E0+0x88: RscMgr2 work handle per type (0xE01848E2) */
#define LP_GRPCTR_PTR    0x11F34   /* LvGraphicController 0xE02997BE: +0x70/+0x74 last panel VRAM */
#define LP_MEMIF_WIN     0x1355A4  /* RAM copy of the 12 static windows, MMIO 0xD2144080.. (copy 0xE089F230);
                                    * word [11] (+0x2C) doubles as the 'copied' flag (0xE089F26C) */
#define LP_DYNWIN        0x7250    /* 4 x 0x18 dynamic window state (MemifDynamicWindow 0xE01AF30A) */
#define LP_MEM1PATH      0x1EFF0   /* LiveViewDrive::Mem1Path.c state block 0x1EFF0..0x1F06F (getters 0xE037C00E..) */
#define LP_LV_SAVE_RAW   0x302FC   /* lv_save_raw eventproc 0xE0160340 sets it */
#define LP_VMIX          0x18404   /* VMIX_TransferRectangleToVram 0xE0260A16: +8 xcmNum, +0xC outputNum,
                                    * +0x10 buffer index (rsb #1 at 0xE0260CB2), +0x18 pending flag */
#define LP_XCM_TABLE     0x14B2D8  /* 3 XCM handles (VMIX_InitializeScreen 0xE026069E); [xcm+0xC] = context
                                    * count (XCM_GetContext 0xE0175274), [xcm+0x1700+l*0x24] = layer MARV (0xE01755B0) */
#define LP_XOC_TABLE     0x14B2E4  /* XOC handles [xcm*4+out] (VMIX_SetLayerEnable 0xE0260F66),
                                    * [xoc+4] = layer enable bits (XOC_GetLayerEnable 0xE019E724) */
#define LP_RENDERER      0x14B130  /* 2 x {MARV*, w, h} (RENDERER_InitializeScreen 0xE0233CC6) */
#define LP_TTBR1         0xDFFC0000 /* TTBR1 L1 table (QEMU snapshot: 0x80000000 -> 0x80054402, 0xBF000000 -> 0xBF050416) */

/* M7.2a census extension (decision doc docs/m7_2_wegb.md), RAM reads only */
#define LP_HIST_STATE    0x11F44   /* LvHistController: +4 = ctx (ldr =0x11F44 in HistDisp 0xE012C0B8, ldr [r4,#4]),
                                    * +0xC != 0: the ALO set is delivered as DISP (0xE012C372) */
#define LP_HIST_CTX_SIZE 0xD0      /* ctx is 0xD0 bytes (init 0xE012BC56, from eppInit 0xE010F39A) */
                                   /* ctx+4..+0x18 published payload {numPix?, p0..p3, info = ctx+0x70}: HistDisp
                                    * stores 0xE012C0BE..0xE012C0D6, prop_deliver 0x8005000F, 0x18 bytes.
                                    * "numPix" = ctx[0xC4]*ctx[0xC8] (0xE012C646) is a hypothesis.
                                    * 16 arrays of 256 u32 in a 0x4000-byte uncached block (0xE07BD4E4 ->
                                    * GetUncacheableMemoryAddress 0xE08DA940): ALO set 0/1 ctx+0x28../+0x2C..,
                                    * DISP set 0 ctx+0x4C/54/5C/64, set 1 ctx+0x50/58/60/68 (stride 8) */
#define LP_DTS           0x1200C   /* LV DataStore state: +0x10 use working copy, +0x18/+0x1C ring index
                                    * (copy 0xE079C626, getter DTS_GetAllRandomData 0xE079D026) */
#define LP_RD_WORK       0x131B70  /* RandomData working copy, 0xF00 bytes (literal at 0xE079C640) */
#define LP_RD_RING       0x132A70  /* published RandomData pointers, [LP_RD_RING + 4*idx] (0xE079C642) */
#define LP_RD_RING_N     8         /* entries logged; Canon's ring size is unknown */
#define LP_RD_SIZE       0xF00     /* 'RandomData Total(%d)' 0xE079C2B6 */
#define LP_FY_STATE      0x1ED68   /* LvFaceYuvController: +0xC = ctx (0xB060 bytes, 0xE0808F32), s8 [+0] ring
                                    * index; ctx+0x20: 4 x 64-bit IBus addresses (0xE08092DC..0xE0809386) */
#define LP_FY_CTX_SIZE   0xB060
#define LP_FY_RING       0x164614  /* 3 x {flag, CPU address} (per-frame step 0xE0809212, alias 0xE08090C4) */
#define LP_LVIC_SIZE     0x11FC8   /* u16 w, u16 h (QEMU snapshot 0x01AA0280 = 640 x 426) */
#define LP_MEMIF_DBG     0x7248    /* != 0: SetTarget 0xE01AF274 runs the arbiter and PSR checks (literal 0xE01AF384) */
#define LP_ARB           0x13290   /* MemifArbiter: [0x13290] == 0xFF -> other branch (0xE081DD0E);
                                    * pid byte for DRAM ch = 0x13290 + (ch-1)*0x16F + pid (0xE081DD1C) */
#define LP_ARB_CH3_PID5  0x13573   /* = 0x13290 + 2*0x16F + 5 */
#define LP_PSR_MASK      0xDFFC4FA8 /* bit ch-1 = DRAM channel in partial self-refresh (0xE013311C, check
                                    * MemifSelfRefresh 0xE0865560). Not an L1 word: in the DF on-chip area
                                    * next to CPU1 code/stack (QEMU: pc 0xDFFC491C, sp 0xDFFC5400) */
/* #define LP_READ_PSR_MASK */      /* off: reading LP_PSR_MASK needs the user's approval (M7.2a review) */
#define LP_L1_DF         (LP_TTBR1 + (0xDFF00000 >> 20) * 4)   /* 0xDFFC37FC: L1 word of the DF section */
#define LP_DWW_PTR       0xFE988   /* DWindowWrapper handle (init 0xE02A4B14): +0 slot, +0x20 in_use (Acquire 0xE02A4B40) */
#define LP_GS            0x106EBC  /* GS: +4*drv semaphore, +0xC+4*drv table, drv 0..2 (PrepareGSAddrTbl 0xE03C5510) */
#define LP_CH_LIMIT      0x106484  /* DRAM channel end ch1..5 (I2iDmac checks 0xE03EF6B8..; MemifApiInit 0xE01053BC) */
#define LP_I2I           0x72C8    /* I2iDmac channels 12 x 0x20 (ROM .data 0xE1FC1B78): +0xC MMIO base, +0x10 init,
                                    * +0x14/+0x18/+0x1C semaphores */
#define LP_GS_PRIO       0x7698    /* +4: chain priority passed by SyncGSAddrTbl (0xE03C5854) */
#define LP_WINSYS        0x18430   /* +8 transfer request (0xE07B8C88), +0xC draw-disable counter (0xE07B8D1E),
                                    * +0x10 m_fDisableRefresh (flush 0xE07B8C8E) */
#define LP_DTC_SEQ       0x21A58   /* display type change sequence (0xE078A77A / 0xE078A7F0) */
#define LP_GUI_DISPTYPE  0x8D0C    /* GUI display type ([0x8D00+0xC], GUI_SetDisplayType 0xE0799D48/0xE0799DA6) */

/* ---- guarded reads ------------------------------------------------------ */
#define RD(a) (*(volatile uint32_t *)(a))

/* 1 = uncached alias accepted: Canon's memif RAM copy is filled (word [11]
 * != 0) and windows 4..7 (PA 0x40000000..) equal windows 0..3 (PA 0..), i.e.
 * the uncached alias maps the same DRAM. Recomputed before every sample. */
static volatile int lp_uc_ok = 0;

static void lp_uc_update(void)
{
    int ok = RD(LP_MEMIF_WIN + 0x2C) != 0;
    for (int i = 0; i < 4; i++)
        if (RD(LP_MEMIF_WIN + 4 * (i + 4)) != RD(LP_MEMIF_WIN + 4 * i)) ok = 0;
    lp_uc_ok = ok;
}

static int lp_is_uncached(uint32_t a) { return a >= 0x40000000 && a < 0x80000000; }

static int lp_region(uint32_t a)
{
    if (a >= 0x1000 && a < 0x40000000) return 1;                  /* cached RAM, page 0 unmapped */
    if (a >= 0x40001000 && a < 0x80000000 && lp_uc_ok) return 2;  /* uncached alias, verified */
    return 0;
}

/* [a, a+n) aligned and inside one allowed region */
static int ram_ok(uint32_t a, uint32_t n)
{
    if ((a & 3) || !n) return 0;
    int r = lp_region(a);
    return r && a + n - 1 > a && lp_region(a + n - 1) == r;
}

static int rd32(uint32_t a, uint32_t * v)
{
    if (!ram_ok(a, 4)) return 0;
    *v = RD(a);
    return 1;
}

/* 1 = ok, -1 = changed between the two reads (4 tries), 0 = not readable */
static int rd64(uint32_t a, uint32_t * lo, uint32_t * hi)
{
    if (!ram_ok(a, 8)) return 0;
    for (int i = 0; i < 4; i++)
    {
        uint32_t l1 = RD(a), h1 = RD(a + 4);
        uint32_t l2 = RD(a), h2 = RD(a + 4);
        *lo = l2; *hi = h2;
        if (l1 == l2 && h1 == h2) return 1;
    }
    return -1;
}

/* field [base+off] of a Canon structure; base is the raw pointer value */
static int fld(uint32_t base, uint32_t off, uint32_t * v)
{
    return base && ram_ok(base, 4) && rd32(base + off, v);
}

/* ---- PROP_LV_ACTION ring (own handler; the ID is registered anyway) ---- */
#define LP_RING 16
static volatile uint32_t lp_ring_t[LP_RING];
static volatile uint32_t lp_ring_v[LP_RING];
static volatile uint32_t lp_ring_len[LP_RING];
static volatile uint32_t lp_ring_count = 0;    /* deliveries since boot */

PROP_HANDLER(PROP_LV_ACTION)
{
    uint32_t n = lp_ring_count;
    lp_ring_t[n % LP_RING] = get_ms_clock();
    lp_ring_v[n % LP_RING] = buf[0];           /* 32-bit word as ML reads it */
    lp_ring_len[n % LP_RING] = len;            /* delivered length (lv = !buf[0] assumes 4) */
    lp_ring_count = n + 1;
}

/* ---- refresh_yuv_from_rgb timing (bmp.c hook) -------------------------- */
static volatile uint32_t lp_ref_n, lp_ref_conv, lp_ref_wait, lp_ref_ximr;
static volatile uint32_t lp_ref_conv_max, lp_ref_wait_max, lp_ref_ximr_max;
static volatile uint64_t lp_ref_conv_sum, lp_ref_wait_sum, lp_ref_ximr_sum;

/* conv = indexed -> RGBA conversion, wait = take_semaphore(winsys_sem),
 * ximr = XimrExe + give_semaphore */
void lvprobe_refresh_timing(uint32_t conv_us, uint32_t wait_us, uint32_t ximr_us)
{
    lp_ref_conv = conv_us;
    lp_ref_wait = wait_us;
    lp_ref_ximr = ximr_us;
    if (conv_us > lp_ref_conv_max) lp_ref_conv_max = conv_us;
    if (wait_us > lp_ref_wait_max) lp_ref_wait_max = wait_us;
    if (ximr_us > lp_ref_ximr_max) lp_ref_ximr_max = ximr_us;
    lp_ref_conv_sum += conv_us;
    lp_ref_wait_sum += wait_us;
    lp_ref_ximr_sum += ximr_us;
    lp_ref_n++;
}

/* ---- text buffer for the log ------------------------------------------- */
struct lp_buf { char * p; int len; int cap; };

static void lp_printf(struct lp_buf * b, const char * fmt, ...)
{
    if (!b->p || b->len >= b->cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->p[b->cap - 1] = 0;
    b->len += strlen(b->p + b->len);
}

/* a pointer that was not dereferenced */
static void lp_bad(struct lp_buf * b, const char * key, uint32_t base)
{
    if (lp_is_uncached(base) && !lp_uc_ok && base >= 0x40001000)
        lp_printf(b, "%s=uncached_unverified(0x%08x)\n", key, (int) base);
    else
        lp_printf(b, "%s=invalid(0x%08x)\n", key, (int) base);
}

/* fixed Canon global */
static void lp_glob(struct lp_buf * b, const char * key, uint32_t addr)
{
    uint32_t v;
    if (rd32(addr, &v)) lp_printf(b, "%s=0x%08x\n", key, (int) v);
    else lp_printf(b, "%s=invalid@0x%08x\n", key, (int) addr);
}

/* 32-bit field of a Canon structure */
static void lp_fld(struct lp_buf * b, const char * key, uint32_t base, uint32_t off)
{
    uint32_t v;
    if (!base) lp_printf(b, "%s=NULL\n", key);
    else if (!fld(base, off, &v)) lp_bad(b, key, base);
    else lp_printf(b, "%s=0x%08x\n", key, (int) v);
}

/* 64-bit field, read once into a local (q) and printed later */
struct lp_q { uint32_t base; int r; uint32_t lo, hi; };

static void lp_q64(struct lp_q * q, uint32_t base, uint32_t off)
{
    q->base = base; q->lo = q->hi = 0;
    q->r = (base && ram_ok(base, 4)) ? rd64(base + off, &q->lo, &q->hi) : 0;
}

static void lp_put64(struct lp_buf * b, const char * key, const struct lp_q * q)
{
    if (!q->base) lp_printf(b, "%s=NULL\n", key);
    else if (q->r == 1) lp_printf(b, "%s=0x%x:%08x\n", key, (int) q->hi, (int) q->lo);
    else if (q->r < 0) lp_printf(b, "%s=unstable(0x%x:%08x)\n", key, (int) q->hi, (int) q->lo);
    else lp_bad(b, key, q->base);
}

static void lp_fld64(struct lp_buf * b, const char * key, uint32_t base, uint32_t off)
{
    struct lp_q q;
    lp_q64(&q, base, off);
    lp_put64(b, key, &q);
}

/* informational only: which static window (4..11) of the RAM copy would map
 * IBus hi:lo by Canon's rule (0xE089F2B8..0xE089F2D4: same channel, same
 * 256 MiB slice; start address only). Pure arithmetic, nothing is read. */
static int lp_win_for(const uint32_t * win, uint32_t lo, uint32_t hi)
{
    if (!lo && !hi) return -1;
    for (int i = 4; i < 12; i++)
        if ((win[i] & 7) == hi && (win[i] & 0xF0000000) == (lo & 0xF0000000) && !(win[i] & 0x0FFFFFF8))
            return i;
    return -1;
}

/* ---- M7.2a census extension ----------------------------------------------- */

/* byte at a, read as the aligned 32-bit word that contains it */
static int rd8(uint32_t a, uint32_t * v)
{
    uint32_t w;
    if (!rd32(a & ~3u, &w)) return 0;
    *v = (w >> (8 * (a & 3))) & 0xFF;
    return 1;
}

/* n words [a, a+4n) of a Canon global area */
static void lp_globs(struct lp_buf * b, const char * key, uint32_t addr, int n)
{
    char k[48];
    for (int i = 0; i < n; i++)
    {
        snprintf(k, sizeof(k), "%s[0x%02x]", key, i * 4);
        lp_glob(b, k, addr + 4 * i);
    }
}

/* n words of a Canon structure from offset off (whole range checked) */
static void lp_flds(struct lp_buf * b, const char * key, uint32_t base, uint32_t off, int n)
{
    char k[48];
    snprintf(k, sizeof(k), "%s@0x%x", key, (int) off);     /* failure key, unique per range */
    if (!base) { lp_printf(b, "%s=NULL\n", k); return; }
    if (!ram_ok(base, 4) || base + off < base || !ram_ok(base + off, 4 * n)) { lp_bad(b, k, base); return; }
    for (int i = 0; i < n; i++)
    {
        /* %02x only below 0x100: a width smaller than the digit count garbles the
         * output of this snprintf (seen in QEMU: "rd[0x 0]") */
        uint32_t o = off + 4 * i;
        snprintf(k, sizeof(k), o < 0x100 ? "%s[0x%02x]" : "%s[0x%x]", key, (int) o);
        lp_printf(b, "%s=0x%08x\n", k, (int) RD(base + o));
    }
}

/* Canon LV histogram, DISP set (Option 1). ctx = MEM(0x11F44+4).
 * Fills p[4] (published pointers), returns the set id: 0/1 DISP set 0/1,
 * 2/3 ALO set 0/1, -1 no set matches, -2 ctx not readable */
static int lp_hist_ptrs(uint32_t ctx, uint32_t p[4])
{
    p[0] = p[1] = p[2] = p[3] = 0;
    if (!ctx || !ram_ok(ctx, LP_HIST_CTX_SIZE)) return -2;
    for (int i = 0; i < 4; i++) p[i] = RD(ctx + 8 + 4 * i);
    static const uint8_t first[4] = { 0x4C, 0x50, 0x28, 0x2C };   /* DISP0, DISP1, ALO0, ALO1; stride 8 */
    for (int s = 0; s < 4; s++)
    {
        int m = 1;
        for (int i = 0; i < 4; i++)
            if (RD(ctx + first[s] + 8 * i) != p[i]) m = 0;
        if (m) return s;
    }
    return -1;
}

/* one pass over the 256 u32 bins at p (32-bit loads, uncached heap):
 * sum, 16 buckets of 16 bins, bins 0..7 and 248..255, mean bin (and x10);
 * 0 = not readable */
struct lp_hstat { uint32_t sum, mean, mean_x10, b16[16], lo[8], hi[8]; };
static int lp_hist_stats(uint32_t p, struct lp_hstat * h)
{
    if (!p || !ram_ok(p, 0x400)) return 0;
    uint32_t s = 0;
    uint64_t ws = 0;
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < 256; i++)
    {
        uint32_t v = RD(p + 4 * i);
        s += v;
        ws += (uint64_t) v * i;
        h->b16[i >> 4] += v;
        if (i < 8) h->lo[i] = v;
        if (i >= 248) h->hi[i - 248] = v;
    }
    h->sum = s;
    h->mean = s ? (uint32_t) (ws / s) : 0;
    h->mean_x10 = s ? (uint32_t) (ws * 10 / s) : 0;
    return 1;
}

/* type of a short-descriptor L1 entry (section / supersection), assuming
 * TEX remap off (the M7.1 descriptors 0x80054402 / 0xBF050416 decode as
 * Normal non-cacheable / Device, matching how Canon uses those areas) */
static const char * lp_l1_type(uint32_t d)
{
    if ((d & 3) != 2) return (d & 3) ? "not_section" : "fault";
    if (!(d & 0xC00)) return "no_access";                       /* AP[1:0] = 00 */
    uint32_t tex = (d >> 12) & 7, c = (d >> 3) & 1, b = (d >> 2) & 1;
    if (tex & 4) return "normal";                               /* cacheable Normal */
    if (tex == 0) return c ? "normal" : b ? "device" : "strongly_ordered";
    if (tex == 1) return c == b ? "normal" : "reserved";        /* 001/00 Normal NC, 001/11 WBWA */
    if (tex == 2) return (!c && !b) ? "device" : "reserved";
    return "reserved";
}

/* RandomData base as DTS_GetAllRandomData 0xE079D026 computes it, without
 * calling it: [0x1200C+0x10] ? working copy : [0x132A70 + 4*[0x1200C+0x1C]].
 * Returns 0 if not readable; *src: 1 = working copy, 2 = ring */
static uint32_t lp_rd_base(int * src)
{
    uint32_t use_work = 0, idx = 0;
    *src = 0;
    if (!rd32(LP_DTS + 0x10, &use_work) || !rd32(LP_DTS + 0x1C, &idx)) return 0;
    if (use_work) { *src = 1; return LP_RD_WORK; }
    if (idx >= LP_RD_RING_N) return 0;
    uint32_t p = RD(LP_RD_RING + 4 * idx);
    if (!p || !ram_ok(p, LP_RD_SIZE)) return 0;
    *src = 2;
    return p;
}

static const char * lp_region_name(uint32_t a)
{
    if (a >= 0x1000 && a < 0x40000000) return "cached";
    if (a >= 0x40001000 && a < 0x80000000) return "uncached";
    return a ? "other" : "zero";
}

#ifdef CONFIG_R6_REFRESH_GUARD
static void lp_guard_log(struct lp_buf * b)
{
    const struct r6_refresh_guard_stats * g = r6_refresh_guard_stats();
    char k[48];
#define LG(name, val) lp_printf(b, "guard." name "=%d\n", (int) (val))
#define LGX(name, val) lp_printf(b, "guard." name "=0x%08x\n", (int) (val))
    LG("now", get_ms_clock());
    LG("calls", g->calls);
    LG("ok", g->ok);
    LG("take_timeout", g->take_timeout);
    LG("busy", g->busy);
    LG("no_sem", g->no_sem);
    LG("skip_pend", g->pend);
    LG("skip_size", g->size);
    LG("skip_ctx", g->ctx);
    LG("skip_img", g->img);
    LG("skip_settle", g->settle);
    LG("skip_rate", g->rate);
    LG("pig_late", g->pig_late);
    LG("skip_after_take", g->post_skip);
    LG("pig", g->pig);
    LG("settle_pig", g->settle_pig);
    LG("pig_ok", g->pig_ok);
    LG("pig_rearm", g->pig_rearm);
    LG("t_act", g->t_act);
    LG("act_idx", g->act_idx);
    LG("act_pend", g->act_pend);
    LG("act_img", g->act_img);
    LG("act_win_ms", g->act_win_ms);
    LG("would_lock", g->would_lock);
    LG("would_disdraw", g->would_disdraw);
    LG("would_disref", g->would_disref);
    LG("would_seq", g->would_seq);
    LG("polls", g->polls);
    LG("duty_pend", g->d_pend);
    LG("duty_size", g->d_size);
    LG("duty_img", g->d_img);
    LG("duty_lock", g->d_lock);
    LG("duty_disdraw", g->d_disdraw);
    LG("duty_disref", g->d_disref);
    LG("duty_seq", g->d_seq);
    LG("duty_evf", g->d_evf);
    LG("changes", g->changes);
    lp_printf(b, "guard.change_mask=0x%x\n", (int) g->change_mask);
    lp_printf(b, "guard.change_mask_all=0x%x\n", (int) g->change_mask_all);
    LG("t_change", g->t_change);
    LG("idx_changes", g->idx_changes);
    LG("t_idx_change", g->t_idx_change);
    LGX("w_mask", g->w_mask);
    LGX("w_vrammgr_mode", g->w_vm);
    LGX("w_gui_disptype", g->w_disptype);
    LGX("w_xoc_enable", g->w_xoc);
    LG("t_exe_start", g->t_exe_start);
    LG("t_exe_end", g->t_exe_end);
    LG("exe_active", g->exe_active);
    LG("exe_evf", g->exe_evf);
    LG("hold_us_last", g->hold_us_last);
    LG("hold_us_max", g->hold_us_max);
    LG("wait_us_last", g->wait_us_last);
    LG("wait_us_max", g->wait_us_max);
    LG("wait_timeout_us_last", g->wait_to_us_last);
    LG("ximr_us_last", g->ximr_us_last);
    LG("ximr_us_max", g->ximr_us_max);
    LG("gate_exe_us_max", g->gate_exe_us_max);
    LG("overlap", g->overlap);
    lp_printf(b, "guard.overlap_mask=0x%x\n", (int) g->overlap_mask);
    LG("t_overlap", g->t_overlap);
    LG("clock_back", g->clock_back);
    LG("t_last_shown", g->t_last_shown);
    LG("ep_active", g->ep_active);
    LG("ep_start", g->ep_start);
    lp_printf(b, "guard.ep_mask=0x%x\n", (int) g->ep_mask);
    LG("starve_ms_now", r6_refresh_guard_starve_ms());
    LG("starve_ms_max", g->starve_ms_max);
    lp_printf(b, "guard.starve_max_mask=0x%x\n", (int) g->starve_max_mask);
    LG("starve_max_t", g->starve_max_t);
    LG("starve_n", g->starve_n);
    /* ring, oldest first; mask bits R6G_R_* (refresh_guard.h), end 1 XimrExe 2 piggyback 3 idle */
    uint32_t n = g->starve_n, cnt = n < R6G_RING ? n : R6G_RING;
    for (uint32_t i = 0; i < cnt; i++)
    {
        const struct r6g_episode * ep = &g->ring[(n - cnt + i) % R6G_RING];
        snprintf(k, sizeof(k), "guard.starve[%d]", (int) i);
        lp_printf(b, "%s.t=%d\n", k, ep->t_start);
        lp_printf(b, "%s.dur_ms=%d\n", k, (int) ep->dur_ms);
        lp_printf(b, "%s.mask=0x%x\n", k, (int) ep->mask);
        lp_printf(b, "%s.end=%d\n", k, (int) ep->end);
    }
#undef LG
#undef LGX
}
#endif

static void lp_census_m72a(struct lp_buf * b)
{
    char k[48];
    uint32_t v;

    lp_printf(b, "# M7.2a: LV histogram, RandomData, FaceYuv, dynamic window / GS stage 0, VMIX/WINSYS, guard\n");

    /* -- Canon LV histogram (Option 1) -- */
    lp_globs(b, "hist.state", LP_HIST_STATE, 7);                 /* 0x11F44..+0x1B */
    uint32_t hctx = RD(LP_HIST_STATE + 4);
    lp_printf(b, "hist.ctx_ptr=0x%08x\n", (int) hctx);
    lp_flds(b, "hist.ctx", hctx, 0, LP_HIST_CTX_SIZE / 4);       /* ctx+0..+0xCF */
    uint32_t p0[4], p1[4], w = 0, h = 0;
    int set = lp_hist_ptrs(hctx, p0);
    lp_printf(b, "hist.set=%d\n", set);                           /* 0/1 DISP, 2/3 ALO, -1 none, -2 ctx bad */
    if (set > -2)
    {
        lp_printf(b, "hist.numpix_hyp=%d\n", (int) RD(hctx + 4));
        w = RD(hctx + 0xC4); h = RD(hctx + 0xC8);
        lp_printf(b, "hist.w_h=%dx%d\n", (int) w, (int) h);
        int eq = 0;
        struct lp_hstat hs;
        for (int i = 0; i < 4; i++)
        {
            snprintf(k, sizeof(k), "hist.p[%d]", i);              lp_printf(b, "%s=0x%08x\n", k, (int) p0[i]);
            snprintf(k, sizeof(k), "hist.p[%d].bins", i);
            if (set < 0)                                          /* not one of Canon's buffer sets */
            {
                lp_printf(b, "%s=skipped(no_set)\n", k);
                continue;
            }
            if (!lp_hist_stats(p0[i], &hs)) { lp_bad(b, k, p0[i]); continue; }
            for (int j = 0; j < 8; j++)
            {
                snprintf(k, sizeof(k), "hist.p[%d].bin[%d]", i, j); lp_printf(b, "%s=%d\n", k, (int) hs.lo[j]);
            }
            for (int j = 0; j < 8; j++)
            {
                snprintf(k, sizeof(k), "hist.p[%d].bin[%d]", i, 248 + j); lp_printf(b, "%s=%d\n", k, (int) hs.hi[j]);
            }
            for (int j = 0; j < 16; j++)                          /* bins 16j..16j+15 */
            {
                snprintf(k, sizeof(k), "hist.p[%d].b16[%d]", i, j); lp_printf(b, "%s=%d\n", k, (int) hs.b16[j]);
            }
            snprintf(k, sizeof(k), "hist.p[%d].sum", i);          lp_printf(b, "%s=%d\n", k, (int) hs.sum);
            snprintf(k, sizeof(k), "hist.p[%d].mean_x10", i);     lp_printf(b, "%s=%d\n", k, (int) hs.mean_x10);
            if (hs.sum == RD(hctx + 4)) eq++;
        }
        lp_printf(b, "hist.sum_eq_numpix=%d\n", eq);
        int set2 = lp_hist_ptrs(hctx, p1);                        /* tear check: same set after the reads */
        int same = set2 == set;
        for (int i = 0; i < 4; i++) if (p1[i] != p0[i]) same = 0;
        lp_printf(b, "hist.stable=%d\n", same);
    }

    /* -- RandomData (LV DataStore) -- */
    lp_globs(b, "dts", LP_DTS, 0x70 / 4);                         /* 0x1200C..+0x6F */
    lp_globs(b, "rd.ring", LP_RD_RING, LP_RD_RING_N);
    int src = 0;
    uint32_t rd = lp_rd_base(&src);
    lp_printf(b, "rd.base=0x%08x\n", (int) rd);
    lp_printf(b, "rd.src=%s\n", src == 1 ? "work" : src == 2 ? "ring" : "invalid");
    if (rd)
    {
        uint32_t f1 = RD(rd), f2 = RD(rd);
        lp_printf(b, "rd.framenum=%d\n", (int) f2);
        lp_printf(b, "rd.framenum_stable=%d\n", f1 == f2);
        lp_flds(b, "rd", rd, 0x5D0, (0x634 - 0x5D0) / 4);         /* HistAlo, Hist, ExpSim, Metering, AewbScene, PetitYch */
        lp_flds(b, "rd", rd, 0xE8C, (0xEAC - 0xE8C) / 4);         /* MfPeakingSetting .. MovieRecordMode */
    }

    /* -- FaceYuv (Option 2): pointers and headers only, no pixels -- */
    lp_globs(b, "fy.state", LP_FY_STATE, 7);                      /* 0x1ED68..+0x18 */
    uint32_t fctx = RD(LP_FY_STATE + 0xC);
    lp_printf(b, "fy.ctx_ptr=0x%08x\n", (int) fctx);
    lp_flds(b, "fy.ctx", fctx, 0, 0x40 / 4);
    lp_flds(b, "fy.ctx", fctx, 0xB020, 0x40 / 4);
    uint32_t win[12];
    for (int i = 0; i < 12; i++) win[i] = RD(LP_MEMIF_WIN + 4 * i);
    for (int i = 0; i < 4; i++)
    {
        struct lp_q q;
        lp_q64(&q, fctx, 0x20 + 8 * i);
        snprintf(k, sizeof(k), "fy.ibus[%d]", i);                 lp_put64(b, k, &q);
        if (q.r != 1) continue;
        snprintf(k, sizeof(k), "fy.ibus[%d].ch", i);              lp_printf(b, "%s=%d\n", k, (int) q.hi);
        snprintf(k, sizeof(k), "fy.ibus[%d].static_window", i);
        if (!win[11]) lp_printf(b, "%s=unknown(table_not_copied)\n", k);
        else
        {
            int wi = lp_win_for(win, q.lo, q.hi);
            if (wi < 0) lp_printf(b, "%s=none\n", k); else lp_printf(b, "%s=%d\n", k, wi);
        }
    }
    for (int i = 0; i < 6; i++)
    {
        snprintf(k, sizeof(k), "fy.ring[%d]", i);
        if (!rd32(LP_FY_RING + 4 * i, &v)) { lp_printf(b, "%s=invalid\n", k); continue; }
        lp_printf(b, "%s=0x%08x\n", k, (int) v);
        if (i & 1)
        {
            snprintf(k, sizeof(k), "fy.ring[%d].region", i);      lp_printf(b, "%s=%s\n", k, lp_region_name(v));
        }
    }
    if (rd32(LP_LVIC_SIZE, &v))
    {
        lp_printf(b, "fy.lvic_w=%d\n", (int) (v & 0xFFFF));       /* MEM16(0x11FC8) */
        lp_printf(b, "fy.lvic_h=%d\n", (int) (v >> 16));          /* MEM16(0x11FCA) */
    }

    /* -- dynamic memif window, stage 0 (Option 3): preconditions only -- */
    lp_glob(b, "dw.memif_debug", LP_MEMIF_DBG);
    if (rd8(LP_ARB, &v)) lp_printf(b, "dw.arb_mode=0x%02x\n", (int) v);
    if (rd8(LP_ARB_CH3_PID5, &v)) lp_printf(b, "dw.arb_ch3_pid5=%d\n", (int) v);
    /* the DF section's own L1 descriptor (L1 word, read only) */
    uint32_t l1df = RD(LP_L1_DF);
    lp_printf(b, "ttbr1_l1[0xdff00000]=0x%08x\n", (int) l1df);   /* VA in the key text: no 0xDFF00000 literal */
    lp_printf(b, "dw.df_section=%s\n", lp_l1_type(l1df));
#ifdef LP_READ_PSR_MASK
    /* PSR mask per ROM 0xE013311C, adjacent to CPU1 code/stack */
    if (!strcmp(lp_l1_type(l1df), "normal"))
        lp_printf(b, "dw.psr_mask=0x%08x\n", (int) RD(LP_PSR_MASK));
    else
        lp_printf(b, "dw.psr_mask=not_read(section_not_normal)\n");
#else
    lp_printf(b, "dw.psr_mask=not_read(needs_approval)\n");
#endif
    uint32_t dww = RD(LP_DWW_PTR);
    lp_printf(b, "dw.dww_ptr=0x%08x\n", (int) dww);
    lp_fld(b, "dw.dww_slot", dww, 0);
    lp_fld(b, "dw.dww_in_use", dww, 0x20);
    lp_printf(b, "ttbr1_l1[0x%08x]=0x%08x\n", (int) 0xC6000000, (int) RD(LP_TTBR1 + (0xC6000000 >> 20) * 4));

    /* -- GS / I2iDmac, stage 0 (Option 5): preconditions only -- */
    lp_globs(b, "gs", LP_GS, 0x28 / 4);                           /* 0x106EBC..+0x24 */
    for (int ch = 1; ch <= 5; ch++)
        for (int pid = 0x92; pid <= 0x97; pid++)
        {
            snprintf(k, sizeof(k), "arb.ch%d.pid%02x", ch, pid);
            if (rd8(LP_ARB + (ch - 1) * 0x16F + pid, &v)) lp_printf(b, "%s=%d\n", k, (int) v);
            else lp_printf(b, "%s=invalid\n", k);
        }
    for (int ch = 1; ch <= 5; ch++)
    {
        snprintf(k, sizeof(k), "ch_limit[%d]", ch);               lp_glob(b, k, LP_CH_LIMIT + 4 * (ch - 1));
    }
    for (int c = 0; c < 12; c++)
    {
        snprintf(k, sizeof(k), "i2i[%d]", c);                     lp_globs(b, k, LP_I2I + 0x20 * c, 8);
    }
    lp_glob(b, "gs.prio_base", LP_GS_PRIO);
    lp_glob(b, "gs.prio", LP_GS_PRIO + 4);

    /* -- VMIX / WINSYS flags (hang guard inputs) -- */
    lp_globs(b, "vmixw", LP_VMIX, (0x18448 - LP_VMIX) / 4);       /* 0x18404..0x18444 */
    lp_glob(b, "vmix.size_change", LP_VMIX + 0x14);
    lp_glob(b, "vmix.active_outputs", LP_VMIX + 0x1C);
    lp_glob(b, "vmix.cb_left", LP_VMIX + 0x20);
    lp_glob(b, "vmix.flipped", LP_VMIX + 0x24);
    lp_glob(b, "winsys.request", LP_WINSYS + 0x8);
    lp_glob(b, "winsys.disable_draw", LP_WINSYS + 0xC);
    lp_glob(b, "winsys.disable_refresh", LP_WINSYS + 0x10);
    lp_glob(b, "dtc_seq", LP_DTC_SEQ);
    lp_glob(b, "gui_disptype", LP_GUI_DISPTYPE);
    lp_glob(b, "gui_disptype2", LP_GUI_DISPTYPE + 4);
    lp_printf(b, "ml_refresh_display_needed=%d\n", (int) ml_refresh_display_needed);

#ifdef CONFIG_R6_REFRESH_GUARD
    lp_guard_log(b);
#else
    lp_printf(b, "guard=off\n");
#endif
}

/* ---- one census sample --------------------------------------------------- */
static const struct { const char * name; uint16_t cur, ready; } lp_outs[5] = {
    /* DisplayOut 0..4: current 0xE025DA44.., ready 0xE025DB8E.. */
    { "panel", 0x130, 0x128 },
    { "evf",   0x140, 0x138 },
    { "line",  0x120, 0x118 },
    { "out3",  0x150, 0x148 },
    { "out4",  0x160, 0x158 },
};

/* DVC init loop 0xE025E6EC..0xE025E71A clears 3 x 2 slots of 0x98 bytes */
static const uint16_t lp_info_slots[6] = { 0x1C8, 0x260, 0x390, 0x428, 0x558, 0x5F0 };

static void lp_census(struct lp_buf * b, const char * tag)
{
    char k[48];
    uint32_t v;
    int len;

    lp_uc_update();

    /* Buffer addresses that M7.1 compares with each other: read back to back
     * into locals first, formatted below (Canon rotates them per frame). */
    struct lp_q dvc_cur[5], dvc_ready[5], img_ready[5], img_disp[5], grp_last;
    uint32_t t_set0 = (uint32_t) get_us_clock();
    uint32_t dvc = RD(LP_DVC_PTR);
    uint32_t img[5];
    for (int o = 0; o < 5; o++) img[o] = RD(LP_IMG_STRUCTS + 4 * o);
    uint32_t g = RD(LP_GRPCTR_PTR);
    for (int o = 0; o < 5; o++)
    {
        lp_q64(&dvc_cur[o], dvc, lp_outs[o].cur);
        lp_q64(&dvc_ready[o], dvc, lp_outs[o].ready);
    }
    for (int o = 0; o < 5; o++)
    {
        lp_q64(&img_ready[o], img[o], 0x60);
        lp_q64(&img_disp[o], img[o], 0x68);
    }
    lp_q64(&grp_last, g, 0x70);
    uint32_t t_set1 = (uint32_t) get_us_clock();

    lp_printf(b, "sample=%s\n", tag);
    lp_printf(b, "t_ms=%d\n", get_ms_clock());
    lp_printf(b, "uncached_alias_ok=%d\n", lp_uc_ok);

    /* LV flag */
    lp_printf(b, "lv=%d\n", lv);
    lp_printf(b, "lv_paused=%d\n", lv_paused);
    lp_printf(b, "lv_follow_canon=%d\n", lv_follow_canon_get());
    if (lv_action_raw_get(&v, &len))
    {
        lp_printf(b, "lv_action_raw=0x%08x\n", (int) v);
        lp_printf(b, "lv_action_len=%d\n", len);
    }
    else lp_printf(b, "lv_action_raw=none\n");
    uint32_t n = lp_ring_count;
    lp_printf(b, "lv_action_ring_count=%d\n", (int) n);
    for (uint32_t i = (n > LP_RING ? n - LP_RING : 0); i < n; i++)
    {
        lp_printf(b, "lv_action_ring[%d].t=%d\n", (int) i, (int) lp_ring_t[i % LP_RING]);
        lp_printf(b, "lv_action_ring[%d].v=0x%x\n", (int) i, (int) lp_ring_v[i % LP_RING]);
        lp_printf(b, "lv_action_ring[%d].len=%d\n", (int) i, (int) lp_ring_len[i % LP_RING]);
    }

    /* GUI / LV state */
    lp_printf(b, "shooting_type=%d\n", shooting_type);
    lp_glob(b, "gui_mode", LP_GUI_MODE);
    lp_printf(b, "gui_state=%d\n", gui_state);
    lp_glob(b, "display_is_on", LP_DISPLAY_IS_ON);
    lp_glob(b, "lvapp_dialog", LP_LVAPP_DIALOG);
    lp_glob(b, "level_dialog", LP_LEVEL_DIALOG);
    lp_glob(b, "silent_dialog", LP_SILENT_DIALOG);
    lp_printf(b, "lv_dispsize=%d\n", lv_dispsize);
    lp_glob(b, "lv_overlays_mode", LP_LV_OVERLAYS);
    lp_printf(b, "expsim_raw=%d\n", _expsim);
    lp_printf(b, "expsim=%d\n", get_expsim());
    lp_printf(b, "digic_iso_gain_photo=%d\n", get_digic_iso_gain_photo());

    /* display outputs */
    uint32_t dd = RD(LP_DISPDEV_PTR);
    lp_printf(b, "dispdev_ptr=0x%08x\n", (int) dd);
    lp_fld(b, "disp_mask", dd, 0x18);
    uint32_t gp = RD(LP_GUI_PTR);
    lp_printf(b, "gui_ptr=0x%08x\n", (int) gp);
    lp_fld(b, "gui_lv_action", gp, 0x1828);
    lp_glob(b, "vrammgr_mode", LP_VRAMMGR_MODE);

    /* the coherent address set */
    lp_printf(b, "coherent_set_us=%d\n", (int) (t_set1 - t_set0));
    lp_printf(b, "dvc_ptr=0x%08x\n", (int) dvc);
    for (int o = 0; o < 5; o++)
    {
        snprintf(k, sizeof(k), "dvc.%s.cur", lp_outs[o].name);   lp_put64(b, k, &dvc_cur[o]);
        snprintf(k, sizeof(k), "dvc.%s.ready", lp_outs[o].name); lp_put64(b, k, &dvc_ready[o]);
    }
    for (int o = 0; o < 5; o++)
    {
        snprintf(k, sizeof(k), "img[%d]", o);                    lp_printf(b, "%s=0x%08x\n", k, (int) img[o]);
        snprintf(k, sizeof(k), "img[%d].ready", o);              lp_put64(b, k, &img_ready[o]);
        snprintf(k, sizeof(k), "img[%d].disp", o);               lp_put64(b, k, &img_disp[o]);
    }
    lp_printf(b, "grpctr_ptr=0x%08x\n", (int) g);
    lp_put64(b, "grpctr.last_panel", &grp_last);

    /* DISPIMAGEINFO slots (0x98 bytes each; validator 0xE07B0438): [1] DisplayOut,
     * [2..3] IBus address, [5] pitch in px, [6] height, [0xd], [0xe] format
     * (0 = UYVY 8 bit, 3 = packed 10 bit), [0x19], [0x1a], [0x1d] = 1 when initialised.
     * Slot -> output mapping is unverified: w[1] (DisplayOut) settles it. */
    lp_printf(b, "# dvc.imginfo slots, assumed panel 0x1c8/0x260, evf 0x390/0x428, line 0x558/0x5f0 (unverified; w[0x1] = DisplayOut)\n");
    static const uint8_t dii[] = { 0, 1, 2, 3, 5, 6, 0xd, 0xe, 0x19, 0x1a, 0x1d };
    for (int s = 0; s < 6; s++)
        for (int i = 0; i < (int) COUNT(dii); i++)
        {
            snprintf(k, sizeof(k), "dvc.imginfo[0x%x].w[0x%x]", lp_info_slots[s], dii[i]);
            lp_fld(b, k, dvc, lp_info_slots[s] + 4 * dii[i]);
        }

    /* DispVramMgr per-output state (slots; 'cur' must be one of vram[0..3]) */
    for (int o = 0; o < 5; o++)
    {
        snprintf(k, sizeof(k), "img[%d].alloc_type", o);         lp_fld(b, k, img[o], 0);
        for (int i = 0; i < 4; i++)
        {
            snprintf(k, sizeof(k), "img[%d].vram[%d]", o, i);    lp_fld64(b, k, img[o], 8 + 8 * i);
        }
        snprintf(k, sizeof(k), "img[%d].wp_rp", o);              lp_fld(b, k, img[o], 0x28);
        snprintf(k, sizeof(k), "img[%d].latest_ready_push", o);  lp_fld64(b, k, img[o], 0x58);
    }

    /* RscMgr2 IMGVRAM work entries: entry = handle + idx*0x18, +8 cpu, +0x10/+0x14 IBus, +0x18 size
     * (ExtractWorkCpuAddr 0xE0142E88, ExtractWorkIbusAddr 0xE0142EB0); types 1 = FHD, 2 = UHD
     * (size table 0xE0FC88B0: 0x4F1A00, 0x1518000) */
    for (int i = 0; i < 4; i++)
    {
        snprintf(k, sizeof(k), "rsc_idx[%d]", i);                lp_glob(b, k, LP_VRAMMGR_IDX + 4 * i);
    }
    for (int t = 1; t <= 2; t++)
    {
        uint32_t h = RD(LP_RSC_HANDLE + 4 * t);
        snprintf(k, sizeof(k), "rsc[%d]", t);                    lp_printf(b, "%s=0x%08x\n", k, (int) h);
        for (int i = 0; i < 4; i++)
        {
            uint32_t idx = RD(LP_VRAMMGR_IDX + 4 * i);
            uint32_t e = (h && idx < 0x100) ? h + idx * 0x18 : 0;
            if (h && !e)
            {
                snprintf(k, sizeof(k), "rsc[%d].e%d", t, i);     lp_printf(b, "%s=invalid_idx(0x%x)\n", k, (int) idx);
                continue;
            }
            snprintf(k, sizeof(k), "rsc[%d].e%d.cpu", t, i);     lp_fld(b, k, e, 8);
            snprintf(k, sizeof(k), "rsc[%d].e%d.ibus", t, i);    lp_fld64(b, k, e, 0x10);
            snprintf(k, sizeof(k), "rsc[%d].e%d.size", t, i);    lp_fld(b, k, e, 0x18);
        }
    }

    /* static memif windows (RAM copy) and ML's own window lookup, from the
     * coherent dvc.*.cur values above */
    uint32_t win[12];
    for (int i = 0; i < 12; i++)
    {
        win[i] = RD(LP_MEMIF_WIN + 4 * i);
        snprintf(k, sizeof(k), "memif_win[%d]", i);              lp_printf(b, "%s=0x%08x\n", k, (int) win[i]);
    }
    lp_printf(b, "memif_win_copied=%d\n", win[11] != 0);
    for (int o = 0; o < 2; o++)
    {
        const struct lp_q * q = &dvc_cur[o];
        snprintf(k, sizeof(k), "%s_static_window", lp_outs[o].name);
        if (!win[11]) { lp_printf(b, "%s=unknown(table_not_copied)\n", k); continue; }
        int w = (q->r == 1) ? lp_win_for(win, q->lo, q->hi) : -1;
        if (w < 0) { lp_printf(b, "%s=none\n", k); continue; }
        lp_printf(b, "%s=%d\n", k, w);
        snprintf(k, sizeof(k), "%s_static_cpu_start", lp_outs[o].name);
        lp_printf(b, "%s=0x%08x\n", k, (int) (((uint32_t) w << 28) | (q->lo & 0x0FFFFFFF)));
    }
    for (int i = 0; i < 24; i++)
    {
        snprintf(k, sizeof(k), "dynwin[%d]", i);                 lp_glob(b, k, LP_DYNWIN + 4 * i);
    }

    /* LV RAW path (Mem1Path): +0x10 bpp, +0x14/+0x18 W/H, +0x2C save flag, +0x50/+0x54 IBus address */
    lp_glob(b, "lv_save_raw", LP_LV_SAVE_RAW);
    for (int i = 0; i < 0x80 / 4; i++)
    {
        snprintf(k, sizeof(k), "mem1path[0x%02x]", i * 4);       lp_glob(b, k, LP_MEM1PATH + 4 * i);
    }

    /* VMIX / XCM / XOC / RENDERER (Canon's OSD compositor) */
    lp_glob(b, "vmix.xcm_num", LP_VMIX + 0x8);
    lp_glob(b, "vmix.output_num", LP_VMIX + 0xC);
    lp_glob(b, "vmix.buf_index", LP_VMIX + 0x10);
    lp_glob(b, "vmix.pending", LP_VMIX + 0x18);
    for (int x = 0; x < 3; x++)
    {
        uint32_t xcm = RD(LP_XCM_TABLE + 4 * x);
        snprintf(k, sizeof(k), "xcm[%d]", x);                    lp_printf(b, "%s=0x%08x\n", k, (int) xcm);
        snprintf(k, sizeof(k), "xcm[%d].contexts", x);           lp_fld(b, k, xcm, 0xC);
        for (int l = 0; l < 4; l++)
        {
            snprintf(k, sizeof(k), "xcm[%d].layer[%d]", x, l);   lp_fld(b, k, xcm, 0x1700 + 0x24 * l);
        }
    }
    for (int i = 0; i < 12; i++)
    {
        uint32_t xoc = RD(LP_XOC_TABLE + 4 * i);
        snprintf(k, sizeof(k), "xoc[%d.%d]", i / 4, i % 4);      lp_printf(b, "%s=0x%08x\n", k, (int) xoc);
        snprintf(k, sizeof(k), "xoc[%d.%d].layer_enable", i / 4, i % 4); lp_fld(b, k, xoc, 4);
    }
    for (int i = 0; i < 6; i++)
    {
        snprintf(k, sizeof(k), "renderer[%d]", i);               lp_glob(b, k, LP_RENDERER + 4 * i);
    }

    /* ML's drawing surface (Canon layer 0 MARV) */
    uint32_t m = (uint32_t) rgb_vram_info;
    lp_printf(b, "rgb_vram_info=0x%08x\n", (int) m);
    lp_fld(b, "rgb_vram_info.bitmap_data", m, 4);
    lp_fld64(b, "rgb_vram_info.ibus", m, 0x10);
    lp_fld(b, "rgb_vram_info.flags", m, 0x18);
    lp_fld(b, "rgb_vram_info.width", m, 0x1C);
    lp_fld(b, "rgb_vram_info.height", m, 0x20);

    /* memory, as ML's own allocator sees it (MALLOC_STRUCT_ADDR, RAM only) */
    struct malloc_status * ms = (struct malloc_status *) MALLOC_STRUCT_ADDR;
    lp_printf(b, "malloc_free=%d\n", GetFreeMemForMalloc());
    lp_printf(b, "malloc_total=%d\n", (int) ms->total_size_external);
    lp_printf(b, "malloc_used=%d\n", (int) ms->used_size);
    lp_printf(b, "malloc_used_peak=%d\n", (int) ms->used_peak);
    lp_printf(b, "malloc_free_min=%d\n", (int) (ms->total_size_external - ms->used_peak));

    /* MMU: TTBR1 L1 words (explicit exception to the RAM range, read only) */
    static const uint32_t vas[3] = { 0x80000000, 0xB0000000, 0xBF000000 };
    for (int i = 0; i < 3; i++)
    {
        uint32_t a = LP_TTBR1 + (vas[i] >> 20) * 4;
        lp_printf(b, "ttbr1_l1[0x%08x]=0x%08x\n", (int) vas[i], (int) RD(a));
    }

    /* ML refresh cost: conv = indexed -> RGBA, wait = winsys_sem wait (Canon lock), ximr = XimrExe */
    uint32_t rn = lp_ref_n;
    lp_printf(b, "refresh_count=%d\n", (int) rn);
    lp_printf(b, "refresh_conv_us=%d\n", (int) lp_ref_conv);
    lp_printf(b, "refresh_wait_us=%d\n", (int) lp_ref_wait);
    lp_printf(b, "refresh_ximr_us=%d\n", (int) lp_ref_ximr);
    lp_printf(b, "refresh_conv_us_max=%d\n", (int) lp_ref_conv_max);
    lp_printf(b, "refresh_wait_us_max=%d\n", (int) lp_ref_wait_max);
    lp_printf(b, "refresh_ximr_us_max=%d\n", (int) lp_ref_ximr_max);
    lp_printf(b, "refresh_conv_us_avg=%d\n", rn ? (int) (lp_ref_conv_sum / rn) : 0);
    lp_printf(b, "refresh_wait_us_avg=%d\n", rn ? (int) (lp_ref_wait_sum / rn) : 0);
    lp_printf(b, "refresh_ximr_us_avg=%d\n", rn ? (int) (lp_ref_ximr_sum / rn) : 0);
    lp_census_m72a(b);
    lp_printf(b, "end_sample=%s\n", tag);
}

/* VMIX buffer index: Canon flips it once per VMIX_TransferRectangleToVram.
 * Sampled every ~10 ms, so the change count is a lower bound. Bounded by
 * time and by sample count (ML's clock is not guaranteed monotonic on SMP). */
#define LP_POLL_MAX_SAMPLES 150
static void lp_vmix_poll(struct lp_buf * b)
{
    uint32_t prev = RD(LP_VMIX + 0x10);
    int changes = 0, pending = 0, samples = 0;
    int n_size = 0, n_img = 0, n_lock = 0, n_disdraw = 0, n_disref = 0, n_seq = 0;
    int t0 = get_ms_clock(), t = t0;
    while (samples < LP_POLL_MAX_SAMPLES)
    {
        msleep(10);
        uint32_t v = RD(LP_VMIX + 0x10);
        if (v != prev) { changes++; prev = v; }
        if (RD(LP_VMIX + 0x18)) pending++;
        if (RD(LP_VMIX + 0x14)) n_size++;
        if (RD(LP_VMIX + 0x20) && !RD(LP_VMIX + 0x24)) n_img++;
        if (RD(LP_WINSYS + 0x8)) n_lock++;
        if ((int) RD(LP_WINSYS + 0xC) > 0) n_disdraw++;
        if (RD(LP_WINSYS + 0x10)) n_disref++;
        if (RD(LP_DTC_SEQ)) n_seq++;
        samples++;
        t = get_ms_clock();
        if (t < t0 || t - t0 >= 1000) break;
    }
    lp_printf(b, "vmix_poll_ms=%d\n", t - t0);
    lp_printf(b, "vmix_poll_samples=%d\n", samples);
    lp_printf(b, "vmix_buf_index_changes=%d\n", changes);
    lp_printf(b, "vmix_pending_samples=%d\n", pending);
    lp_printf(b, "vmix_size_samples=%d\n", n_size);              /* 0x18418 != 0 */
    lp_printf(b, "vmix_img_samples=%d\n", n_img);                /* 0x18424 != 0 && 0x18428 == 0 */
    lp_printf(b, "winsys_lock_samples=%d\n", n_lock);            /* 0x18438 != 0 */
    lp_printf(b, "winsys_disdraw_samples=%d\n", n_disdraw);      /* (int) 0x1843C > 0 */
    lp_printf(b, "winsys_disref_samples=%d\n", n_disref);        /* 0x18440 != 0 */
    lp_printf(b, "dtc_seq_samples=%d\n", n_seq);                 /* 0x21A58 != 0 */
}

/* ---- log snapshot -------------------------------------------------------- */
#define LP_LOG_SIZE (64 * 1024)   /* M7.2a: about 45 KB with the census extension */
#define LP_FILE_MAX 99999
static char lp_snap_msg[32] = "";
static volatile int lp_snap_busy = 0;     /* one snapshot at a time */
static int lp_snap_seq = 0;               /* per boot */
static int lp_file_no = 0;                /* last file number used in this boot */

/* each snapshot goes to a new file: no append, no seek, no overwrite */
static int lp_write_log(struct lp_buf * b, char * name, int name_size)
{
    if (!is_dir("ML/LOGS"))
        FIO_CreateDirectory("ML/LOGS");
    name[0] = 0;
    for (int i = 0, n = lp_file_no + 1; i < 1000 && n <= LP_FILE_MAX; i++, n++)
    {
        snprintf(name, name_size, "ML/LOGS/LVP%05d.LOG", n);
        if (!is_file(name)) { lp_file_no = n; break; }
        name[0] = 0;
    }
    if (!name[0]) return 0;
    FILE * f = FIO_CreateFile(name);
    if (!f) return 0;
    int w = FIO_WriteFile(f, b->p, b->len);
    FIO_CloseFile(f);
    return w == b->len;
}

/* caller holds lp_snap_busy */
static void lp_snapshot_do(int delay_ms)
{
    char name[32];
    snprintf(lp_snap_msg, sizeof(lp_snap_msg), "running");
    if (delay_ms) msleep(delay_ms);
    struct lp_buf b = { .p = malloc(LP_LOG_SIZE), .len = 0, .cap = LP_LOG_SIZE };
    if (!b.p)
    {
        snprintf(lp_snap_msg, sizeof(lp_snap_msg), "no memory");
        return;
    }
    lp_printf(&b, "# LVPROBE M7.0 snapshot\n");
    lp_printf(&b, "# format: key=value; raw words 0x%%08x, counts decimal, 64-bit IBus 0xHI:LO;\n");
    lp_printf(&b, "# NULL = Canon pointer 0; invalid(...) / uncached_unverified(...) = not dereferenced\n");
    lp_printf(&b, "probe_stage=M7.2a\n");
    lp_printf(&b, "snapshot_seq=%d\n", ++lp_snap_seq);
    lp_printf(&b, "boot_ms=%d\n", get_ms_clock());
    lp_printf(&b, "ml_build_version=%s\n", build_version);
    char id[24];
    snprintf(id, sizeof(id), "%s", build_id);
    for (char * c = id; *c; c++) if (*c == ' ' || *c == '=') *c = 0;   /* build_id ends with a space */
    lp_printf(&b, "ml_build_id=%s\n", id);
    lp_census(&b, "A");
    msleep(500);
    lp_census(&b, "B");
    lp_vmix_poll(&b);
    lp_printf(&b, "log_bytes=%d\n# end\n", b.len);
    int ok = lp_write_log(&b, name, sizeof(name));
    const char * base = name[0] ? name + 8 : "?";     /* without "ML/LOGS/" */
    if (ok) snprintf(lp_snap_msg, sizeof(lp_snap_msg), "%s, %d B", base, b.len);
    else snprintf(lp_snap_msg, sizeof(lp_snap_msg), "write FAILED (%s)", base);
    free(b.p);
}

/* direct call (QEMU test); 0 = another snapshot is running */
int lvprobe_snapshot_run(int delay_ms)
{
    if (lp_snap_busy) return 0;
    lp_snap_busy = 1;
    lp_snapshot_do(delay_ms);
    lp_snap_busy = 0;
    return 1;
}

static void lp_snapshot_task(int unused)
{
    lp_snapshot_do(4000);    /* menu closed; user shows the LV screen */
    lp_snap_busy = 0;
}

/* ---- status line --------------------------------------------------------- */
static int lp_status_on = 0;              /* 0 = OFF, 1 = ON, 2 = ON + refresh while DISPLAY_IS_ON == 0 and EVF active */
static int lp_status_task_started = 0;
static volatile uint32_t lp_forced_refresh = 0;

/* "-" if not readable */
static const char * lp_hex(char * s, int ok, uint32_t v)
{
    if (ok) snprintf(s, 12, "%x", (int) v); else snprintf(s, 12, "-");
    return s;
}

static const char * lp_dec(char * s, int ok, uint32_t v)
{
    if (ok) snprintf(s, 12, "%d", (int) v); else snprintf(s, 12, "-");
    return s;
}

/* EVF output active per Canon's display mask (unknown -> no) */
static int lp_evf_active(void)
{
    uint32_t mask = 0;
    return fld(RD(LP_DISPDEV_PTR), 0x18, &mask) && (mask & LP_MASK_EVF);
}

/* compact counter: up to 9999, then k / M units (at most 5 characters) */
static const char * lp_cnt(char * s, uint32_t v)
{
    if (v < 10000) snprintf(s, 8, "%d", (int) v);
    else if (v < 10000000) snprintf(s, 8, "%dk", (int) (v / 1000));
    else snprintf(s, 8, "%dM", (int) (v / 1000000));
    return s;
}

/* duration: ms up to 9999, then s */
static const char * lp_ms(char * s, uint32_t ms)
{
    if (ms < 10000) snprintf(s, 8, "%dms", (int) ms);
    else snprintf(s, 8, "%ds", (int) (ms / 1000));
    return s;
}

/* one status line: printed only if its text changed (or lp_sl_force);
 * padded with spaces to the width drawn last time */
#define LP_SLINES 13
#define LP_SLINE_MAX 64
static char lp_sl_prev[LP_SLINES][LP_SLINE_MAX];
static int lp_sl_w[LP_SLINES];
static int lp_sl_force = 1;
static uint32_t lp_sl_printed = 0;       /* lines printed (QEMU test reads it) */

static void lp_sline_put(int i, int font, int x, int y, char * t)
{
    t[LP_SLINE_MAX - 1] = 0;
    if (!lp_sl_force && !strcmp(t, lp_sl_prev[i])) return;
    strcpy(lp_sl_prev[i], t);
    int n = strlen(t);
    while (n < lp_sl_w[i] && n < LP_SLINE_MAX - 1) t[n++] = ' ';
    t[n] = 0;
    lp_sl_w[i] = n;
    bmp_printf(font, x, y, "%s", t);
    lp_sl_printed++;
}
/* snprintf (ML) instead of a va_list helper: no new call edge to Canon's vsnprintf */
#define lp_sline(i, font, x, y, ...) do { char _t[LP_SLINE_MAX]; \
    snprintf(_t, sizeof(_t), __VA_ARGS__); lp_sline_put(i, font, x, y, _t); } while (0)

static void lp_status_draw(int cnt)
{
    char s1[12], s2[12], s3[12], s4[12], s5[12];
    uint32_t v, lo, hi, w5 = 0, w6 = 0, fmt = 0;
    int len = 0;
    int font = FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK);
    int x = 40, y = 120, dy = 23;

    lp_uc_update();

    int seen = lv_action_raw_get(&v, &len);
    lp_sline(0, font, x, y + 0 * dy, "LVP %d lv=%d act=%s/%d n=%d fol=%d R%d", cnt, lv,
             lp_hex(s1, seen, v), len, (int) lp_ring_count, lv_follow_canon_get(), (int) lp_forced_refresh);

    uint32_t dd = RD(LP_DISPDEV_PTR), mask = 0;
    int mask_ok = fld(dd, 0x18, &mask);
    lp_sline(1, font, x, y + 1 * dy, "gm=%x gs=%d disp=%d dlg=%x ovl=%d",
             (int) RD(LP_GUI_MODE), gui_state, (int) RD(LP_DISPLAY_IS_ON),
             (int) RD(LP_LVAPP_DIALOG), (int) RD(LP_LV_OVERLAYS));

    uint32_t xcm = RD(LP_XCM_TABLE), nctx = 0;
    int nctx_ok = fld(xcm, 0xC, &nctx);
    lp_sline(2, font, x, y + 2 * dy, "mask=%s vm=%d xcm=%s lvl=%x ss=%x",
             lp_hex(s1, mask_ok, mask), (int) RD(LP_VRAMMGR_MODE), lp_hex(s2, nctx_ok, nctx),
             (int) RD(LP_LEVEL_DIALOG), (int) RD(LP_SILENT_DIALOG));

    uint32_t dvc = RD(LP_DVC_PTR);
    int dvc_ok = dvc && ram_ok(dvc, 4);
    int p_ok = dvc_ok && rd64(dvc + 0x130, &lo, &hi) == 1;
    int i_ok = fld(dvc, 0x1C8 + 4 * 5, &w5) && fld(dvc, 0x1C8 + 4 * 6, &w6) && fld(dvc, 0x1C8 + 4 * 0xe, &fmt);
    lp_sline(3, font, x, y + 3 * dy, "pnl=%s:%s f=%s %sx%s",
             lp_hex(s1, p_ok, hi), lp_hex(s2, p_ok, lo),
             lp_hex(s3, i_ok, fmt), lp_hex(s4, i_ok, w5), lp_hex(s5, i_ok, w6));

    int e_ok = dvc_ok && rd64(dvc + 0x140, &lo, &hi) == 1;
    lp_sline(4, font, x, y + 4 * dy, "evf=%s:%s vmix=%d/%d",
             lp_hex(s1, e_ok, hi), lp_hex(s2, e_ok, lo),
             (int) RD(LP_VMIX + 0x10), (int) RD(LP_VMIX + 0x18));

    lp_sline(5, font, x, y + 5 * dy, "ref=%d/%d/%dus free=%dK",
             (int) lp_ref_conv, (int) lp_ref_wait, (int) lp_ref_ximr,
             GetFreeMemForMalloc() / 1024);

    /* XOC layer enable of XCM 0, outputs 0 and 1; expsim (raw / ML); digic gain */
    uint32_t x0 = 0, x1 = 0;
    int x0_ok = fld(RD(LP_XOC_TABLE), 4, &x0), x1_ok = fld(RD(LP_XOC_TABLE + 4), 4, &x1);
    lp_sline(6, font, x, y + 6 * dy, "xoc=%s/%s es=%d/%d g=%d",
             lp_hex(s1, x0_ok, x0), lp_hex(s2, x1_ok, x1),
             _expsim, get_expsim(), get_digic_iso_gain_photo());

    /* M7.2a, small font (8x12), at most 50 characters per line */
    int fs = FONT(FONT_SMALL, COLOR_WHITE, COLOR_BLACK);
    int ys = y + 7 * dy, dys = 13;
    char c[6][8];
#ifdef CONFIG_R6_REFRESH_GUARD
    /* g: XimrExe ok, take timeouts, busy, ML XimrExe with EVF bit, overlap suspects
     * s: skips per enforced gate (pending, size change, context count, frame in flight, no semaphore)
     * s: settle, rate, piggyback conversions / confirmed / re-armed, Canon active after the conversion
     * w: current starvation episode, longest, episodes > 500 ms, reasons of the current one, last change */
    const struct r6_refresh_guard_stats * g = r6_refresh_guard_stats();
    lp_sline(7, fs, x, ys + 0 * dys, "g ok=%s to=%s bsy=%s ev=%s ov=%s",
             lp_cnt(c[0], g->ok), lp_cnt(c[1], g->take_timeout), lp_cnt(c[2], g->busy),
             lp_cnt(c[3], g->exe_evf), lp_cnt(c[4], g->overlap));
    lp_sline(8, fs, x, ys + 1 * dys, "s pd=%s sz=%s cx=%s im=%s ns=%s",
             lp_cnt(c[0], g->pend), lp_cnt(c[1], g->size), lp_cnt(c[2], g->ctx),
             lp_cnt(c[3], g->img), lp_cnt(c[4], g->no_sem));
    lp_sline(9, fs, x, ys + 2 * dys, "s st=%s rt=%s pg=%s/%s/%s pl=%s",
             lp_cnt(c[0], g->settle), lp_cnt(c[1], g->rate), lp_cnt(c[2], g->pig),
             lp_cnt(c[3], g->pig_ok), lp_cnt(c[4], g->pig_rearm), lp_cnt(c[5], g->pig_late));
    lp_sline(10, fs, x, ys + 3 * dys, "w %s max=%s n=%s m=%x c=%x",
             lp_ms(c[0], r6_refresh_guard_starve_ms()), lp_ms(c[1], g->starve_ms_max),
             lp_cnt(c[2], g->starve_n), (int) (g->ep_active ? g->ep_mask : 0), (int) g->change_mask);
#else
    lp_sline(7, fs, x, ys, "g off");
#endif

    /* h: numPix (hypothesis), arrays with sum == numPix, set (0/1 DISP; bins
     * only read for these), RandomData FrameNum, FaceYuv IBus channel;
     * h: mean bin per array, bin 0 and bin 255 of array 0 */
    uint32_t hctx = RD(LP_HIST_STATE + 4), hp[4], n = 0, eq = 0, mean[4] = { 0 }, y0 = 0, y255 = 0;
    int set = lp_hist_ptrs(hctx, hp), hok = 0;
    if (set == 0 || set == 1)
    {
        n = RD(hctx + 4);
        struct lp_hstat hs;
        for (int i = 0; i < 4; i++)
        {
            if (!lp_hist_stats(hp[i], &hs)) continue;
            hok |= 1 << i;
            mean[i] = hs.mean;
            if (i == 0) { y0 = hs.lo[0]; y255 = hs.hi[7]; }
            if (hs.sum == n) eq++;
        }
    }
    int src = 0;
    uint32_t rd = lp_rd_base(&src), fr = rd ? RD(rd) : 0;
    struct lp_q fq;
    lp_q64(&fq, RD(LP_FY_STATE + 0xC), 0x20);
    lp_sline(11, fs, x, ys + 4 * dys, "h n=%s eq=%d set=%d fr=%s ch=%s",
             lp_cnt(c[0], n), (int) eq, set, lp_hex(s1, rd != 0, fr), lp_hex(s2, fq.r == 1, fq.hi));
    lp_sline(12, fs, x, ys + 5 * dys, "h m=%s/%s/%s/%s y0=%s y255=%s",
             lp_dec(s1, hok & 1, mean[0]), lp_dec(s2, hok & 2, mean[1]),
             lp_dec(s3, hok & 4, mean[2]), lp_dec(s4, hok & 8, mean[3]),
             lp_cnt(c[1], y0), lp_cnt(c[2], y255));
}

#define LP_STATUS_TICK_MS  200
#define LP_STATUS_DRAW_N   5      /* compose the text every 5 ticks = 1 s */
#define LP_STATUS_FORCE_N  25     /* print all lines every 25 ticks = 5 s */

static void lp_status_task(void * unused)
{
    int cnt = 0, tick = 0, menu_seen = 0;
    TASK_LOOP
    {
        if (lp_status_on && !gui_menu_shown())
        {
            if (menu_seen || tick % LP_STATUS_FORCE_N == 0) lp_sl_force = 1;
            if (lp_sl_force || tick % LP_STATUS_DRAW_N == 0)
            {
                lp_status_draw(++cnt);
                lp_sl_force = 0;
            }
            menu_seen = 0;
            tick++;
            /* redraw_task refreshes only while DISPLAY_IS_ON; "ON, refresh"
             * refreshes here as well, like menu_redraw_flood does, but only
             * for the EVF (mask bit 4), never during power-save display off
             * or shutdown (same shutdown check as bmp.c's refresh) */
            if (lp_status_on == 2 && !DISPLAY_IS_ON && ml_refresh_display_needed
                && !ml_shutdown_requested && lp_evf_active())
            {
#ifdef CONFIG_R6_REFRESH_GUARD
                /* R<n> counts refreshes that converted (own XimrExe or
                 * piggyback), not the ones the guard skipped */
                const struct r6_refresh_guard_stats * g = r6_refresh_guard_stats();
                uint32_t done0 = g->ok + g->pig;
                refresh_yuv_from_rgb();
                if (g->ok + g->pig != done0) lp_forced_refresh++;
#else
                refresh_yuv_from_rgb();
                lp_forced_refresh++;
#endif
            }
        }
        else if (gui_menu_shown())
            menu_seen = 1;
        msleep(LP_STATUS_TICK_MS);
    }
}

void lvprobe_status_enable(int mode)
{
    lp_status_on = COERCE(mode, 0, 2);
    lp_sl_force = 1;                     /* print every line on the next draw */
    if (lp_status_on && !lp_status_task_started)
    {
        lp_status_task_started = 1;
        task_create("lvprobe_status", 0x1e, 0x2000, lp_status_task, 0);
    }
}

/* QEMU test only reads it; also shown as R<n> on the status line */
uint32_t lvprobe_forced_refresh_count(void) { return lp_forced_refresh; }
uint32_t lvprobe_status_lines_printed(void) { return lp_sl_printed; }

/* ---- menu ---------------------------------------------------------------- */
static int lp_follow_menu = 0;

static MENU_SELECT_FUNC(lp_follow_toggle)
{
    lp_follow_menu = !lp_follow_menu;
    lv_follow_canon_set(lp_follow_menu);
}

static MENU_SELECT_FUNC(lp_status_toggle)
{
    lvprobe_status_enable(MOD(lp_status_on + delta, 3));
}

/* menu task: check and set the busy flag here, so a second press during the
 * 4 s wait cannot start another task */
static MENU_SELECT_FUNC(lp_snapshot_select)
{
    if (lp_snap_busy)
    {
        snprintf(lp_snap_msg, sizeof(lp_snap_msg), "busy");
        return;
    }
    lp_snap_busy = 1;
    snprintf(lp_snap_msg, sizeof(lp_snap_msg), "starting");
    run_in_separate_task(lp_snapshot_task, 0);
}

static MENU_UPDATE_FUNC(lp_snapshot_update)
{
    if (lp_snap_msg[0]) MENU_SET_VALUE("%s", lp_snap_msg);
}

static struct menu_entry lp_menu[] = {
    {
        .name   = "LV probe",
        .select = menu_open_submenu,
        .help   = "M7.0 read-only LiveView census (RAM reads only).",
        .children = (struct menu_entry[]) {
            {
                .name   = "LV flag follows Canon",
                .priv   = &lp_follow_menu,
                .max    = 1,
                .select = lp_follow_toggle,
                .help   = "ON: lv = !PROP_LV_ACTION. OFF (default, not saved): lv = 0 as in M6.",
                .help2  = "The ML menu always opens over Canon MENU (0x2003).",
            },
            {
                .name   = "Status line",
                .priv   = &lp_status_on,
                .max    = 2,
                .choices = (const char *[]) { "OFF", "ON", "ON, refresh" },
                .select = lp_status_toggle,
                .help   = "lv, GUI, display, LV buffers, guard: 1/s, changed lines only.",
                .help2  = "ON, refresh: also refresh for the EVF. Use ON for display off.",
            },
            {
                .name   = "Log snapshot",
                .select = lp_snapshot_select,
                .update = lp_snapshot_update,
                .help   = "Menu closes; show the LV screen within 4 s -> ML/LOGS/LVP*.LOG",
                .help2  = "Read-only: Canon RAM values, key=value per line.",
            },
            MENU_EOL
        },
    },
};

static void lp_init()
{
    menu_add("Debug", lp_menu, COUNT(lp_menu));
}

INIT_FUNC("lvprobe", lp_init);

#endif /* FEATURE_LV_BUFFER_PROBE */
