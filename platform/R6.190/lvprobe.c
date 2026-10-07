/*
 * R6.190 M7.0: read-only LiveView census probe.
 *
 * Debug > "LV probe":
 *  - "LV flag follows Canon": lv = !PROP_LV_ACTION (propvalues.c,
 *    CONFIG_LV_FOLLOW_TOGGLE). Not saved; every boot starts with lv = 0 as
 *    in M6. M7.6: r6_autostart (CONFIG_R6_AUTOSTART) switches it ON at boot
 *    after a clean shutdown.
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
 * M7.3 (approved by the user 2026-10-01): the PSR mask word 0xDFFC4FA8 is
 * read (LP_READ_PSR_MASK), still only if the DF section is Normal memory;
 * "FaceYuv sample" (menu one-shot, and in every snapshot sample) reads a
 * sparse grid of FaceYuv pixels: Canon's 640x426 8-bit YUV 4:2:2 image on
 * IBus channel 1 (M7.2a: static window 5, plain RAM), outside IMGVRAM,
 * only through the uncached alias of a static window 4..7, after checking
 * the context, the geometry, the buffer pointer (channel 1, aligned) and that
 * the whole 0x85200-byte buffer lies inside the window, inside channel 1's
 * RAM end and inside the verified uncached region; recomputed per sample.
 * 32-bit loads only, nothing written. The status line pads every line to a
 * fixed width and shows the ownership refresh (r6_layer.c) and the histogram
 * overlay (r6_overlays.c).
 *
 * No Canon calls beyond what ML already uses: FIO through the ML wrappers,
 * msleep, task_create, vsnprintf, property handler registration via
 * PROP_HANDLER. No new MMIO by ML; existing M6 accesses: the DIGIC timer
 * 0xC0242014 (get_ms_clock/get_us_clock, read more often in this build) and
 * Canon's XimrExe (also from the status task in "ON, refresh").
 * No property writes, no EDMAC, no LV pixel reads, no memif window lookups
 * through Canon code.
 *
 * M9.1: "Expo log" (Tor 0, read-only exposure values, section expo.* of the
 * snapshot, see lp_x_* below); the RandomData ring is read with its 20
 * slots; the grid's counters (grid.*, r6_grid.c).
 *
 * M10.2: the Tv write path's state and its 32-entry write ring (pw.*, from
 * r6_propw.c), R6 Interval's bracket (iv.bkt.*), and the Tor 0d fields of the
 * expo log (D1, read only: Canon's multi-shot settings, the raw C.Fn Tv range,
 * the CC backups, the flash predicate inputs, the CC object globals).
 *
 * M11.0 (CONFIG_R6_ETTR, docs/m11_plan.md 6.1 / 6.2): every snapshot writes a
 * second file ML/LOGS/ETTnnnnn.LOG with the number of its LVPnnnnn.LOG (R6
 * ETTR: a full histogram measurement with the 4 x 256 bins, the advisor,
 * the ALO set, the 20 RandomData slots, the Tor 0e fields of answer E2 and
 * R6 ETTR's rings), into the same buffer after the LVP file is written; the
 * main log gets the one line ettr.file= before the ring tail. Read only, every
 * pointer checked; an existing ETT file is never overwritten.
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
#ifdef CONFIG_R6_LAYER_OWNERSHIP
#include "r6_layer.h"
#endif
#ifdef FEATURE_R6_CANON_HISTOGRAM
#include "r6_overlays.h"
#endif
#include "r6_faceyuv.h"
#ifdef CONFIG_R6_GLOBAL_DRAW
#include "r6_gdraw.h"
#endif
#ifdef CONFIG_R6_INTERVAL
#include "lens.h"
#include "r6_interval.h"
#endif
#ifdef CONFIG_R6_PROPW_TV
#include "r6_propw.h"
#endif
#ifdef CONFIG_R6_GRID
#include "r6_grid.h"
#endif
#ifdef CONFIG_R6_MOTION
#include "r6_faceyuv.h"
#include "r6_motion.h"
#endif
#ifdef CONFIG_R6_ETTR
#include "r6_ettr.h"
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

#ifdef CONFIG_R6_AUTOSTART
#ifndef CONFIG_R6_GLOBAL_DRAW
#error "CONFIG_R6_AUTOSTART needs CONFIG_R6_GLOBAL_DRAW (r6_global_draw_boot, zebra.c)"
#endif
extern void r6_global_draw_boot(int keep);   /* zebra.c */
/* r6_autostart: -2 = not called yet, else its argument (1 clean, 0 / -1 not) */
static int lp_autostart = -2;
#endif

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
#define LP_WINSYS_ORIGIN 0x18358   /* M7.6: 4 x {x, y, w, h}: WINSYS_SetRGBAVramActualSizeAndOffset 0xE078C82C
                                    * stores origin i at 0x1832C+0x2C+16*i (strd at 0xE078C87A / 0xE078C850);
                                    * boot (0xE09D2AD2..0xE09D2B00): 720x480@(152,30), 720x540@(152,0),
                                    * 960x540@(32,0), 1024x540@(0,0) */
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
#define LP_RD_RING_N     20        /* M9.1: 20 slots (ring step mod 0x14, 0xE079C69E; camera LVP00045 index 0x11),
                                    * M7.2a..M8.2 read only 8 and refused an index >= 8 */
#define LP_RD_SIZE       0xF00     /* 'RandomData Total(%d)' 0xE079C2B6 */
#define LP_FY_STATE      0x1ED68   /* LvFaceYuvController: +0xC = ctx (0xB060 bytes, 0xE0808F32), s8 [+0] ring
                                    * index; ctx+0x20: 4 x 64-bit IBus addresses (0xE08092DC..0xE0809386) */
#define LP_FY_CTX_SIZE   0xB060
#define LP_FY_RING       0x164614  /* 3 x {flag, word}, index s8 MEM(0x1ED68) (per-frame step 0xE0809212); the
                                    * word goes through 0xE08090C4 (& ~0xC0000000) but on the camera it is
                                    * RandomData's FrameNum (M7.2a: newest entry == rd.framenum) */
#define LP_LVIC_SIZE     0x11FC8   /* u16 w, u16 h (QEMU snapshot 0x01AA0280 = 640 x 426) */
#define LP_MEMIF_DBG     0x7248    /* != 0: SetTarget 0xE01AF274 runs the arbiter and PSR checks (literal 0xE01AF384) */
#define LP_ARB           0x13290   /* MemifArbiter: [0x13290] == 0xFF -> other branch (0xE081DD0E);
                                    * pid byte for DRAM ch = 0x13290 + (ch-1)*0x16F + pid (0xE081DD1C) */
#define LP_ARB_CH3_PID5  0x13573   /* = 0x13290 + 2*0x16F + 5 */
#define LP_PSR_MASK      0xDFFC4FA8 /* bit ch-1 = DRAM channel in partial self-refresh (0xE013311C, check
                                    * MemifSelfRefresh 0xE0865560). Not an L1 word: in the DF on-chip area
                                    * next to CPU1 code/stack (QEMU: pc 0xDFFC491C, sp 0xDFFC5400) */
#define LP_READ_PSR_MASK            /* approved by the user 2026-10-01: read only if the DF section decodes as
                                    * Normal memory (M7.2a camera: L1 0xDF054406 = normal) */
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
struct lp_buf { char * p; int len; int cap; int trunc; };

/* text that does not fit is cut and flagged (trunc), the trailer
 * (log_truncated / log_bytes / # end) has its own reserve, see lp_snapshot_do */
static void lp_printf(struct lp_buf * b, const char * fmt, ...)
{
    if (!b->p) return;
    if (b->len >= b->cap - 1) { b->trunc = 1; return; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->p[b->cap - 1] = 0;
    b->len += strlen(b->p + b->len);
    if (b->len >= b->cap - 1) b->trunc = 1;
}

/* a pointer that was not dereferenced */
static void lp_bad(struct lp_buf * b, const char * key, uint32_t base)
{
    if (lp_is_uncached(base) && !lp_uc_ok && base >= 0x40001000)
        lp_printf(b, "%s=uncached_unverified(0x%08x)\n", key, (int) base);
    else
        lp_printf(b, "%s=invalid(0x%08x)\n", key, (int) base);
}

/* a name as one token (spaces -> '_'): every log line stays key=value without spaces (M9.2: also the overlay
 * reason names, e.g. "ML bulb picture") */
static const char * lp_nosp(const char * s, char * buf, int n)
{
    int i = 0;
    for (; s[i] && i < n - 1; i++) buf[i] = s[i] == ' ' ? '_' : s[i];
    buf[i] = 0;
    return buf;
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

/* ---- M7.3: FaceYuv read-only sample (approved 2026-10-01) ---------------
 * M7.4: the buffer choice and all pointer checks moved to r6_fy_locate
 * (r6_faceyuv.c, same rules and refusal names); this file only samples the
 * pixel grid of the located buffer. */
#define LP_FY_PITCH      R6_FY_PITCH
#define LP_FY_GET_IDX    0xB029                      /* census keys (fy.get_idx / rec_idx / rec[k]) */
#define LP_FY_REC_IDX    0xB02A
#define LP_FY_REC        0x40
#define LP_FY_REC_STRIDE 0xA0
#define LP_FY_GX         32                          /* sample grid 32 x 24 words */
#define LP_FY_GY         24

struct lp_fys
{
    int st;                  /* 1 = sampled, 0 = refused (why) */
    const char * why;
    int t_ms;
    int ring_i;              /* s8 MEM(0x1ED68): next ring slot */
    uint32_t ring_flag, ring_frame;
    int gidx, ridx, gidx_after;
    int pick, src;           /* buffer 0..3; src 1 rec, 2 rec_old, 3 get */
    int rec;                 /* record used, -1 none */
    uint32_t rec_frame;
    uint32_t lo, hi, cpu;
    int win;
    uint32_t n;
    uint32_t lsum[4], lmin[4], lmax[4];     /* per byte lane of the 32-bit words */
    uint32_t w0;
};
static struct lp_fys lp_fy_last;            /* last menu one-shot */
static char lp_fy_msg[40] = "";

static int lp_fy_fail(struct lp_fys * r, const char * why)
{
    r->st = 0;
    r->why = why;
    return 0;
}

static int lp_fy_sample(struct lp_fys * r)
{
    memset(r, 0, sizeof(*r));
    r->t_ms = get_ms_clock();
    r->why = "";
    lp_uc_update();

    /* shared locate (r6_faceyuv.c): buffer choice, pointer and range checks */
    struct r6_fy_loc loc;
    int ok = r6_fy_locate(&loc);
    r->ring_i = loc.ring_i; r->ring_flag = loc.ring_flag; r->ring_frame = loc.ring_frame;
    r->gidx = loc.gidx; r->ridx = loc.ridx;
    r->pick = loc.pick; r->src = loc.src; r->rec = loc.rec; r->rec_frame = loc.rec_frame;
    r->lo = loc.lo; r->hi = loc.hi; r->cpu = loc.cpu; r->win = loc.win;
    if (!ok) return lp_fy_fail(r, loc.why);
    uint32_t g = 0, ctx = RD(LP_FY_STATE + 0xC);

    for (int i = 0; i < 4; i++) { r->lmin[i] = 0xFF; r->lmax[i] = 0; }
    for (int j = 0; j < LP_FY_GY; j++)
    {
        uint32_t y = 8 + j * 17;                       /* 8..399 */
        for (int i = 0; i < LP_FY_GX; i++)
        {
            uint32_t x = 4 + i * 20;                   /* even: 4-byte aligned 4:2:2 pair */
            uint32_t w = RD(r->cpu + y * LP_FY_PITCH + x * 2);
            if (!r->n) r->w0 = w;
            for (int l = 0; l < 4; l++)
            {
                uint32_t c = (w >> (8 * l)) & 0xFF;
                r->lsum[l] += c;
                if (c < r->lmin[l]) r->lmin[l] = c;
                if (c > r->lmax[l]) r->lmax[l] = c;
            }
            r->n++;
        }
    }
    rd8(ctx + LP_FY_GET_IDX, &g);
    r->gidx_after = g;
    r->st = 1;
    return 1;
}

/* mean x10 of lane l, or of lanes a and b */
static int lp_fy_m10(const struct lp_fys * r, int a, int b)
{
    uint32_t s = r->lsum[a] + (b >= 0 ? r->lsum[b] : 0), n = r->n * (b >= 0 ? 2 : 1);
    return n ? (int) (s * 10 / n) : 0;
}

static void lp_fy_log(struct lp_buf * b, const char * p, const struct lp_fys * r)
{
    static const char * const srcs[4] = { "none", "rec", "rec_old", "get" };
    if (r->st) lp_printf(b, "%s.status=ok\n", p);
    else lp_printf(b, "%s.status=refused(%s)\n", p, r->why);
    lp_printf(b, "%s.t_ms=%d\n", p, r->t_ms);
    lp_printf(b, "%s.ring_i=%d\n", p, r->ring_i);
    lp_printf(b, "%s.ring_frame=%d\n", p, (int) r->ring_frame);
    lp_printf(b, "%s.ring_flag=%d\n", p, (int) r->ring_flag);
    lp_printf(b, "%s.get_idx=%d\n", p, r->gidx);
    lp_printf(b, "%s.rec_idx=%d\n", p, r->ridx);
    lp_printf(b, "%s.pick=%d\n", p, r->pick);
    lp_printf(b, "%s.src=%s\n", p, srcs[r->src & 3]);
    lp_printf(b, "%s.rec=%d\n", p, r->rec);
    lp_printf(b, "%s.rec_frame=%d\n", p, (int) r->rec_frame);
    lp_printf(b, "%s.ibus=0x%x:%08x\n", p, (int) r->hi, (int) r->lo);
    lp_printf(b, "%s.window=%d\n", p, r->win);
    lp_printf(b, "%s.cpu=0x%08x\n", p, (int) r->cpu);
    if (!r->st) return;
    lp_printf(b, "%s.get_idx_after=%d\n", p, r->gidx_after);
    lp_printf(b, "%s.n=%d\n", p, (int) r->n);
    lp_printf(b, "%s.w0=0x%08x\n", p, (int) r->w0);
    for (int l = 0; l < 4; l++)
    {
        lp_printf(b, "%s.lane[%d].mean_x10=%d\n", p, l, lp_fy_m10(r, l, -1));
        lp_printf(b, "%s.lane[%d].min=%d\n", p, l, (int) r->lmin[l]);
        lp_printf(b, "%s.lane[%d].max=%d\n", p, l, (int) r->lmax[l]);
    }
    /* UYVY: U Y0 V Y1 (lanes 0 1 2 3, little endian word); YUYV: Y0 U Y1 V */
    lp_printf(b, "%s.uyvy.y_mean_x10=%d\n", p, lp_fy_m10(r, 1, 3));
    lp_printf(b, "%s.uyvy.y_min=%d\n", p, (int) MIN(r->lmin[1], r->lmin[3]));
    lp_printf(b, "%s.uyvy.y_max=%d\n", p, (int) MAX(r->lmax[1], r->lmax[3]));
    lp_printf(b, "%s.uyvy.u_mean_x10=%d\n", p, lp_fy_m10(r, 0, -1));
    lp_printf(b, "%s.uyvy.v_mean_x10=%d\n", p, lp_fy_m10(r, 2, -1));
    lp_printf(b, "%s.yuyv.y_mean_x10=%d\n", p, lp_fy_m10(r, 0, 2));
    lp_printf(b, "%s.yuyv.y_min=%d\n", p, (int) MIN(r->lmin[0], r->lmin[2]));
    lp_printf(b, "%s.yuyv.y_max=%d\n", p, (int) MAX(r->lmax[0], r->lmax[2]));
    lp_printf(b, "%s.yuyv.u_mean_x10=%d\n", p, lp_fy_m10(r, 1, -1));
    lp_printf(b, "%s.yuyv.v_mean_x10=%d\n", p, lp_fy_m10(r, 3, -1));
}

/* text of the last one-shot (menu value; the QEMU test reads it) */
const char * lvprobe_faceyuv_msg(void) { return lp_fy_msg; }

/* menu one-shot; returns 1 if sampled (QEMU test calls it too) */
int lvprobe_faceyuv_oneshot(void)
{
    int ok = lp_fy_sample(&lp_fy_last);
    const struct lp_fys * r = &lp_fy_last;
    if (ok)
        snprintf(lp_fy_msg, sizeof(lp_fy_msg), "b%d UYVY Y%d, YUYV Y%d", r->pick,
                 lp_fy_m10(r, 1, 3) / 10, lp_fy_m10(r, 0, 2) / 10);
    else
        snprintf(lp_fy_msg, sizeof(lp_fy_msg), "refused: %s", r->why);
    return ok;
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
    LG("act_seq", g->act_seq);
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
    LG("t_settle", g->t_settle);
    LG("settle_len_ms", g->settle_len_ms);
    LG("settle_xoc", g->settle_xoc);
    LG("settle_xoc_full", g->settle_xoc_full);
    LG("settle_keep", g->settle_keep);
    LG("settle_disp", g->settle_disp);
    LG("clear_locks", g->clear_locks);
    LG("clear_waits", g->clear_waits);
    LG("clear_timeouts", g->clear_timeouts);
    LG("clear_wait_ms_max", g->clear_wait_ms_max);
    LG("busy_clear", g->busy_clear);
    LG("idx_changes", g->idx_changes);
    LG("t_idx_change", g->t_idx_change);
    LGX("w_mask", g->w_mask);
    LGX("w_vrammgr_mode", g->w_vm);
    LGX("w_gui_disptype", g->w_disptype);
    LGX("w_xoc_enable", g->w_xoc);
    LGX("w_xoc_handle", g->w_xoc_h);
    LG("w_xoc_ok", g->w_xoc_ok);
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
    lp_printf(b, "# M7.3: FaceYuv records + sample, ownership refresh (layer.*), histogram overlay (ovh.*)\n");
    lp_printf(b, "# M7.4: ML LV image source (fyl.*), ML overlays / global draw (gd.*)\n");
    lp_printf(b, "# M7.5: lazy conversion (layer.lazy_*, blk_same, blk_keep, full_forced), ML histogram from Canon's bins (gd.hcanon_*, gd.hist_hold, mlh.*), subsampled waveform (gd.wfm_*)\n");

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
    /* M7.3: buffer indices and the consumer request records (frame -> buffer) */
    if (fctx && ram_ok(fctx, LP_FY_CTX_SIZE))
    {
        if (rd8(fctx + LP_FY_GET_IDX, &v)) lp_printf(b, "fy.get_idx=%d\n", (int) v);
        if (rd8(fctx + LP_FY_REC_IDX, &v)) lp_printf(b, "fy.rec_idx=%d\n", (int) v);
        for (int i = 0; i < 4; i++)
        {
            uint32_t rb = fctx + LP_FY_REC + LP_FY_REC_STRIDE * i;
            snprintf(k, sizeof(k), "fy.rec[%d].w0", i);           lp_printf(b, "%s=0x%08x\n", k, (int) RD(rb));
            snprintf(k, sizeof(k), "fy.rec[%d].frame", i);        lp_printf(b, "%s=%d\n", k, (int) RD(rb + 4));
            snprintf(k, sizeof(k), "fy.rec[%d].w0c", i);          lp_printf(b, "%s=0x%08x\n", k, (int) RD(rb + 0xC));
            snprintf(k, sizeof(k), "fy.rec[%d].ibus", i);         lp_fld64(b, k, rb, 0x10);
            snprintf(k, sizeof(k), "fy.rec[%d].w24", i);          lp_printf(b, "%s=0x%08x\n", k, (int) RD(rb + 0x24));
        }
    }
    {
        struct lp_fys fs;
        lp_fy_sample(&fs);
        lp_fy_log(b, "fy.sample", &fs);
        if (lp_fy_last.t_ms) lp_fy_log(b, "fy.oneshot", &lp_fy_last);
        else lp_printf(b, "fy.oneshot=none\n");
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

#ifdef CONFIG_R6_LAYER_OWNERSHIP
    {
        const struct r6_layer_stats * s = r6_layer_stats();
#define LL(name, val) lp_printf(b, "layer." name "=%d\n", (int) (val))
        LL("table_bytes", s->table_bytes);
        LL("alloc_failed", s->alloc_failed);
        LL("conversions", s->conversions);
        LL("clears", s->clears);
        LL("resets", s->resets);
        LL("slow_lut", s->slow_lut);
        LL("blk_skip", s->blk_skip);
        LL("blk_conv", s->blk_conv);
        LL("blk_owned", s->blk_owned);
        LL("px_owned", s->px_owned);
        LL("px_cleared", s->px_cleared);
        LL("veto_blk", s->veto_blk);
        LL("veto_px", s->veto_px);
        LL("clear_px", s->clear_px);
        LL("clear_veto_blk", s->clear_veto_blk);
        LL("clear_veto_px", s->clear_veto_px);
        LL("us_last", s->us_last);
        LL("us_max", s->us_max);
        LL("us_min", s->conversions ? s->us_min : 0);
        LL("us_avg", s->conversions ? (uint32_t) (s->us_sum / s->conversions) : 0);
        LL("clear_us_last", s->clear_us_last);
        LL("clear_us_max", s->clear_us_max);
        LL("defer_blk", s->defer_blk);
        LL("defer_px", s->defer_px);
        LL("defer_convs", s->defer_convs);
        LL("under_convs", s->under_convs);
        LL("under_blocked_last", s->under_blocked_last);
        LL("under_blocked_px", s->under_blocked_px);
        LL("under_drop_blk", s->under_drop_blk);
        LL("under_drop_px", s->under_drop_px);
        LL("under_val_n", s->under_val_n);
        LL("lazy_convs", s->lazy_convs);
        LL("full_convs", s->full_convs);
        LL("blk_same", s->blk_same);
        LL("blk_same_total", s->blk_same_total);
        LL("blk_keep", s->blk_keep);
        LL("blk_keep_total", s->blk_keep_total);
        LL("us_lazy_last", s->us_lazy_last);
        LL("us_lazy_max", s->us_lazy_max);
        LL("us_full_last", s->us_full_last);
        LL("us_full_max", s->us_full_max);
        LL("full_forced", s->full_forced);
        LL("lazy_streak_max", s->lazy_streak_max);
        LL("defer_forced", s->defer_forced);
        LL("defer_age_max", s->defer_age_max);
        LL("race_px", s->race_px);
        for (int i = 0; i < 4; i++)
        {
            snprintf(k, sizeof(k), "layer.under_val[%d]", i);       lp_printf(b, "%s=0x%08x\n", k, (int) s->under_val[i]);
        }
        LL("bad_target", s->bad_target);
        LL("bad_w", s->bad_w);
        LL("bad_h", s->bad_h);
        lp_printf(b, "layer.bad_data=0x%08x\n", (int) s->bad_data);
#undef LL
    }
#else
    lp_printf(b, "layer=off\n");
#endif

#ifdef FEATURE_R6_CANON_HISTOGRAM
    {
        const struct r6_overlay_stats * o = r6_overlay_stats();
#define LO(name, val) lp_printf(b, "ovh." name "=%d\n", (int) (val))
        LO("mode", o->mode);
        char nbo[40];
        lp_printf(b, "ovh.state=%s\n", lp_nosp(r6_overlay_reason(o->state), nbo, sizeof(nbo)));
        LO("box", o->box);
        LO("ticks", o->ticks);
        LO("draws", o->draws);
        LO("erases", o->erases);
        LO("torn", o->torn);
        LO("same_frame", o->same_frame);
        LO("same_bins", o->same_bins);
        LO("forced", o->forced);
        LO("rate", o->rate);
        for (int i = 0; i < R6H_NREASONS; i++)
        {
            snprintf(k, sizeof(k), "ovh.ticks[%d]", i);           lp_printf(b, "%s=%d\n", k, (int) o->reasons[i]);
        }
        LO("set", o->set);
        LO("numpix", o->numpix);
        LO("frame", o->frame);
        LO("peak", o->peak);
        for (int i = 0; i < 4; i++)
        {
            snprintf(k, sizeof(k), "ovh.clip[%d]", i);            lp_printf(b, "%s=%d\n", k, (int) o->clip[i]);
        }
        LO("t_draw", o->t_draw);
        LO("draw_us_last", o->draw_us_last);
        LO("draw_us_max", o->draw_us_max);
#undef LO
    }
#else
    lp_printf(b, "ovh=off\n");
#endif

    /* M7.4: ML's LV image source (r6_faceyuv.c) and ML's overlays (r6_gdraw.c) */
    {
        const struct r6_fy_stats * f = r6_fy_stats();
        lp_printf(b, "fyl.calls=%d\n", (int) f->calls);
        for (int i = 0; i < R6FY_NREASONS; i++)
        {
            snprintf(k, sizeof(k), "fyl.reason[%d]", i);          lp_printf(b, "%s=%d\n", k, (int) f->reasons[i]);
        }
        lp_printf(b, "fyl.last=%d\n", (int) f->last);
        lp_printf(b, "fyl.last_why=%s\n", f->last_why ? f->last_why : "");
        lp_printf(b, "fyl.cpu=0x%08x\n", (int) f->cpu);
        lp_printf(b, "fyl.frame=%d\n", (int) f->frame);
        lp_printf(b, "fyl.switches=%d\n", (int) f->switches);
    }
#ifdef CONFIG_R6_GLOBAL_DRAW
    {
        const struct r6_gd_stats * g = &r6_gd_st;
        extern int get_global_draw_setting(void);
#define LGD(name, val) lp_printf(b, "gd." name "=%d\n", (int) (val))
        LGD("setting", get_global_draw_setting());
        LGD("checks", g->checks);
        for (int i = 0; i < R6GD_NREASONS; i++)
        {
            snprintf(k, sizeof(k), "gd.reason[%d]", i);           lp_printf(b, "%s=%d\n", k, (int) g->reasons[i]);
        }
        char nbg[40];
        lp_printf(b, "gd.last=%s\n", lp_nosp(r6_gd_reason(g->last), nbg, sizeof(nbg)));
        lp_printf(b, "gd.last_outside_menu=%s\n", lp_nosp(r6_gd_reason(g->last_outside_menu), nbg, sizeof(nbg)));
        lp_printf(b, "gd.mask=0x%08x\n", (int) g->mask);
        LGD("ovl", g->ovl);
        LGD("zebra_n", g->zebra_n);        LGD("zebra_changed", g->zebra_changed);
        LGD("zebra_us", g->zebra_us);      LGD("zebra_us_max", g->zebra_us_max);   LGD("zebra_nolv", g->zebra_nolv);
        LGD("fc_n", g->fc_n);              LGD("fc_us", g->fc_us);                 LGD("fc_us_max", g->fc_us_max);
        LGD("fc_nolv", g->fc_nolv);
        LGD("hist_n", g->hist_n);          LGD("hist_us", g->hist_us);             LGD("hist_us_max", g->hist_us_max);
        LGD("hist_nolv", g->hist_nolv);
        LGD("hdraw_n", g->hdraw_n);        LGD("wdraw_n", g->wdraw_n);
        LGD("draw_us", g->draw_us);        LGD("draw_us_max", g->draw_us_max);
        LGD("spot_n", g->spot_n);          LGD("spot_us", g->spot_us);             LGD("spot_us_max", g->spot_us_max);
        LGD("erase_n", g->erase_n);        LGD("refresh_req", g->refresh_req);     LGD("drawn", g->drawn);
        LGD("reshow", g->reshow);
        LGD("refresh_same", g->refresh_same);
        LGD("spot_same", g->spot_same);
        LGD("spot_y", g->spot_y);          LGD("spot_u", g->spot_u);               LGD("spot_v", g->spot_v);
        LGD("spot_nu", g->spot_nu);        LGD("spot_nv", g->spot_nv);
        LGD("cap_hold", g->cap_hold);      LGD("cap_ms", g->cap_ms);               LGD("resets_seen", g->resets_seen);
        LGD("movie_sm", g->movie_sm);      LGD("movie_lvms", g->movie_lvms);
        lp_printf(b, "gd.shooting_mode=0x%x\n", (int) g->shooting_mode);
        LGD("lv_movie_select", g->lv_movie_select);
        LGD("hcanon_n", g->hcanon_n);      LGD("hcanon_us", g->hcanon_us);         LGD("hcanon_us_max", g->hcanon_us_max);
        LGD("hcanon_ok", g->hcanon_ok);    LGD("hcanon_fail", g->hcanon_fail);
        lp_printf(b, "gd.hcanon_state=%s\n", lp_nosp(r6_overlay_reason(g->hcanon_state), nbg, sizeof(nbg)));
        LGD("hist_valid", g->hist_valid);  LGD("hist_none", g->hist_none);       LGD("hist_hold", g->hist_hold);
        LGD("wfm_n", g->wfm_n);            LGD("wfm_us", g->wfm_us);               LGD("wfm_us_max", g->wfm_us_max);
        LGD("wfm_samples", g->wfm_samples);
        /* M7.6 */
        LGD("evf", r6_gd_evf);             LGD("evf_ok", g->evf_ok);               LGD("evf_switch", g->evf_switch);
        LGD("peak_n", g->peak_n);          LGD("peak_us", g->peak_us);             LGD("peak_us_max", g->peak_us_max);
        LGD("peak_nolv", g->peak_nolv);    LGD("peak_dots", g->peak_dots);         LGD("peak_over", g->peak_over);
        LGD("peak_thr", g->peak_thr);      LGD("peak_changed", g->peak_changed);   LGD("peak_full", g->peak_full);
        LGD("peak_off", g->peak_off);      LGD("redraw_n", g->redraw_n);
        /* M7.7 */
        lp_printf(b, "gd.evf_slot=0x%x\n", (int) g->evf_slot);
        LGD("evf_bw", g->evf_bw);          LGD("evf_bh", g->evf_bh);
        LGD("evf_x", g->evf_x);            LGD("evf_y", g->evf_y);
        LGD("evf_vw", g->evf_vw);          LGD("evf_vh", g->evf_vh);
#undef LGD
    }
#endif
#ifdef CONFIG_R6_GRID
    {
        /* M9.1: grids & masks (r6_grid.c) */
        const struct r6_grid_stats * r = r6_grid_stats();
#define LGR(name, val) lp_printf(b, "grid." name "=%d\n", (int) (val))
        lp_printf(b, "grid.cfg=0x%08x\n", (int) r->cfg);
        lp_printf(b, "grid.sig=0x%08x\n", (int) r->sig);
        LGR("wanted", r6_grid_wanted_grid()); LGR("drawn", r->drawn);        LGR("evf", r->evf);   /* M9.3: the grid alone */
        LGR("y0", r->y0);                  LGR("px_line", r->px_line);       LGR("px_fill", r->px_fill);
        LGR("px_shadow", r->px_shadow);    LGR("steps", r->steps);           LGR("off", r->off);
        LGR("draws", r->draws);            LGR("erases", r->erases);         LGR("repairs", r->repairs);
        LGR("repair_px", r->repair_px);    LGR("claim_ovl", r->claim_ovl);   LGR("cleanup_px", r->cleanup_px);
        LGR("skip_foreign", r->skip_foreign); LGR("skip_mark", r->skip_mark); LGR("excl", r->excl);
        LGR("us_last", r->us_last);        LGR("us_max", r->us_max);         LGR("us_draw_max", r->us_draw_max);
        LGR("y0_mismatch", r->y0_mismatch); LGR("changed", r->changed);
#undef LGR
    }
#endif
#if defined(CONFIG_R6_GLOBAL_DRAW) && defined(FEATURE_HISTOGRAM)
    {
        const struct r6_mlhist_stats * m = r6_mlhist_stats();
#define LMH(name, val) lp_printf(b, "mlh." name "=%d\n", (int) (val))
        LMH("calls", m->calls);            LMH("ok", m->ok);                       LMH("torn", m->torn);
        LMH("noset", m->noset);
        for (int i = 0; i < R6H_NREASONS; i++)
        {
            snprintf(k, sizeof(k), "mlh.reason[%d]", i);          lp_printf(b, "%s=%d\n", k, (int) m->reasons[i]);
        }
        char nbm[40];
        lp_printf(b, "mlh.last=%s\n", lp_nosp(r6_overlay_reason(m->last), nbm, sizeof(nbm)));
        LMH("set", m->set);                LMH("numpix", m->numpix);               LMH("frame", m->frame);
        LMH("us_last", m->us_last);        LMH("us_max", m->us_max);
#undef LMH
    }
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
#ifdef CONFIG_R6_AUTOSTART
    lp_printf(b, "autostart=%d\n", lp_autostart);
#endif
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
    lp_printf(b, "shooting_mode=0x%x\n", shooting_mode);       /* M7.4: movie gate evidence */
    lp_printf(b, "lv_movie_select=%d\n", lv_movie_select);
    lp_printf(b, "is_movie_mode=%d\n", (int) is_movie_mode());
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
    /* M7.7: + [7],[8] crop x/y, [9],[0xa] output X/Y, [0xb],[0xc] visible W/H (builder 0xE02995A8) */
    static const uint8_t dii[] = { 0, 1, 2, 3, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0x19, 0x1a, 0x1d };
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

    /* M7.6: EVF geometry. The WINSYS origin table, and what layer 0 shows in
     * origin 1 (720x540 at (152,0), assumed to be the EVF) per 30-row band:
     * every 4th pixel of every 2nd row (2700 samples per band), split into
     * Canon's and ML's pixels (ML = owned in the ownership table; a stale
     * entry while a conversion runs only shifts the split). RAM reads only. */
    for (int i = 0; i < 4; i++)
    {
        uint32_t a = LP_WINSYS_ORIGIN + 16 * i, v[4];
        snprintf(k, sizeof(k), "winsys.origin[%d]", i);
        if (rd32(a, &v[0]) && rd32(a + 4, &v[1]) && rd32(a + 8, &v[2]) && rd32(a + 12, &v[3]))
            lp_printf(b, "%s=%dx%d@(%d,%d)\n", k, (int) v[2], (int) v[3], (int) v[0], (int) v[1]);
        else
            lp_printf(b, "%s=invalid\n", k);
    }
    {
        uint32_t base = 0, lw = 0, lh = 0;
        if (fld(m, 4, &base) && fld(m, 0x1C, &lw) && fld(m, 0x20, &lh) && lw == BMP_LAYER_WIDTH && lh >= 540
            && ram_ok(base, BMP_LAYER_WIDTH * 4 * 540))
        {
            const uint32_t * L = (const uint32_t *) base;
            for (int band = 0; band < 18; band++)
            {
                int canon = 0, ml = 0;
                for (int y = band * 30; y < band * 30 + 30; y += 2)
                {
                    for (int x = 152; x < 872; x += 4)
                    {
                        uint32_t c = L[y * BMP_LAYER_WIDTH + x];
                        if (!(c & 0xFF000000) || c == 0xFF000000) continue;   /* empty, as r6_layer.c R6L_EMPTY */
                        if (r6_layer_owns(x, y)) ml++; else canon++;
                    }
                }
                snprintf(k, sizeof(k), "evfprof[%d]", band);
                lp_printf(b, "%s=%d/%d\n", k, canon, ml);
            }
        }
        else
            lp_printf(b, "evfprof=invalid\n");
    }

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

#ifdef CONFIG_R6_INTERVAL
/* M8.0 / M8.1: interval release state (r6_interval.c), ML's values it checks, the
 * Canon RAM words it reads, and the last pictures (oldest first). M8.1 adds the
 * CameraConductor, the switch manager, the release counter, the job manager, the
 * FA-Neo node walk (only with a live job), the last-picture detail and the
 * passive trace. M8.2 adds the long-Tv fields (CC copies of NR / interval timer /
 * the 0x80040064 timer setting, the CC flags 0xB98 / 0x830 / 0x5F8, the snapshot's validity
 * mask, the Tv budget, the ML menu guard count, per-shot Tv / budget / timing).
 * M9.2 adds the bulb fields: Canon's bulb timer word [p+0x648], [cc+0xD30] /
 * [cc+0xBA0] / [cc+0x99C], the timer RAM 0x1E568 / 0x1E5BC..0x1E5C8 (iv.bulb.*,
 * 9 more fields per iv.last.* snapshot), the bulb limits / warning bits / overrun
 * latch, and per shot the bulb class, end, button, warnings, drive, [cc+0x5E8],
 * T, seconds, timer words, Canon's measured bulb time and the clock glitches.
 * Every pointer taken from a Canon structure passes ram_ok() for the whole
 * structure first (p: cached RAM or the verified uncached alias); otherwise the
 * key prints invalid(0x...) and nothing is dereferenced (also for 0: no NULL keys
 * in this block). */

/* 32-bit field [base+off] of a Canon structure of n bytes (whole structure range-checked) */
static void lp_ivw(struct lp_buf * b, const char * key, uint32_t base, uint32_t n, uint32_t off)
{
    if (!ram_ok(base, n)) { lp_bad(b, key, base); return; }
    lp_printf(b, "%s=0x%08x\n", key, (int) RD(base + off));
}

/* byte [base+off] of a Canon structure of n bytes, through its aligned word */
static void lp_ivb(struct lp_buf * b, const char * key, uint32_t base, uint32_t n, uint32_t off)
{
    if (!ram_ok(base, n)) { lp_bad(b, key, base); return; }
    lp_printf(b, "%s=%d\n", key, (int) ((RD(base + (off & ~3u)) >> (8 * (off & 3))) & 0xFF));
}

/* [[base+poff]+off]: the outer structure (n1 bytes) and the inner one (n2 bytes) both checked */
static void lp_ivw2(struct lp_buf * b, const char * key, uint32_t base, uint32_t n1, uint32_t poff, uint32_t n2, uint32_t off)
{
    if (!ram_ok(base, n1)) { lp_bad(b, key, base); return; }
    lp_ivw(b, key, RD(base + poff), n2, off);
}

/* byte [[base+poff]+off]: the outer structure (n1 bytes) and the inner one (n2 bytes) both checked (M8.2) */
static void lp_ivb2(struct lp_buf * b, const char * key, uint32_t base, uint32_t n1, uint32_t poff, uint32_t n2, uint32_t off)
{
    if (!ram_ok(base, n1)) { lp_bad(b, key, base); return; }
    lp_ivb(b, key, RD(base + poff), n2, off);
}

/* one r6_iv_cc snapshot on one line (M8.2: 29 fields; M9.2: 38; M10.2: 44) */
static void lp_iv_cc(struct lp_buf * b, const char * key, const struct r6_iv_cc * s)
{
    lp_printf(b, "%s=%d,%d,%d,%d,%d,%d,0x%x,0x%x,%d,0x%x,0x%x,%d,%d,%d,%d,%d,%d,%d,%d,%d,0x%x,0x%x,%d,%d,%d,%d,%d,%d,%d"
                 ",%d,%d,%d,0x%x,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", key,
        (int) s->state, (int) s->state_prev, (int) s->relseq, (int) s->b54, s->df, s->e0, (int) s->pend, (int) s->work,
        (int) s->shmode, (int) s->a68, (int) s->a6c, (int) s->r888, (int) s->relcnt, (int) s->jobs, (int) s->jid,
        (int) s->jdone, (int) s->access,
        s->faneo[0] == R6IV_RAM_FANEO && s->faneo[1] == R6IV_RAM_FANEO, s->tsc, s->fn, s->gui,
        (int) s->p_tv, (int) s->p_92c, (int) s->p_lenr, (int) s->p_itimer, (int) s->p_fbrk, (int) s->b98, (int) s->c830,
        (int) s->r5f8,
        (int) s->d30, (int) s->ba0, (int) s->sec, (int) s->bulbw, (int) s->ta, (int) s->ta_ms, (int) s->tb, (int) s->tb_ms,
        (int) s->bulb_ms,
        (int) s->p_hdr, (int) s->p_me, (int) s->p_wbb, (int) s->p_fbkt, (int) s->p_msnr, (int) s->p_shtype);   /* M10.2 */
}

/* FA-Neo node walk (approved Option 4, only with a live job: a jobref of a finished job points to
 * freed memory). 0 = complete, else the failed step (raw = the value that failed) */
struct lp_fa { uint32_t stop, raw, node, jobref, jobdata, mask, id, sm, save, tbl, cnt, pos, stage; };
static int lp_fa_walk(struct lp_fa * f)
{
    memset(f, 0, sizeof(*f));
    uint32_t link = RD(R6IV_RAM_FANEO_TAIL);
    uint32_t node = link - 0x14;
    f->raw = link;
    if (!ram_ok(node, 0x20) || RD(link) != R6IV_RAM_FANEO) return f->stop = 1;      /* oldest.next == head */
    f->node = node;
    uint32_t jobref = RD(node + 0x1C);
    f->raw = jobref;
    if (!ram_ok(jobref, 0x18) || RD(jobref) != R6IV_JOBCLASS_SIG) return f->stop = 2;
    f->jobref = jobref;
    uint32_t jobdata = RD(jobref + 4);
    f->raw = jobdata;
    uint32_t d = jobref - jobdata;                                                    /* unit slot 0 / 1 / 2 */
    if (d != 0 && d != 0x138 && d != 0x270) return f->stop = 3;
    if (!ram_ok(jobdata, 0x3070) || RD(jobdata) != R6IV_JOBCLASS_SIG || RD(jobdata + 4) != jobdata) return f->stop = 4;
    f->jobdata = jobdata;
    f->mask = RD(jobdata + 0x3DC);              /* unit / flag mask: bit0 card, bit1 PC, bit3 factory */
    f->id = RD(jobdata + 0x3C0);                /* job ID */
    f->sm = RD(jobdata + 0x41C);                /* ShutterMode copy of this job */
    f->save = RD(jobdata + 0xD5C);              /* save mode at creation */
    f->tbl = RD(jobdata + 0x3B4);               /* stage table (ROM, default 0xE0CC331C) */
    f->cnt = RD(jobdata + 0x3BC);               /* stage count (default 0x11) */
    f->pos = RD(jobref + 0xC);                  /* stage position */
    f->raw = f->tbl;
    if (f->tbl < 0xE0000000 || f->tbl >= 0xE2040000 || (f->tbl & 3)) return f->stop = 5;
    f->raw = f->cnt;
    if (f->cnt < 1 || f->cnt > 32) return f->stop = 6;
    f->raw = f->pos;
    if (f->pos >= f->cnt || f->tbl + 4 * f->pos + 4 > 0xE2040000) return f->stop = 7;
    f->stage = RD(f->tbl + 4 * f->pos);         /* ROM word */
    return 0;
}

static void lp_iv_faneo(struct lp_buf * b)
{
    uint32_t tail = RD(R6IV_RAM_FANEO_TAIL), live = RD(R6IV_RAM_JOBCNT);
    if (tail == R6IV_RAM_FANEO) { lp_printf(b, "iv.job.faneo=empty\n"); return; }
    if (!live) { lp_printf(b, "iv.job.faneo=not_walked(live_0)\n"); return; }
    struct lp_fa f, f2;
    lp_fa_walk(&f);
    lp_fa_walk(&f2);
    lp_printf(b, "iv.job.faneo=walked\n");
    if (f.node) lp_printf(b, "iv.job.fa.node=0x%08x\n", (int) f.node);
    if (f.jobref) lp_printf(b, "iv.job.fa.jobref=0x%08x\n", (int) f.jobref);
    if (f.jobdata)
    {
        lp_printf(b, "iv.job.fa.jobdata=0x%08x\n", (int) f.jobdata);
        lp_printf(b, "iv.job.fa.unit_off=0x%x\n", (int) (f.jobref - f.jobdata));
        lp_printf(b, "iv.job.fa.mask=0x%08x\n", (int) f.mask);
        lp_printf(b, "iv.job.fa.id=%d\n", (int) f.id);
        lp_printf(b, "iv.job.fa.sm=%d\n", (int) f.sm);
        lp_printf(b, "iv.job.fa.save=0x%08x\n", (int) f.save);
        lp_printf(b, "iv.job.fa.tbl=0x%08x\n", (int) f.tbl);
        lp_printf(b, "iv.job.fa.cnt=%d\n", (int) f.cnt);
        lp_printf(b, "iv.job.fa.pos=%d\n", (int) f.pos);
    }
    if (f.stop) lp_printf(b, "iv.job.fa.stop=%d,0x%08x\n", (int) f.stop, (int) f.raw);
    else lp_printf(b, "iv.job.fa.stage=0x%08x\n", (int) f.stage);
    lp_printf(b, "iv.job.fa.stable=%d\n", memcmp(&f, &f2, sizeof(f)) == 0);
}

#define LP_IV_SHOTS_LOG 12        /* M9.2 review: picture-ring entries in the log (~170..220 B each). M10.2 (spec 6.3): 32 ->
                                   * 24. M10.2b: 12, and printed last in the ring tail (lp_ring_tail): camera LVP00101 was
                                   * 96,621 of 98,176 B, the M10.2 budget had undercounted (spec 6.3) */
#define LP_PW_LOG       16        /* M10.2b: the newest write-ring entries in the log (of R6PW_RING 32; ~130..190 B each) */
#define LP_TAIL_SLACK   512       /* M10.2b review (S2, L1): the ring tail fills the buffer down to this; it covers a picture
                                   * line (<= 224 B) and a write line (<= 187 B) that land between measuring and printing.
                                   * The 4 KB rule is a static budget of everything before the tail (spec 6.3) */
static const char lp_shot_hdr[] = "# iv.shot[i]=t,ret,dur,d_cnt,d_fn,pic_ms,job_ms,jobs_after,js_before,js_after,wait_ms,wait_mask,"
                 "sw1_ms,acc_ms,off_ms,seq_ms,off_why,off_state,states,d_rel,d_jid,sm_before,sm_after,late,second,final,"
                 "tv,tv_cc,busy,s5_ms,exp_ms,slot_late,r92c,nr,cflag,"
                 "bulb,bend,bbtn,bwarn,drive,r5e8,bt,sec_max,bulbw,bulbw_after,ta_ms,bulb_ms,d30_ms,clk_back,"
                 "bk,bn,want,ev,wr\n";

/* one picture-ring line (entry idx since boot) into t, as printed before M10.2b; returns its length (with the newline) */
static int lp_shot_line(char * t, int n, const struct r6_iv_stats * s, uint32_t idx)
{
    const struct r6_iv_shot * e = &s->ring[idx % R6IV_RING];
    snprintf(t, n, "iv.shot[%d]=%d,%d,%d,%d,%d,%d,%d,%d,0x%x,0x%x,%d,0x%x,%d,%d,%d,%d,%d,%d,0x%x,%d,%d,%d,%d,0x%x,%d,0x%x,"
                 "%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                 "%d,%d,0x%x,0x%x,%d,%d,%d,%d,0x%x,0x%x,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        (int) idx,
        (int) e->t, (int) e->ret, (int) e->dur, (int) e->d_cnt, (int) e->d_fn, (int) e->pic_ms,
        (int) e->job_ms, (int) e->jobs_after, (int) e->js_before, (int) e->js_after,
        (int) e->wait_ms, (int) e->wait_mask,
        e->sw1_ms, e->acc_ms, e->off_ms, (int) e->seq_ms, e->off_why, e->off_state, e->states, e->d_rel, e->d_jid,
        e->sm_before, e->sm_after, e->late, e->second, e->final,
        e->tv, e->tv_cc, (int) e->busy, (int) e->s5_ms, (int) e->exp_ms, (int) e->slot_late, e->r92c, e->nr, e->cflag,
        e->bulb, e->bend, e->bbtn, e->bwarn, e->drive, e->r5e8, e->bt, e->sec_max, (int) e->bulbw, (int) e->bulbw_after,
        (int) e->ta_ms, (int) e->bulb_ms, (int) e->d30_ms, e->clk_back,
        e->bk, e->bn, e->want, e->ev, e->wr);                                         /* M10.2 */
    return strlen(t);
}

static void __attribute__((noinline)) lp_iv_log(struct lp_buf * b)   /* M10.2b: out of line as before (canon_gate layout) */
{
    const struct r6_iv_stats * s = r6_iv_stats();
    char w[40];
    int hard = 0;
    int k = r6_iv_canon_check(&hard);
    int c = r6_iv_cam_check();
    lp_printf(b, "iv.running=%d\n", s->running);
    lp_printf(b, "iv.n_done=%d\n", s->n_done);
    lp_printf(b, "iv.n_target=%d\n", s->n_target);
    lp_printf(b, "iv.interval_s=%d\n", s->interval_s);
    lp_printf(b, "iv.t_start=%d\n", (int) s->t_start);
    lp_printf(b, "iv.stop=%d,%s\n", s->stop, lp_nosp(r6_iv_stop_name(s->stop), w, sizeof(w)));
    lp_printf(b, "iv.stop_cam=%d,%s\n", s->stop_cam, lp_nosp(r6_iv_cam_name(s->stop_cam), w, sizeof(w)));
    lp_printf(b, "iv.stop_canon=%d,%s\n", s->stop_canon, lp_nosp(r6_iv_canon_name(s->stop_canon), w, sizeof(w)));
    lp_printf(b, "iv.stop_val=%d\n", (int) s->stop_val);
    lp_printf(b, "iv.t_stop=%d\n", (int) s->t_stop);
    lp_printf(b, "iv.skipped=%d\n", s->skipped);
    lp_printf(b, "iv.calls=%d\n", (int) s->calls);
    lp_printf(b, "iv.shots=%d\n", (int) s->shots);
    lp_printf(b, "iv.refused=%d\n", (int) s->refused);
    {
        struct r6_iv_ref r;                                              /* M10.2b: the newest Start / Test refusals */
        char wn[48];
        int nref = r6_iv_refusal(0, &r);
        lp_printf(b, "iv.ref.n=%d\n", (int) r6_iv_refusal_n());
        lp_printf(b, "# iv.ref[i]=t,src(1 Start,2 Test,3/4 the task,5/6 Motion),cam,lock,bk,tv,name (oldest first)\n");
        for (int i = 0; i < nref; i++)
        {
            r6_iv_refusal(i, &r);
            lp_nosp(r.cam == R6IV_CAM_TVW ? _r6pw_lock_name(r.lock) : r6_iv_cam_name(r.cam), wn, sizeof(wn));
            for (char * c = wn; *c; c++) if (*c == ',') *c = ';';     /* one field */
            lp_printf(b, "iv.ref[%d]=%d,%d,%d,%d,0x%x,0x%04x,%s\n", i, (int) r.t, r.src, r.cam, r.lock, r.bk, r.tv & 0xFFFF, wn);
        }
    }
    lp_printf(b, "iv.test=%d\n", s->test);
    lp_printf(b, "iv.cnt_last=%d\n", s->cnt_last);
    lp_printf(b, "iv.fn_last=%d\n", s->fn_last);
    uint32_t pn[4], refused = 0;
    int held = r6_iv_press_info(pn, &refused);
    lp_printf(b, "iv.press.n=%d,%d,%d,%d\n", (int) pn[0], (int) pn[1], (int) pn[2], (int) pn[3]);
    lp_printf(b, "iv.press.refused=%d\n", (int) refused);
    lp_printf(b, "iv.press.held=%d\n", held);
    lp_printf(b, "iv.gate.cc_settings=%d\n", r6_iv_gate_cc_settings());
    lp_printf(b, "iv.tv_min=%d\n", r6_iv_tv_min());
    lp_printf(b, "iv.menu_blocked=%d\n", (int) s->menu_blocked);
    {
        int tv_ub = 0, busy = 0, need = 0, need_nr = 0, fit = 0;
        r6_iv_budget(&tv_ub, &busy, &need, &need_nr, &fit);
        lp_printf(b, "iv.tv_ub_ms=%d\n", tv_ub);
        lp_printf(b, "iv.busy_ms=%d\n", busy);
        lp_printf(b, "iv.need_ms=%d\n", need);
        lp_printf(b, "iv.need_nr_ms=%d\n", need_nr);
        lp_printf(b, "iv.interval_fit=%d\n", fit);
    }
    lp_printf(b, "iv.bulb.min_s=%d\n", r6_iv_bulb_min_s());           /* M9.2 */
    lp_printf(b, "iv.bulb.max_s=%d\n", r6_iv_bulb_max_s());
    lp_printf(b, "iv.bulb.warn=0x%x\n", (int) s->bulb_warn);
    lp_printf(b, "iv.bulb.running=%d\n", r6_iv_bulb_running());
    lp_printf(b, "iv.mode=%d\n", s->mode);                            /* M9.3: 0 interval, 1 motion release, 2 motion view */
    {
        const struct r6_iv_bkt * bkp = r6_iv_bkt();                      /* M10.2: the run's bracket */
        char t[80];
        int j = 0;
        t[0] = 0;
        lp_printf(b, "iv.bkt=%d,%d,%d,%d,%d,%d,%d\n", bkp->mode, bkp->n, bkp->step, bkp->order, _r6pw_ev_on(), bkp->k, bkp->ev);
        lp_printf(b, "iv.bkt.base=0x%04x\n", bkp->base & 0xFFFF);
        for (int i = 0; i < bkp->n && i < R6IV_BKT_MAXN; i++) j += snprintf(t + j, sizeof(t) - j, i ? ",0x%04x" : "0x%04x", bkp->list[i] & 0xFFFF);
        lp_printf(b, "iv.bkt.list=%s\n", t[0] ? t : "-");
        lp_printf(b, "iv.bkt.need_ms=%d\n", bkp->need);
        lp_printf(b, "iv.bkt.need_nr_ms=%d\n", bkp->need_nr);
        lp_printf(b, "iv.bkt.fit=%d\n", bkp->fit);
        lp_printf(b, "iv.bkt.writes=%d\n", bkp->writes);
    }
    lp_printf(b, "iv.md_refused=%d,%s\n", r6_iv_md_refused(), lp_nosp(r6_iv_cam_name(r6_iv_md_refused()), w, sizeof(w)));
    lp_printf(b, "iv.cam=%d,%s\n", c, lp_nosp(r6_iv_cam_name(c), w, sizeof(w)));
    lp_printf(b, "iv.canon=%d,%s\n", k, lp_nosp(r6_iv_canon_name(k), w, sizeof(w)));
    lp_printf(b, "iv.canon_hard=%d\n", hard);
    lp_printf(b, "iv.shooting_mode=%d\n", shooting_mode);
    lp_printf(b, "iv.drive_mode=0x%08x\n", drive_mode);
    lp_printf(b, "iv.af_mode=0x%08x\n", af_mode);
    lp_printf(b, "iv.raw_shutter=%d\n", lens_info.raw_shutter);
    int tv_seen = 0, tv_len = 0;
    int tv_prop = r6_iv_tv_prop(&tv_seen, &tv_len);
    lp_printf(b, "iv.tv_prop=%d\n", tv_prop);
    lp_printf(b, "iv.tv_seen=%d\n", tv_seen);
    lp_printf(b, "iv.tv_len=%d\n", tv_len);
    int tv_ev = 0;
    int tv_ok = r6_iv_tv_ev768(&tv_ev);
    lp_printf(b, "iv.tv_ev768=%d\n", tv_ok ? tv_ev : -99999);
    int aeb_seen = 0;
    int aeb = r6_iv_aeb(&aeb_seen);
    lp_printf(b, "iv.aeb=0x%08x\n", aeb);
    lp_printf(b, "iv.aeb_seen=%d\n", aeb_seen);
    lp_printf(b, "iv.avail_shot=%d\n", avail_shot);
    lp_printf(b, "iv.burst_count=%d\n", burst_count);
    lp_printf(b, "iv.icu_uilock=0x%08x\n", icu_uilock);
    lp_printf(b, "iv.sensor_cleaning=%d\n", sensor_cleaning);
    lp_printf(b, "iv.auto_power_off_time=%d\n", auto_power_off_time);
    lp_printf(b, "iv.total_shots_count=%d\n", total_shots_count);
    lp_printf(b, "iv.shutter_count=%d\n", shutter_count);
    struct card_info * card = get_shooting_card();
    lp_printf(b, "iv.file_number=%d\n", card ? card->file_number : -1);
    lp_printf(b, "iv.job_state=0x%08x\n", lens_info.job_state);
    lp_printf(b, "iv.gui_mode=0x%08x\n", CURRENT_GUI_MODE);
    lp_printf(b, "iv.gui_state=%d\n", gui_state);
    lp_glob(b, "iv.ram.jobcnt", R6IV_RAM_JOBCNT);
    lp_glob(b, "iv.ram.fasy_sem", R6IV_RAM_FASY_SEM);
    lp_glob(b, "iv.ram.fa_log", R6IV_RAM_FA_LOG);
    lp_glob(b, "iv.ram.fa_logmode", R6IV_RAM_FA_LOGMODE);
    lp_glob(b, "iv.ram.fa_logfile", R6IV_RAM_FA_LOGFILE);
    lp_glob(b, "iv.ram.fa_store", R6IV_RAM_FA_STORE);
    lp_globs(b, "iv.ram.faneo", R6IV_RAM_FANEO, 4);
    lp_glob(b, "iv.ram.faneo_sem", R6IV_RAM_FANEO_SEM);
    lp_glob(b, "iv.ram.cc_sw1", R6IV_RAM_CC_SW1);
    lp_glob(b, "iv.ram.cc_sw2", R6IV_RAM_CC_SW2);
    lp_glob(b, "iv.ram.cc_obj", R6IV_RAM_CC_OBJ);

    /* CameraConductor */
    static const uint32_t cps[9] = { R6IV_RAM_CC_MASTER, R6IV_RAM_CC_RELSEQ, R6IV_RAM_CC_STATEGET, R6IV_RAM_CC_SWWRAP,
        R6IV_RAM_CC_SHMODE, R6IV_RAM_CC_LVH1, R6IV_RAM_CC_LVH2, R6IV_RAM_CC_JSW2OFF, R6IV_RAM_CC_JSW1OFF };
    uint32_t cc = RD(R6IV_RAM_CC);
    lp_printf(b, "iv.cc=0x%08x\n", (int) cc);
    int ok_v = cc != 0, ok_i = cc != 0;
    for (int i = 0; i < 9; i++)
    {
        uint32_t v = RD(cps[i]);
        lp_printf(b, "iv.cc.copy[0x%x]=0x%08x\n", (int) cps[i], (int) v);
        if (v != cc) { if (i < 4) ok_v = 0; else ok_i = 0; }
    }
    lp_printf(b, "iv.cc.copy_ok=%d\n", ok_v);
    lp_printf(b, "iv.cc.copy_ok_i=%d\n", ok_i);
    {
        struct r6_iv_cc cs;                     /* M8.2: the validity mask of r6_interval.c's own read (camera: 0x1f) */
        r6_iv_cc_read(&cs);
        lp_printf(b, "iv.cc.v=0x%x\n", (int) cs.valid);
        /* M9.2: Canon's bulb timer (-1 = off / not readable), the bulb flags and the timer RAM, from the same read */
        lp_printf(b, "iv.bulb.t=%d\n", r6_iv_bulb_t(&cs));
        if (cs.valid & R6IV_V_P) lp_printf(b, "iv.bulb.word=0x%08x\n", (int) cs.bulbw);
        else lp_bad(b, "iv.bulb.word", (cs.valid & R6IV_V_CC) ? cs.p : cs.cc);
        if (cs.valid & R6IV_V_CC)
        {
            lp_printf(b, "iv.bulb.d30=%d\n", (int) cs.d30);
            lp_printf(b, "iv.bulb.ba0=%d\n", (int) cs.ba0);
            lp_printf(b, "iv.bulb.sec=%d\n", (int) cs.sec);
        }
        else
        {
            lp_bad(b, "iv.bulb.d30", cs.cc);
            lp_bad(b, "iv.bulb.ba0", cs.cc);
            lp_bad(b, "iv.bulb.sec", cs.cc);
        }
        lp_printf(b, "iv.bulb.ta=%d\n", (int) cs.ta);
        lp_printf(b, "iv.bulb.ta_ms=%d\n", (int) cs.ta_ms);
        lp_printf(b, "iv.bulb.tb=%d\n", (int) cs.tb);
        lp_printf(b, "iv.bulb.tb_ms=%d\n", (int) cs.tb_ms);
        lp_printf(b, "iv.bulb.ms=%d\n", (int) cs.bulb_ms);
    }
    lp_ivw(b, "iv.cc.evflag", cc, R6IV_CC_SIZE, R6IV_CC_EVFLAG);
    lp_ivw(b, "iv.cc.table", cc, R6IV_CC_SIZE, R6IV_CC_TABLE);
    lp_ivb(b, "iv.cc.lvkind", cc, R6IV_CC_SIZE, 0x2CE);
    lp_ivw(b, "iv.cc.shoottype", cc, R6IV_CC_SIZE, R6IV_CC_SHOOTTYPE);
    lp_ivw(b, "iv.cc.dispatch", cc, R6IV_CC_SIZE, R6IV_CC_DISPATCH);
    lp_ivw(b, "iv.cc.swgate", cc, R6IV_CC_SIZE, R6IV_CC_SWGATE);
    lp_ivw(b, "iv.cc.remote", cc, R6IV_CC_SIZE, R6IV_CC_REMOTE);
    lp_ivw(b, "iv.cc.state", cc, R6IV_CC_SIZE, R6IV_CC_STATE);
    lp_ivw(b, "iv.cc.state_prev", cc, R6IV_CC_SIZE, R6IV_CC_STATE_PREV);
    lp_ivb(b, "iv.cc.sw1", cc, R6IV_CC_SIZE, 0xDF);
    lp_ivb(b, "iv.cc.sw2", cc, R6IV_CC_SIZE, 0xE0);
    lp_ivb(b, "iv.cc.snap_sw1", cc, R6IV_CC_SIZE, 0x16B);
    lp_ivb(b, "iv.cc.snap_sw2", cc, R6IV_CC_SIZE, 0x16C);
    lp_ivw(b, "iv.cc.pend2", cc, R6IV_CC_SIZE, R6IV_CC_PEND2);
    lp_ivw(b, "iv.cc.work2", cc, R6IV_CC_SIZE, R6IV_CC_WORK2);
    lp_glob(b, "iv.ram.aux2", R6IV_RAM_AUX2);
    lp_ivw(b, "iv.cc.b54", cc, R6IV_CC_SIZE, R6IV_CC_B54);
    lp_glob(b, "iv.ram.relseq", R6IV_RAM_RELSEQ);
    lp_glob(b, "iv.ram.relcb", R6IV_RAM_RELCB);
    lp_ivw(b, "iv.cc.shmode", cc, R6IV_CC_SIZE, R6IV_CC_SHMODE);
    lp_ivw(b, "iv.cc.a68", cc, R6IV_CC_SIZE, R6IV_CC_A68);
    lp_ivw(b, "iv.cc.a6c", cc, R6IV_CC_SIZE, R6IV_CC_A6C);
    lp_ivw(b, "iv.cc.r888", cc, R6IV_CC_SIZE, R6IV_CC_R888);
    lp_ivw(b, "iv.cc.bfc", cc, R6IV_CC_SIZE, R6IV_CC_BFC);
    lp_ivw(b, "iv.cc.r5e8", cc, R6IV_CC_SIZE, R6IV_CC_R5E8);
    lp_ivw(b, "iv.cc.af", cc, R6IV_CC_SIZE, R6IV_CC_AF);
    lp_ivw(b, "iv.cc.r730", cc, R6IV_CC_SIZE, R6IV_CC_R730);
    lp_ivw(b, "iv.cc.r3b4", cc, R6IV_CC_SIZE, R6IV_CC_R3B4);
    lp_ivw(b, "iv.cc.b98", cc, R6IV_CC_SIZE, R6IV_CC_B98);
    lp_ivw(b, "iv.cc.c830", cc, R6IV_CC_SIZE, R6IV_CC_C830);
    lp_ivw(b, "iv.cc.r5f8", cc, R6IV_CC_SIZE, R6IV_CC_R5F8);
    lp_ivw(b, "iv.cc.p", cc, R6IV_CC_SIZE, R6IV_CC_P);
    lp_ivw2(b, "iv.cc.p.ae", cc, R6IV_CC_SIZE, R6IV_CC_P, R6IV_P_SIZE, R6IV_P_AE);
    lp_ivw2(b, "iv.cc.p.drive", cc, R6IV_CC_SIZE, R6IV_CC_P, R6IV_P_SIZE, R6IV_P_DRIVE);
    lp_ivw2(b, "iv.cc.p.r92c", cc, R6IV_CC_SIZE, R6IV_CC_P, R6IV_P_SIZE, R6IV_P_92C);
    {
        uint32_t p = ram_ok(cc, R6IV_CC_SIZE) ? RD(cc + R6IV_CC_P) : 0;
        if (ram_ok(p, R6IV_P_SIZE)) lp_printf(b, "iv.cc.p.tv=0x%04x\n", (int) (RD(p + R6IV_P_TV_W) >> 16));
        else lp_bad(b, "iv.cc.p.tv", ram_ok(cc, R6IV_CC_SIZE) ? p : cc);
    }
    lp_ivw2(b, "iv.cc.p.lenr", cc, R6IV_CC_SIZE, R6IV_CC_P, R6IV_P_SIZE, R6IV_P_LENR);
    lp_ivb2(b, "iv.cc.p.itimer", cc, R6IV_CC_SIZE, R6IV_CC_P, R6IV_P_SIZE, R6IV_P_ITIMER_W + 2);   /* byte 0x50A */
    lp_ivb2(b, "iv.cc.p.fbrk", cc, R6IV_CC_SIZE, R6IV_CC_P, R6IV_P_SIZE, R6IV_P_FBRK_W);          /* byte 0x634 */
    lp_glob(b, "iv.ram.swhack_q", R6IV_RAM_SWHACK_Q);
    lp_glob(b, "iv.ram.swhack_hook", R6IV_RAM_SWHACK_HOOK);

    /* switch manager */
    uint32_t mgr = RD(R6IV_RAM_SWMGR);
    lp_printf(b, "iv.sw.mgr=0x%08x\n", (int) mgr);
    lp_ivw2(b, "iv.sw.cbr[0]", mgr, R6IV_SWMGR_SIZE, 0, 8, 0);
    lp_ivw2(b, "iv.sw.cbr[1]", mgr, R6IV_SWMGR_SIZE, 0, 8, 4);
    lp_ivw2(b, "iv.sw.status[0]", mgr, R6IV_SWMGR_SIZE, 4, 8, 0);
    lp_ivw2(b, "iv.sw.status[1]", mgr, R6IV_SWMGR_SIZE, 4, 8, 4);
    lp_ivw(b, "iv.sw.hook", mgr, R6IV_SWMGR_SIZE, 0xC);

    /* release counter */
    uint32_t po = RD(R6IV_RAM_PO);
    lp_printf(b, "iv.po=0x%08x\n", (int) po);
    lp_ivw(b, "iv.po.relcnt", po, R6IV_PO_SIZE, R6IV_PO_RELCNT);
    uint32_t po2 = RD(R6IV_RAM_PO2);
    lp_ivw(b, "iv.po.relcnt_copy", po2, 0x1C0, 0x1BC);
    if (ram_ok(po, R6IV_PO_SIZE)) lp_printf(b, "iv.po.rel_minus_prop=%d\n", (int) (RD(po + R6IV_PO_RELCNT) - total_shots_count));
    else lp_bad(b, "iv.po.rel_minus_prop", po);

    /* job manager (fixed globals) and the FA-Neo list */
    lp_glob(b, "iv.job.last_id", R6IV_RAM_JOB_LASTID);
    lp_glob(b, "iv.job.done_id", R6IV_RAM_JOB_DONEID);
    lp_glob(b, "iv.job.skel_id", 0x9E0C);
    lp_glob(b, "iv.job.live", R6IV_RAM_JOBCNT);
    lp_glob(b, "iv.job.capture", 0x9DC4);
    lp_glob(b, "iv.job.develop", 0x9DC8);
    lp_glob(b, "iv.job.card", 0x9DCC);
    lp_glob(b, "iv.job.pc", 0x9DD0);
    lp_glob(b, "iv.job.access_remain", R6IV_RAM_JOB_ACCESS);
    lp_glob(b, "iv.job.size", 0x9DD8);
    lp_glob(b, "iv.job.state_m", 0x9DEC);
    lp_glob(b, "iv.job.state_s", 0x9DF0);
    lp_glob(b, "iv.job.class_m", 0x9DA0);
    lp_glob(b, "iv.job.class_s", 0x9DA4);
    lp_iv_faneo(b);

    /* the picture ring: M10.2b in the ring tail (lp_ring_tail), after everything else */

    /* last picture: snapshots and trace (codes: r6_interval.c iv_tr_codes, 1..4 the switch calls, X the reason) */
    const struct r6_iv_last * l = r6_iv_last();
    lp_printf(b, "# iv.last.*=state,state_prev,relseq,b54,df,e0,pend,work,shmode,a68,a6c,r888,relcnt,jobs,jid,jdone,"
                 "access,faneo_empty,tsc,fn,gui,p_tv,p_92c,p_lenr,p_itimer,p_fbrk,b98,c830,r5f8,"
                 "d30,ba0,sec,bulbw,ta,ta_ms,tb,tb_ms,bulb_ms,hdr,me,wbb,fbkt,msnr,shtype\n");
    lp_iv_cc(b, "iv.last.pre", &l->pre);
    lp_iv_cc(b, "iv.last.off", &l->off);
    lp_iv_cc(b, "iv.last.post", &l->post);
    lp_printf(b, "iv.last.valid=0x%x,0x%x,0x%x\n", (int) l->pre.valid, (int) l->off.valid, (int) l->post.valid);
    lp_printf(b, "iv.last.n=%d\n", l->n);
    lp_printf(b, "iv.last.lost=%d\n", l->lost);
    char cs[2] = { 0, 0 };                     /* the code as a string: Canon's vsnprintf garbles %c */
    for (int i = 0; i < l->n && i < R6IV_TRACE; i++)
    {
        cs[0] = l->tr[i].code ? l->tr[i].code : '?';
        lp_printf(b, "iv.tr[%d]=%d,%s,%d\n", i, (int) l->tr[i].dt, cs, (int) l->tr[i].v);
    }

    /* passive trace (Canon's own shots, no ML picture running): absolute values */
    struct r6_iv_ptr pe;
    int pn_cnt = r6_iv_ptrace(0, &pe);
    lp_printf(b, "iv.pt.n=%d\n", pn_cnt);
    for (int i = 0; i < pn_cnt; i++)
    {
        r6_iv_ptrace(i, &pe);
        cs[0] = pe.code ? pe.code : '?';
        lp_printf(b, "iv.pt[%d]=%d,%s,%d\n", i, (int) pe.t, cs, (int) pe.v);
    }
}
#endif

/* ---- M9.1 Tor 0: exposure values, read only (docs/m9_plan.md 1.4) --------
 * Debug > LV probe > "Expo log": own flag, OFF at boot, not saved; the
 * status line does not depend on it. Reads approved by the user (2026-10-04),
 * all plain RAM reads with the rules above (ram_ok: cached RAM, or the
 * verified uncached alias; every pointer checked; nothing written, no Canon
 * call):
 *  - CameraConductor cc = [0x10AB8] (as M8) and its settings sheets: p =
 *    [cc+0x34FC] is the master storage of CC's 417 properties (id table
 *    0xE1C3AABC, row table 0xE0FE3344, getter 0xE0713E4C), [cc+0x34FC+4k]
 *    k = 0..4; Canon's DataStore sheets [[0x10868]+0x2F4+4k] k = 0..10
 *    (sheet save 0xE07129CA)
 *  - the lens master buffers 0x127B88 (PROP_LENS_DYNAMIC_DATA, registered
 *    with 0x8C bytes at 0xE058A974) and 0x1279B8 (PROP_LENS_STATIC_DATA)
 *  - the LV ExactData ring: [0x132AC0 + 4 i], i = 0..19, records +0x48 Bv,
 *    +0x4A Tv, +0x4C Av, +0x50 Iso (s16; DTS_GetExactDataOffset 0xE079D188,
 *    "ExactData Bv:%x Tv:%x Av:%x Iso:%x" 0xE06A81C4), index dts[0x2C]
 *  - the GUI ISO copy [[0x16954]+0x1EC] (GUI_GetIso 0xE07A76C4)
 *  - the bulb fields of CC: state [cc+0x294], [cc+0xD30] (bulb running),
 *    [cc+0xBA0] (bulb timer armed), byte [cc+0x16B] (SW1 snapshot), the
 *    seconds tick [cc+0x99C], and Canon's timer RAM 0x1E568 (bulb ms),
 *    0x1E5BC / 0x1E5C0 / 0x1E5C4 / 0x1E5C8 (timers A / B: active, ms)
 *    (SHOOT_START 0xE031D7B4, EXPOSURE_START 0xE0643E08, m9/bulb)
 *  - Canon's Tv substitution flag (0xE035A7B4 returns
 *    [[[0x1085C+8]+0x644]+0x10]; with 1 CC_SetTv stores [p+0x25C] instead of
 *    the request, 0xE03B0FC0..0xE03B0FEC)
 * M10.1 Tor 0b (docs/m10_plan.md 2.6, approved 2026-10-05; same rules, read
 * only): the CC fields that gate or steer CC_SetTv ([cc+0x62C] > 1 stores
 * nothing 0xE0717C32, [cc+0x31C] == 1 stores in any AE mode 0xE0716F9A,
 * [cc+0x5E8] / [cc+0x2E0] / [cc+0x304] "normal context" 0xE0718160..0xE071819C,
 * [cc+0xB78] M memory +0x50 0xE03B13A6, backup slot 0x59 flag [cc+0x2A18] and
 * its Tv [cc+0x316E] 0xE0712FD0, [cc+0x724] clamp 0xE03B12A2), the sheet's
 * exposure step [p+0xD4] (2 = 1/3, 1 = 1/2, 0xE0641FD0), [p+0x5B8] (clamp,
 * 0x8002002E), [p+0x670] (flash firing, 0x80040013); Canon's M memory: the
 * record [[0x10868]+0x2E8] (PROPAD 0x02080002, DataStore init 0xE07119F6,
 * expected != ds+4) with Tv / Av at +0x40 / +0x42 / +0x50 / +0x52
 * (0xE034BB50, 0xE03B13CA) and the copy at ds+4; the substitution flags
 * 8..11 [[[0x10864]+0x5A4 / 0x644 / 0x6E4 / 0x784]+0x10] (getters 0xE035A7A8
 * .. 0xE035A7CC). Plus ML's own decoded lens values (expo.lensml, src/lens.c
 * CONFIG_R6_LENS_DATA) and CONTROL_BV (LVAE_STRUCT+0x28).
 * M10.1b Tor 0c (docs/m10_plan.md 2.11, read only under the same approval):
 * [cc+0xA84] (CC state 3 means "nothing active" only while it is 0: the AE
 * timer is armed only then 0xE070D6B2, screen exits 0xE0643FF8; set to 1
 * 0xE06DAF00), Canon's global [0x5184] (X-sync source of 0xE0A0B72C: 0 ->
 * 0x1800 = 1/250, 2 -> 0x1700 / 0x1680), the backup-slot flags of the checks
 * every Tv request runs, [cc+0x28B4+4*slot] (0xE0712FB0 / 0xE0713006; the
 * slot API's base [0x1086C] is the CC object, allocated 0xE0711E7E): step
 * block 0x28 [cc+0x2954] (restore 0xE0395B30), AEB 0xF [cc+0x28F0]
 * (0xE03B2C22, only while [cc+0xB74] == 0, 0xE03B2C12), compensation 0x2D
 * [cc+0x2968] (0xE03B2934), ALO 0x20 [cc+0x2934] (0xE03DAF52), flash 0x66
 * [cc+0x2A4C] (0xE03DD544), plus [cc+0xB74]; the sheet's byte [p+0x5F4]
 * (the input of 0xE0717AC4: ldrb 0xE0717AB0 / 0xE07177E8), [p+0xD8] (word 2
 * of the step block p+0xD4: 1 <-> 2 resets AEB, 0xE0395BB6 -> 0xE0395F4E)
 * and [p+0x100] (word 3 of the block p+0xF8: a change re-checks AEB,
 * 0xE0395D56). All in ring B and the snapshot. ML's remembered set aperture
 * (src/lens.c fix C) is the last field of expo.lensml.av.
 * Ring A (expo.pr): extra ML property handlers, only on ids this build
 * registers anyway (all in the baseline handler table with another
 * handler, none on the deny list, never PROP_ISO), so Canon's slave list
 * does not change. The handler runs in Canon's property task: it copies at
 * most 8 bytes plus id, length and time, nothing else (no lock, no print).
 * Ring B (expo.cb): every 200 ms (status task) the CC copies and bulb
 * fields; each changed field is one entry (changes < 200 ms apart merge).
 * ISO is only seen here (PROP_ISO stays denied).
 * Snapshot section expo.*: pointers, sheets, raw + decoded values, lens
 * buffers, the 20 ExactData records, the GUI ISO copy, handler value == CC
 * copy, both rings. */
#define LP_X_CC         0x10AB8    /* CameraConductor (set 0xE0295DCA; r6_interval.h R6IV_RAM_CC) */
#define LP_X_CC_SIZE    0x3510     /* fields up to the sheet pointers [cc+0x34FC..0x350F] */
#define LP_X_CC_P       0x34FC     /* p = sheet 0, current stills settings */
#define LP_X_P_SIZE     0xB9C      /* M10.2: fields up to [p+0xB98] (r6_interval.h R6IV_P_SIZE) */
#define LP_X_SHEET_SIZE 0x620      /* per sheet: AE 0x008 .. Tv 0x61E */
#define LP_X_DS_PTR     0x10868    /* Canon's DataStore: sheet pointers [ds+0x2F4+4k], k = 0..10 */
#define LP_X_DS_SIZE    0x320
#define LP_X_LENSDYN    0x127B88   /* PROP_LENS_DYNAMIC_DATA master buffer (registration 0xE058A974, 0x8C bytes) */
#define LP_X_LENSDYN_N  0x8C
#define LP_X_LENSSTAT   0x1279B8   /* PROP_LENS_STATIC_DATA master buffer (registration 0xE058A922, 0x1D0 bytes) */
#define LP_X_LENSSTAT_N 0x1D0      /* M10.1: all of it (M9.1 logged 0x1C8, ML's R5 size) */
#define LP_X_EXACT      0x132AC0   /* ExactData record pointers, 20 slots (DTS_GetExactDataOffset 0xE079D188) */
#define LP_X_EXACT_N    20
#define LP_X_EXACT_IDX  (LP_DTS + 0x2C)   /* dts[0x2C]: newest-slot base of DTS_GetExactDataOffset (which one is the
                                           * newest is open; all 20 are logged) */
#define LP_X_EXACT_SIZE 0x54       /* record fields read: +0x48 .. +0x53 */
#define LP_X_GUI_ISO    0x1EC      /* [[0x16954]+0x1EC] s16 (GUI_GetIso 0xE07A76C4) */
#define LP_X_TVS_PTR    0x10864    /* [0x1085C+8] (Tv substitution flag 0xE035A7B4: [[x+0x644]+0x10]) */
#define LP_X_TVS_SUB    0x25C      /* [p+0x25C] s16: the Tv CC_SetTv stores while the flag is 1 */
#define LP_X_TVS_SIZE   0x788      /* M10.1: the flag objects 8..11 at [x+0x5A4 / 0x644 / 0x6E4 / 0x784] */
#define LP_X_MM_OFF     0x2E8      /* M10.1: [ds+0x2E8] = Canon's M memory record (PROPAD 0x02080002) */
#define LP_X_MM_SIZE    0x54       /* fields read: +0x40 .. +0x53 */

static int lp_x_on = 0;                    /* menu "Expo log" (not saved) */
static volatile int lp_x_reset = 1;        /* the sampler takes a new baseline */

/* ---- ring A: the extra property handlers ---- */
static const uint32_t lp_xph_ids[] = {
    0x80000000, 0x80000001, 0x80000002, 0x80000003, 0x80000005, 0x80000006, 0x80000008, 0x80000009,
    0x8000000B, 0x80000034, 0x8000003A, 0x8000003B, 0x8000003D, 0x8000004A, 0x80020037, 0x8003009A,
};
#define LP_XPH_N   ((int) COUNT(lp_xph_ids))
enum { LP_XPH_DIAL, LP_XPH_AE, LP_XPH_METER, LP_XPH_DRIVE, LP_XPH_TV, LP_XPH_AV, LP_XPH_COMP, LP_XPH_FCOMP, LP_XPH_AEB,
       LP_XPH_EXPSIM, LP_XPH_AEMOVIE, LP_XPH_AUTOISO, LP_XPH_ALO, LP_XPH_HTP, LP_XPH_LENSDYN, LP_XPH_LENSSTAT };
#define LP_XPR_N   64            /* review: room for the mode-dial republishes; the lens ids stay out (lp_x_prop) */
struct lp_xpr { uint32_t t, id, len, w0, w1; };
static struct lp_xpr lp_xpr[LP_XPR_N];               /* written by the handler only (Canon's property task) */
static volatile uint32_t lp_xpr_n = 0;
static struct lp_xpr lp_xph_last[LP_XPH_N];          /* last delivery per id (also while the log is OFF) */
static volatile uint32_t lp_xph_cnt[LP_XPH_N];

static void lp_x_prop(unsigned property, void * priv, void * addr, unsigned len)
{
    int i = 0;
    while (i < LP_XPH_N && lp_xph_ids[i] != property) i++;
    if (i == LP_XPH_N) return;
    uint32_t w[2] = { 0, 0 };
    if (addr) memcpy(w, addr, MIN(len, 8u));           /* at most 8 bytes */
    struct lp_xpr e = { .t = get_ms_clock(), .id = property, .len = len, .w0 = w[0], .w1 = w[1] };
    lp_xph_last[i] = e;
    lp_xph_cnt[i]++;
    if (!lp_x_on || i >= LP_XPH_LENSDYN) return;     /* lens data (zoom / focus bursts): count + last only (expo.ph) */
    uint32_t n = lp_xpr_n;
    lp_xpr[n % LP_XPR_N] = e;
    lp_xpr_n = n + 1;
}

/* the same handler for each id (REGISTER_PROP_HANDLER with an own block name) */
#define LP_XPH(id) __attribute__((section(".prop_handlers"))) __attribute__((used)) \
    static struct prop_handler lp_xph_##id = { .handler = lp_x_prop, .property = id, .property_length = 0 }
LP_XPH(0x80000000);   /* shooting mode (dial) */
LP_XPH(0x80000001);   /* AE mode */
LP_XPH(0x80000002);   /* metering */
LP_XPH(0x80000003);   /* drive */
LP_XPH(0x80000005);   /* Tv, 2 bytes */
LP_XPH(0x80000006);   /* Av, 2 bytes */
LP_XPH(0x80000008);   /* exposure compensation (GUI sends 4) */
LP_XPH(0x80000009);   /* flash compensation */
LP_XPH(0x8000000B);   /* AEB */
LP_XPH(0x80000034);   /* exposure simulation */
LP_XPH(0x8000003A);   /* AE mode movie */
LP_XPH(0x8000003B);   /* auto ISO range, 0x14 bytes */
LP_XPH(0x8000003D);   /* ALO */
LP_XPH(0x8000004A);   /* HTP / D+ */
LP_XPH(0x80020037);   /* lens dynamic data, 0x8C bytes (length + 8 bytes only) */
LP_XPH(0x8003009A);   /* lens static data (length + 8 bytes only) */

/* ---- ring B: the CC copies and bulb fields, sampled every 200 ms ---- */
enum { LP_XB_CC, LP_XB_P, LP_XB_RAM, LP_XB_DS, LP_XB_MM, LP_XB_SUB,    /* M10.1: DataStore, M memory, flags 8..11 */
       LP_XB_FLB, LP_XB_CCG };                                          /* M10.2: flash bytes, CC object globals (computed) */
enum { LP_XD_HEX, LP_XD_DEC, LP_XD_TV, LP_XD_AV, LP_XD_ISO, LP_XD_EV, LP_XD_AE, LP_XD_ISOPAIR, LP_XD_TVPAIR, LP_XD_HMS };
struct lp_xf { const char * name; uint8_t base, size, dec, pad; uint32_t off; };
static const struct lp_xf lp_xf[] = {
    { "p",          LP_XB_CC,  4, LP_XD_HEX,     0, LP_X_CC_P },  /* sheet 0 pointer */
    { "dial",       LP_XB_CC,  4, LP_XD_DEC,     0, 0x89C },      /* 0x80000000 */
    { "ae",         LP_XB_P,   4, LP_XD_AE,      0, 0x008 },      /* 0x80000001 */
    { "tv",         LP_XB_P,   2, LP_XD_TV,      0, 0x61E },      /* 0x80000005, EV * 768 */
    { "av",         LP_XB_P,   2, LP_XD_AV,      0, 0x61C },      /* 0x80000006 */
    { "iso",        LP_XB_P,   4, LP_XD_ISO,     0, 0x510 },      /* 0x80000007, 0 = Auto */
    { "comp",       LP_XB_P,   2, LP_XD_EV,      0, 0x4A0 },      /* 0x80000008 */
    { "drive",      LP_XB_P,   4, LP_XD_HEX,     0, 0x4C0 },      /* 0x80000003 */
    { "aeb",        LP_XB_P,   4, LP_XD_EV,      0, 0x000 },      /* 0x8000000B, low s16 = step */
    { "autoiso0",   LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x094 },      /* 0x8000003B, 5 x {max, min} */
    { "autoiso1",   LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x098 },
    { "autoiso2",   LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x09C },
    { "autoiso3",   LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x0A0 },
    { "autoiso4",   LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x0A4 },
    { "isorange0",  LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x514 },      /* 0x80040033, 0x16 bytes: u16 {max, min}, u16 0, */
    { "isorange_pad", LP_XB_P, 2, LP_XD_HEX,     0, 0x518 },      /* 4 x u16 {max, min} (ROM1 default sheet */
    { "isorange1",  LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x51A },      /* 0xF0737920 + 0x514; review) */
    { "isorange2",  LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x51E },
    { "isorange3",  LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x522 },
    { "isorange4",  LP_XB_P,   4, LP_XD_ISOPAIR, 0, 0x526 },
    { "meter",      LP_XB_P,   4, LP_XD_HEX,     0, 0x584 },      /* 0x80000002 */
    { "expsim",     LP_XB_P,   4, LP_XD_HEX,     0, 0x548 },      /* 0x80000034 */
    { "htp",        LP_XB_P,   4, LP_XD_HEX,     0, 0x4FC },      /* 0x8000004A */
    { "lenr",       LP_XB_P,   4, LP_XD_HEX,     0, 0x54C },      /* 0x80000048 */
    { "tvrange",    LP_XB_P,   4, LP_XD_TVPAIR,  0, 0x114 },      /* Canon's cached dial limits: fastest 0x114, slowest 0x116 (K36) */
    { "bulbtimer",  LP_XB_P,   4, LP_XD_HMS,     0, 0x648 },      /* 0x8004005B {on, h, m, s} */
    { "itimer",     LP_XB_P,   4, LP_XD_HMS,     0, 0x50A },      /* 0x8004005A {on, h, m, s} */
    { "t634",       LP_XB_P,   4, LP_XD_HMS,     0, 0x634 },      /* 0x80040064: byte + h:m:s */
    { "state",      LP_XB_CC,  4, LP_XD_DEC,     0, 0x294 },      /* CC state (12 = bulb) */
    { "d30",        LP_XB_CC,  4, LP_XD_DEC,     0, 0xD30 },      /* bulb running */
    { "ba0",        LP_XB_CC,  4, LP_XD_DEC,     0, 0xBA0 },      /* bulb timer armed */
    { "snap_sw1",   LP_XB_CC,  1, LP_XD_DEC,     0, 0x16B },      /* SW1 byte as CC last saw it */
    { "sec",        LP_XB_CC,  4, LP_XD_DEC,     0, 0x99C },      /* bulb seconds: only 0 <-> non-0 logged, max kept */
    { "tvgate",     LP_XB_CC,  4, LP_XD_HEX,     0, 0x860 },      /* CC_SetTv stores nothing while 0 */
    { "bulb_ms",    LP_XB_RAM, 4, LP_XD_DEC,     0, 0x1E568 },
    { "ta",         LP_XB_RAM, 4, LP_XD_DEC,     0, 0x1E5BC },
    { "ta_ms",      LP_XB_RAM, 4, LP_XD_DEC,     0, 0x1E5C0 },
    { "tb",         LP_XB_RAM, 4, LP_XD_DEC,     0, 0x1E5C4 },
    { "tb_ms",      LP_XB_RAM, 4, LP_XD_DEC,     0, 0x1E5C8 },
    /* M10.1 Tor 0b (docs/m10_plan.md 2.6): what gates or steers CC_SetTv, the step setting, the M memory, flags */
    { "c62c",       LP_XB_CC,  4, LP_XD_DEC,     0, 0x62C },      /* > 1: CC_SetTv stores nothing (0xE0717C32) */
    { "c5e8",       LP_XB_CC,  4, LP_XD_DEC,     0, 0x5E8 },      /* "normal context" needs 0 (0xE0718160) */
    { "cb78",       LP_XB_CC,  4, LP_XD_DEC,     0, 0xB78 },      /* != 0: M memory at +0x50 (0xE03B13A6) */
    { "tvbak",      LP_XB_CC,  4, LP_XD_DEC,     0, 0x2A18 },     /* backup slot 0x59 flag (0xE0712FD0) */
    { "tvbak_val",  LP_XB_CC,  2, LP_XD_TV,      0, 0x316E },     /* its Tv (s16), written back into [p+0x61E] */
    { "c724",       LP_XB_CC,  4, LP_XD_DEC,     0, 0x724 },      /* 0 + 0xE0717422 == 1: only the slow end clamped */
    { "c2e0",       LP_XB_CC,  4, LP_XD_DEC,     0, 0x2E0 },      /* "normal context" needs 0 (0xE0718192) */
    { "c304",       LP_XB_CC,  4, LP_XD_DEC,     0, 0x304 },      /* "normal context" needs 0 (0xE071819C) */
    { "c31c",       LP_XB_CC,  4, LP_XD_DEC,     0, 0x31C },      /* 1: CC_SetTv stores in any AE mode (0xE0716F9A) */
    { "step",       LP_XB_P,   4, LP_XD_DEC,     0, 0x0D4 },      /* exposure step 2 = 1/3, 1 = 1/2 (0xE0641FD0) */
    { "tvclamp",    LP_XB_P,   4, LP_XD_HEX,     0, 0x5B8 },      /* 0x8002002E movie HDR (ATest_MovHDRG; key name kept, K6) */
    { "flash",      LP_XB_P,   4, LP_XD_HEX,     0, 0x670 },      /* 0x80040013 flash firing */
    { "mmem",       LP_XB_DS,  4, LP_XD_HEX,     0, LP_X_MM_OFF }, /* the M memory record pointer [ds+0x2E8] */
    { "mm_tv",      LP_XB_MM,  2, LP_XD_TV,      0, 0x040 },      /* M memory Tv / Av (dial -> M loads them, */
    { "mm_av",      LP_XB_MM,  2, LP_XD_AV,      0, 0x042 },      /* 0xE034BB50) */
    { "mm_tv50",    LP_XB_MM,  2, LP_XD_TV,      0, 0x050 },      /* the same while [cc+0xB78] != 0 */
    { "mm_av52",    LP_XB_MM,  2, LP_XD_AV,      0, 0x052 },
    { "subst8",     LP_XB_SUB, 4, LP_XD_DEC,     0, 0x5A4 },      /* substitution flags 8..11: [[[0x10864]+off]+0x10] */
    { "subst9",     LP_XB_SUB, 4, LP_XD_DEC,     0, 0x644 },      /* (9 = expo.tvsubst, getter 0xE035A7B4) */
    { "subst10",    LP_XB_SUB, 4, LP_XD_DEC,     0, 0x6E4 },
    { "subst11",    LP_XB_SUB, 4, LP_XD_DEC,     0, 0x784 },
    /* M10.1b Tor 0c (all 32-bit loads in the ROM excerpts except p5f4: ldrb) */
    { "ca84",       LP_XB_CC,  4, LP_XD_DEC,     0, 0xA84 },      /* != 0: state 3 without AE timer (0xE070D6B2, 0xE0643FF8) */
    { "xsync_src",  LP_XB_RAM, 4, LP_XD_DEC,     0, 0x5184 },     /* X-sync 0xE0A0B72C: 0 -> 1/250, 2 -> 1/200 (0xE0A0B732) */
    { "slot28",     LP_XB_CC,  4, LP_XD_DEC,     0, 0x2954 },     /* slot flags [cc+0x28B4+4*slot] (0xE0712FB0): step block */
    { "slot0f",     LP_XB_CC,  4, LP_XD_DEC,     0, 0x28F0 },     /* AEB (restored only while cb74 == 0, 0xE03B2C22) */
    { "slot2d",     LP_XB_CC,  4, LP_XD_DEC,     0, 0x2968 },     /* exposure compensation (restore 0xE03B2934) */
    { "slot20",     LP_XB_CC,  4, LP_XD_DEC,     0, 0x2934 },     /* ALO (restore 0xE03DAF52) */
    { "slot66",     LP_XB_CC,  4, LP_XD_DEC,     0, 0x2A4C },     /* flash (restore 0xE03DD544) */
    { "cb74",       LP_XB_CC,  4, LP_XD_DEC,     0, 0xB74 },      /* != 0: AEB slot 0xF not restored (0xE03B2C12) */
    { "p5f4",       LP_XB_P,   1, LP_XD_DEC,     0, 0x5F4 },      /* byte, input of 0xE0717AC4 (0xE0717AB0, 0xE07177E8) */
    { "pd8",        LP_XB_P,   4, LP_XD_DEC,     0, 0x0D8 },      /* step block word 2: 1 <-> 2 resets AEB (0xE0395BB6) */
    { "p100",       LP_XB_P,   4, LP_XD_HEX,     0, 0x100 },      /* block p+0xF8 word 3: change -> AEB check (0xE0395D56) */
    /* M10.2 Tor 0d (spec 6.2): the sheet keys (Tor 0 (a)) */
    { "hdr",        LP_XB_P,   4, LP_XD_HEX,     0, 0x4EC },      /* byte 0 HDRFunc (0x80000045) */
    { "me",         LP_XB_P,   4, LP_XD_HEX,     0, 0x5F4 },      /* byte 0 multiple exposure (p5f4 keeps the byte) */
    { "wbb_ba",     LP_XB_P,   4, LP_XD_HEX,     0, 0x654 },      /* WB bracketing B/A 0..3 */
    { "wbb_gm",     LP_XB_P,   4, LP_XD_HEX,     0, 0x658 },      /* WB bracketing G/M */
    { "fbkt",       LP_XB_P,   4, LP_XD_HEX,     0, 0x918 },      /* focus bracketing on */
    { "fbkt_n",     LP_XB_P,   4, LP_XD_DEC,     0, 0x91C },      /* focus bracketing shots */
    { "msnr",       LP_XB_P,   4, LP_XD_DEC,     0, 0x4F4 },      /* 4 = multi-shot NR */
    { "shtype",     LP_XB_P,   4, LP_XD_DEC,     0, 0xB98 },      /* shutter type, 3 = electronic (ROM-supported, K36) [I] */
    { "cfnr",       LP_XB_P,   4, LP_XD_TVPAIR,  0, 0x110 },      /* raw C.Fn Tv range fast / slow (K36); tvrange = Canon's cache */
    { "flicker",    LP_XB_P,   4, LP_XD_HEX,     0, 0x4E4 },      /* anti-flicker (log only) */
    { "selfcnt",    LP_XB_P,   2, LP_XD_HEX,     0, 0x4A8 },      /* continuous self-timer */
    { "alo",        LP_XB_P,   3, LP_XD_HEX,     0, 0x0A8 },      /* ALO effective / user / off in M (K17) */
    { "safesh",     LP_XB_P,   4, LP_XD_HEX,     0, 0x0DC },      /* CFNExpoSafeShift */
    { "safesh_m",   LP_XB_P,   4, LP_XD_HEX,     0, 0x0E0 },      /* CFNExpoSafeShift (Manual) */
    { "pf8",        LP_XB_P,   4, LP_XD_HEX,     0, 0x0F8 },      /* AEB branch input (R1) */
    /* M10.2 Tor 0d, D1 (Canon-RAM read approval; user answer 2026-10-06): CC backups, publish words, flash inputs, globals */
    { "aebbak",     LP_XB_CC,  4, LP_XD_HEX,     0, 0x2BFC },     /* AEB backup (slot 0x0F) */
    { "alobak",     LP_XB_CC,  3, LP_XD_HEX,     0, 0x2C54 },     /* ALO backup (slot 0x20) */
    { "compbak",    LP_XB_CC,  2, LP_XD_EV,      0, 0x3038 },     /* comp backup (slot 0x2D), s16 */
    { "gis",        LP_XB_CC,  4, LP_XD_HEX,     0, 0x93C },      /* GIS setting (0x8000004E) */
    { "fpub",       LP_XB_CC,  4, LP_XD_HEX,     0, 0xD24 },      /* comp publish / force bit 0x80 */
    { "fpub_lim",   LP_XB_CC,  4, LP_XD_HEX,     0, 0xD20 },
    { "c8c",        LP_XB_CC,  4, LP_XD_HEX,     0, 0xC8C },
    { "c60",        LP_XB_CC,  4, LP_XD_HEX,     0, 0xC60 },
    { "c3bc",       LP_XB_CC,  1, LP_XD_HEX,     0, 0x3BC },
    { "fl1",        LP_XB_CC,  4, LP_XD_HEX,     0, 0x7A8 },      /* flash predicate inputs (K37; row 43 uses fl1 / fl2) */
    { "fl2",        LP_XB_CC,  4, LP_XD_HEX,     0, 0xA30 },
    { "fl3",        LP_XB_CC,  4, LP_XD_HEX,     0, 0x728 },
    { "fl4",        LP_XB_CC,  4, LP_XD_HEX,     0, 0x848 },
    { "fl5",        LP_XB_FLB, 3, LP_XD_HEX,     0, 0 },          /* bytes [cc+0x794] | [cc+0xDB] << 8 | [cc+0xDC] << 16 */
    { "fl6",        LP_XB_CC,  4, LP_XD_HEX,     0, 0x6D0 },
    { "fl7",        LP_XB_CC,  4, LP_XD_HEX,     0, 0xBE8 },
    { "fl8",        LP_XB_CC,  4, LP_XD_HEX,     0, 0xB38 },
    { "ccg",        LP_XB_CCG, 1, LP_XD_HEX,     0, 0 },          /* bit 0 [0x1086C], 1 [0x2F800], 2 [0x10858], 3 [[0x10C28]+8] == cc,
                                                                    * 4 [0x10870] != 0 (K38) */
};
#define LP_XF_N    ((int) COUNT(lp_xf))
#define LP_XF_SEC  32                              /* index of "sec" */
#define LP_XCB_N   64
struct lp_xcb { uint32_t t; uint8_t f, ok_old, ok_new, pad; uint32_t old, val; };
static struct lp_xcb lp_xcb[LP_XCB_N];
static volatile uint32_t lp_xcb_n = 0;
static uint32_t lp_xv[LP_XF_N];
static uint8_t lp_xok[LP_XF_N];
static uint32_t lp_x_samples = 0, lp_x_bad_cc = 0, lp_x_bad_p = 0, lp_x_sec_max = 0, lp_x_t0 = 0;

/* cc and p if readable (bit 0 cc, bit 1 p) */
static int lp_x_ptrs(uint32_t * cc, uint32_t * p)
{
    *cc = RD(LP_X_CC); *p = 0;
    if (!ram_ok(*cc, LP_X_CC_SIZE)) return 0;
    *p = RD(*cc + LP_X_CC_P);
    return ram_ok(*p, LP_X_P_SIZE) ? 3 : 1;
}

/* size bytes at base+off from the aligned word(s), little endian; 0 = not readable */
static int lp_x_rd(uint32_t base, uint32_t off, int size, uint32_t * v)
{
    uint32_t a = base + off, w0, w1 = 0;
    int sh = 8 * (a & 3);
    if (!rd32(a & ~3u, &w0)) return 0;
    if (sh + 8 * size > 32 && !rd32((a & ~3u) + 4, &w1)) return 0;
    uint64_t q = ((uint64_t) w1 << 32) | w0;
    *v = (uint32_t) (q >> sh);
    if (size < 4) *v &= (1u << (8 * size)) - 1;
    return 1;
}

/* M10.1 Tor 0b: the base of a DataStore / M memory / flag field, every pointer checked (0 = not readable) */
static int lp_x_base2(int base, uint32_t off, uint32_t * a)
{
    if (base == LP_XB_SUB)                                 /* [[0x10864]+off] */
    {
        uint32_t t = RD(LP_X_TVS_PTR);
        if (!ram_ok(t, LP_X_TVS_SIZE)) return 0;
        *a = RD(t + off);
        return ram_ok(*a, 0x14);
    }
    uint32_t ds = RD(LP_X_DS_PTR);
    if (!ram_ok(ds, LP_X_DS_SIZE)) return 0;
    if (base == LP_XB_DS) { *a = ds; return 1; }
    *a = RD(ds + LP_X_MM_OFF);                             /* LP_XB_MM */
    return ram_ok(*a, LP_X_MM_SIZE);
}

/* M10.2 Tor 0d: the CC object globals (K38) as a bit mask; cc already checked */
static uint32_t lp_x_ccg(uint32_t cc)
{
    uint32_t api = RD(0x10C28);
    return (RD(0x1086C) == cc) | (RD(0x2F800) == cc) << 1 | (RD(0x10858) == cc) << 2
        | (ram_ok(api, 0xC) && RD(api + 8) == cc) << 3 | (RD(0x10870) != 0) << 4;
}

/* M10.2 Tor 0d: the flash predicate bytes [cc+0x794] | [cc+0xDB] << 8 | [cc+0xDC] << 16 */
static int lp_x_flb(uint32_t cc, uint32_t * v)
{
    uint32_t a, d, c;
    if (!lp_x_rd(cc, 0x794, 1, &a) || !lp_x_rd(cc, 0xDB, 1, &d) || !lp_x_rd(cc, 0xDC, 1, &c)) return 0;
    *v = a | d << 8 | c << 16;
    return 1;
}

static int lp_x_field(int i, uint32_t cc, uint32_t p, int vm, uint32_t * v)
{
    const struct lp_xf * f = &lp_xf[i];
    uint32_t a;
    if (f->base == LP_XB_RAM) return lp_x_rd(0, f->off, f->size, v);
    if (f->base == LP_XB_FLB) return (vm & 1) && lp_x_flb(cc, v);              /* M10.2 */
    if (f->base == LP_XB_CCG) { if (!(vm & 1)) return 0; *v = lp_x_ccg(cc); return 1; }
    if (f->base == LP_XB_CC) return (vm & 1) && lp_x_rd(cc, f->off, f->size, v);
    if (f->base == LP_XB_SUB) return lp_x_base2(f->base, f->off, &a) && lp_x_rd(a, 0x10, f->size, v);
    if (f->base == LP_XB_DS || f->base == LP_XB_MM) return lp_x_base2(f->base, 0, &a) && lp_x_rd(a, f->off, f->size, v);
    return (vm & 2) && lp_x_rd(p, f->off, f->size, v);
}

/* status task, every LP_STATUS_TICK_MS while the log is ON; RAM reads only */
static void lp_x_sample(void)
{
    lp_uc_update();
    uint32_t cc, p;
    int vm = lp_x_ptrs(&cc, &p), base = lp_x_reset;
    if (!(vm & 1)) lp_x_bad_cc++;
    else if (!(vm & 2)) lp_x_bad_p++;
    uint32_t now = get_ms_clock();
    if (base) { lp_xcb_n = 0; lp_x_sec_max = 0; lp_x_t0 = now; }
    for (int i = 0; i < LP_XF_N; i++)
    {
        uint32_t v = 0;
        int ok = lp_x_field(i, cc, p, vm, &v);
        if (ok && i == LP_XF_SEC && v > lp_x_sec_max) lp_x_sec_max = v;
        int diff = ok != lp_xok[i] || (ok && v != lp_xv[i]);
        if (i == LP_XF_SEC && ok && lp_xok[i]) diff = (v == 0) != (lp_xv[i] == 0);
        if (!base && diff)
        {
            uint32_t n = lp_xcb_n;
            struct lp_xcb * e = &lp_xcb[n % LP_XCB_N];
            e->t = now; e->f = i; e->ok_old = lp_xok[i]; e->ok_new = ok; e->old = lp_xv[i]; e->val = v;
            lp_xcb_n = n + 1;
        }
        if (diff || base || i == LP_XF_SEC) { lp_xv[i] = v; lp_xok[i] = ok; }
    }
    lp_x_samples++;
    lp_x_reset = 0;
}

/* ---- decoding (APEX * 768 = Canon's "Code768", 1/3 EV = 0x100) ---- */
static const char * const lp_x_tv_names[] = {      /* Tv thirds -15 (30 s) .. 42 (1/16000) */
    "30s", "25s", "20s", "15s", "13s", "10s", "8s", "6s", "5s", "4s", "3.2s", "2.5s", "2s", "1.6s", "1.3s", "1s",
    "0.8s", "0.6s", "0.5s", "0.4s", "0.3s", "1/4", "1/5", "1/6", "1/8", "1/10", "1/13", "1/15", "1/20", "1/25",
    "1/30", "1/40", "1/50", "1/60", "1/80", "1/100", "1/125", "1/160", "1/200", "1/250", "1/320", "1/400", "1/500",
    "1/640", "1/800", "1/1000", "1/1250", "1/1600", "1/2000", "1/2500", "1/3200", "1/4000", "1/5000", "1/6400",
    "1/8000", "1/10000", "1/12800", "1/16000",
};
static const char * const lp_x_av_names[] = {      /* Av thirds 0 (F1.0) .. 33 (F45) */
    "F1.0", "F1.1", "F1.2", "F1.4", "F1.6", "F1.8", "F2.0", "F2.2", "F2.5", "F2.8", "F3.2", "F3.5", "F4.0", "F4.5",
    "F5.0", "F5.6", "F6.3", "F7.1", "F8.0", "F9.0", "F10", "F11", "F13", "F14", "F16", "F18", "F20", "F22", "F25",
    "F29", "F32", "F36", "F40", "F45",
};
static const uint32_t lp_x_iso_vals[] = {          /* Sv thirds 12 (ISO 50) .. 48 (ISO 204800) */
    50, 64, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000,
    6400, 8000, 10000, 12800, 16000, 20000, 25600, 32000, 40000, 51200, 64000, 80000, 102400, 128000, 160000, 204800,
};

static void lp_x_ev(char * s, int n, int v)        /* EV with one decimal, sign: "+0.3", "-1.0" */
{
    int t = (ABS(v) * 10 + 384) / 768;
    snprintf(s, n, "%s%d.%d", v < 0 ? "-" : "+", t / 10, t % 10);
}

static void lp_x_iso(char * s, int n, uint32_t v)  /* low 16 bits: Sv * 768 */
{
    v &= 0xFFFF;
    if (!v) { snprintf(s, n, "Auto"); return; }
    if (!(v & 0xFF) && v / 256 >= 12 && v / 256 < 12 + COUNT(lp_x_iso_vals))
        snprintf(s, n, "ISO%d", (int) lp_x_iso_vals[v / 256 - 12]);
    else
        snprintf(s, n, "Sv%d.%02d", (int) (v / 768), (int) ((v % 768) * 100 / 768));
}

static void lp_x_tv(char * s, int n, int v)        /* s16 Tv * 768 */
{
    if (v == (int16_t) 0xDF00) { snprintf(s, n, "Bulb"); return; }
    if (v == (int16_t) 0xDC00) { snprintf(s, n, "special"); return; }
    int k = v / 256 + 15;
    if (!(v & 0xFF) && k >= 0 && k < (int) COUNT(lp_x_tv_names)) snprintf(s, n, "%s", lp_x_tv_names[k]);
    else { char e[12]; lp_x_ev(e, sizeof(e), v); snprintf(s, n, "Tv%s", e); }   /* Tv APEX, not a third step */
}

static void lp_x_av(char * s, int n, int v)        /* s16 Av * 768 */
{
    if (v == (int16_t) 0xDC00) { snprintf(s, n, "special"); return; }
    int k = v / 256;
    if (!(v & 0xFF) && k >= 0 && k < (int) COUNT(lp_x_av_names)) snprintf(s, n, "%s", lp_x_av_names[k]);
    else { char e[12]; lp_x_ev(e, sizeof(e), v); snprintf(s, n, "Av%s", e); }
}

static void lp_x_dec(char * s, int n, int dec, uint32_t v)
{
    static const char * const ae[] = { "P", "Tv", "Av", "M", "B" };
    char a[16], b[16];
    switch (dec)
    {
        case LP_XD_DEC:     snprintf(s, n, "%d", (int) v); break;
        case LP_XD_TV:      lp_x_tv(s, n, (int16_t) v); break;
        case LP_XD_AV:      lp_x_av(s, n, (int16_t) v); break;
        case LP_XD_ISO:     lp_x_iso(s, n, v); break;
        case LP_XD_EV:      lp_x_ev(s, n, (int16_t) v); break;
        case LP_XD_AE:      snprintf(s, n, "%s", v < COUNT(ae) ? ae[v] : v == 0x16 ? "A+" : v == 0x37 ? "Fv" : "?"); break;
        case LP_XD_ISOPAIR: if (v & 0xFFFF) lp_x_iso(a, sizeof(a), v); else snprintf(a, sizeof(a), "0");   /* "Auto": PROP_ISO only */
                            if (v >> 16) lp_x_iso(b, sizeof(b), v >> 16); else snprintf(b, sizeof(b), "0");
                            snprintf(s, n, "%s/%s", a, b); break;
        case LP_XD_TVPAIR:  lp_x_tv(a, sizeof(a), (int16_t) v); lp_x_tv(b, sizeof(b), (int16_t) (v >> 16));
                            snprintf(s, n, "%s..%s", a, b); break;
        case LP_XD_HMS:     snprintf(s, n, "%d/%d:%02d:%02d", (int) (v & 0xFF), (int) ((v >> 8) & 0xFF),   /* no comma */
                                     (int) ((v >> 16) & 0xFF), (int) (v >> 24)); break;
        default:            snprintf(s, n, "0x%x", (int) v); break;
    }
}

/* n bytes at a as hex, 64 bytes per line (key@0xOFF) */
static void lp_x_hex(struct lp_buf * b, const char * key, uint32_t a, uint32_t n)
{
    if (!ram_ok(a, (n + 3) & ~3u)) { lp_printf(b, "%s=invalid@0x%08x\n", key, (int) a); return; }
    for (uint32_t o = 0; o < n; o += 64)
    {
        char h[132];
        int k = 0;
        for (uint32_t i = o; i < n && i < o + 64; i++)
            k += snprintf(h + k, sizeof(h) - k, "%02x", (int) ((RD(a + (i & ~3u)) >> (8 * (i & 3))) & 0xFF));
        lp_printf(b, o < 0x100 ? "%s@0x%02x=%s\n" : "%s@0x%x=%s\n", key, (int) o, h);
    }
}

/* [base+off] s16 of a checked sheet */
static int lp_x_s16(uint32_t base, uint32_t off) { return (int16_t) (RD(base + (off & ~3u)) >> (8 * (off & 3))); }

static void lp_x_sheet(struct lp_buf * b, const char * key, uint32_t s)
{
    if (!ram_ok(s, LP_X_SHEET_SIZE)) { lp_bad(b, key, s); return; }
    lp_printf(b, "%s=0x%08x,%d,0x%04x,0x%04x,0x%08x,0x%04x\n", key, (int) s, (int) RD(s + 0x008),
        (int) (lp_x_s16(s, 0x61E) & 0xFFFF), (int) (lp_x_s16(s, 0x61C) & 0xFFFF), (int) RD(s + 0x510),
        (int) (lp_x_s16(s, 0x4A0) & 0xFFFF));
}

/* handler value (delivered bytes, at most 4) == CC copy: 1 / 0, "na" if one of them is missing */
static int lp_x_fi(const char * name)
{
    for (int i = 0; i < LP_XF_N; i++) if (!strcmp(lp_xf[i].name, name)) return i;
    return 0;
}

static void lp_x_match(struct lp_buf * b, const char * name, int i, int field, uint32_t cc, uint32_t p, int vm)
{
    uint32_t v = 0;
    if (!lp_xph_cnt[i] || !lp_x_field(field, cc, p, vm, &v)) { lp_printf(b, "expo.match.%s=na\n", name); return; }
    uint32_t n = MIN(lp_xph_last[i].len, 4u), size = lp_xf[field].size;
    uint32_t m = n >= 4 ? 0xFFFFFFFF : (1u << (8 * n)) - 1;
    if (size < 4) m &= (1u << (8 * size)) - 1;
    lp_printf(b, "expo.match.%s=%d,0x%x,0x%x\n", name, (lp_xph_last[i].w0 & m) == (v & m), (int) (lp_xph_last[i].w0 & m), (int) (v & m));
}

extern int r6_lens_av_set(void);   /* src/lens.c (CONFIG_R6_LENS_DATA, M10.1b fix C): Canon's set aperture, ML raw */

static void lp_x_log(struct lp_buf * b)
{
    char k[40], s1[24], s2[24];
    uint32_t cc, p, v;
    lp_uc_update();
    int vm = lp_x_ptrs(&cc, &p);
    uint32_t ds = RD(LP_X_DS_PTR), gp = RD(LP_GUI_PTR), ex_idx = RD(LP_X_EXACT_IDX);
    int dsok = ram_ok(ds, LP_X_DS_SIZE), gok = ram_ok(gp, LP_X_GUI_ISO + 4);
    lp_printf(b, "# M9.1 Tor 0: exposure values, read only; expo.cb = CC copy changes (200 ms), expo.pr = property deliveries\n");
    lp_printf(b, "expo.on=%d\n", lp_x_on);
    lp_printf(b, "expo.samples=%d\n", (int) lp_x_samples);
    lp_printf(b, "expo.t0=%d\n", (int) lp_x_t0);
    lp_printf(b, "expo.bad_cc=%d\n", (int) lp_x_bad_cc);
    lp_printf(b, "expo.bad_p=%d\n", (int) lp_x_bad_p);
    lp_printf(b, "expo.v=0x%x\n", vm | (dsok << 2) | ((ex_idx < LP_X_EXACT_N) << 3) | (gok << 4));
    lp_printf(b, "expo.cc=0x%08x\n", (int) cc);
    lp_printf(b, "expo.p=0x%08x\n", (int) p);
    lp_printf(b, "expo.p_region=%s\n", lp_region_name(p));
    if (vm & 1)
        for (int i = 0; i < 5; i++)
        {
            snprintf(k, sizeof(k), "expo.sheet[%d]", i);
            lp_x_sheet(b, k, RD(cc + LP_X_CC_P + 4 * i));
        }
    lp_printf(b, "expo.ds=0x%08x\n", (int) ds);
    if (dsok)
        for (int i = 0; i < 11; i++)
        {
            snprintf(k, sizeof(k), "expo.ds.sheet[%d]", i);
            lp_x_sheet(b, k, RD(ds + 0x2F4 + 4 * i));
        }
    /* current values, raw and decoded */
    for (int i = 1; i < LP_XF_N; i++)                     /* 0 = p: expo.p above */
    {
        snprintf(k, sizeof(k), "expo.%s", lp_xf[i].name);
        if (!lp_x_field(i, cc, p, vm, &v)) { lp_printf(b, "%s=na\n", k); continue; }
        lp_x_dec(s1, sizeof(s1), lp_xf[i].dec, v);
        lp_printf(b, "%s=0x%x,%s\n", k, (int) v, s1);
    }
    lp_printf(b, "expo.sec_max=%d\n", (int) lp_x_sec_max);
    /* Canon's Tv substitution flag and the value it substitutes */
    {
        uint32_t a = RD(LP_X_TVS_PTR), c = ram_ok(a, 0x648) ? RD(a + 0x644) : 0;
        if (ram_ok(a, 0x648) && ram_ok(c, 0x14)) lp_printf(b, "expo.tvsubst=%d\n", (int) RD(c + 0x10));
        else lp_printf(b, "expo.tvsubst=invalid(0x%08x,0x%08x)\n", (int) a, (int) c);
        if (vm & 2) { lp_x_tv(s1, sizeof(s1), lp_x_s16(p, LP_X_TVS_SUB)); lp_printf(b, "expo.tvsubst_val=0x%04x,%s\n", (int) (lp_x_s16(p, LP_X_TVS_SUB) & 0xFFFF), s1); }
    }
    if (gok) { lp_x_iso(s1, sizeof(s1), RD(gp + LP_X_GUI_ISO)); lp_printf(b, "expo.gui_iso=0x%04x,%s\n", (int) (RD(gp + LP_X_GUI_ISO) & 0xFFFF), s1); }
    else lp_bad(b, "expo.gui_iso", gp);
    /* M10.1 Tor 0b: the M memory record vs. its copy at ds+4 (memcpy 0xE07119FC; the record is expected elsewhere) */
    if (dsok)
    {
        lp_printf(b, "expo.mmem.is_ds4=%d\n", RD(ds + LP_X_MM_OFF) == ds + 4);
        lp_printf(b, "# expo.ds4.mm=tv,av,tv50,av52 of the copy at ds+4 (s16 Code768)\n");
        lp_printf(b, "expo.ds4.mm=0x%04x,0x%04x,0x%04x,0x%04x\n", (int) (lp_x_s16(ds, 4 + 0x40) & 0xFFFF),
            (int) (lp_x_s16(ds, 4 + 0x42) & 0xFFFF), (int) (lp_x_s16(ds, 4 + 0x50) & 0xFFFF), (int) (lp_x_s16(ds, 4 + 0x52) & 0xFFFF));
    }
    else lp_bad(b, "expo.mmem.is_ds4", ds);
    /* M10.1: ML's own decoded lens values (src/lens.c CONFIG_R6_LENS_DATA) and CONTROL_BV (lens.c uses PROP_APERTURE
     * only while it is 0) */
    {
        const struct r6_lens_dbg * d = &r6_lens_dbg;
        uint32_t cbv = 0;
        int cbv_ok = lp_x_rd(0, LVAE_STRUCT + 0x28, 2, &cbv);
        char nm[sizeof(lens_info.name)];
        int j = 0;
        for (; j < (int) sizeof(nm) - 1 && lens_info.name[j]; j++)
        {
            char c = lens_info.name[j];
            nm[j] = (c <= ' ' || c >= 0x7F || c == '=' || c == ',') ? '_' : c;
        }
        nm[j] = 0;
        lp_printf(b, "# expo.lensml=exists,lens_id,focal_mm,dist_cm,focus_pos,IS,mf,st2,st3,near_cm,far_cm,extender,ext_raw,fl_bad\n");
        lp_printf(b, "expo.lensml=%d,%d,%d,%d,%d,%d,%d,0x%02x,0x%02x,%d,%d,0x%02x,0x%02x,%d\n", (int) lens_info.lens_exists,
            (int) lens_info.lens_id, (int) lens_info.focal_len, (int) lens_info.focus_dist, (int) lens_info.focus_pos,
            (int) lens_info.IS, (d->st2 >> 7) & 1, (int) d->st2, (int) d->st3, (int) d->near, (int) d->far,
            (int) lens_info.lens_extender, (int) d->ext, (int) d->fl_bad);
        lp_printf(b, "# expo.lensml.av=raw,f_x10,raw_min,raw_max,control_bv,dof_near_mm,dof_far_mm,hyperfocal_mm,focal_min,focal_max,av_set\n");
        if (cbv_ok) snprintf(s1, sizeof(s1), "%d", (int) cbv); else snprintf(s1, sizeof(s1), "na");
        lp_printf(b, "expo.lensml.av=0x%02x,%d,0x%02x,0x%02x,%s,%d,%d,%d,%d,%d,0x%02x\n", (int) lens_info.raw_aperture,
            (int) lens_info.aperture, (int) lens_info.raw_aperture_min, (int) lens_info.raw_aperture_max, s1,
            (int) lens_info.dof_near, (int) lens_info.dof_far, (int) lens_info.hyperfocal, (int) lens_info.lens_focal_min,
            (int) lens_info.lens_focal_max, r6_lens_av_set());
        lp_printf(b, "# expo.lensml.n=dyn,dyn_bad,stat,stat_bad,av,av_bad,av_auto,av_auto_bad,av_last,av_auto_last\n");
        lp_printf(b, "expo.lensml.n=%d,%d,%d,%d,%d,%d,%d,%d,0x%04x,0x%04x\n", (int) d->dyn_n, (int) d->dyn_bad,
            (int) d->stat_n, (int) d->stat_bad, (int) d->av_n, (int) d->av_bad, (int) d->aa_n, (int) d->aa_bad,
            (int) d->av_last, (int) d->aa_last);
        lp_printf(b, "expo.lensml.name=%s\n", nm);
    }
    /* handler value == CC copy */
    lp_x_match(b, "dial", LP_XPH_DIAL, lp_x_fi("dial"), cc, p, vm);
    lp_x_match(b, "ae", LP_XPH_AE, lp_x_fi("ae"), cc, p, vm);
    lp_x_match(b, "tv", LP_XPH_TV, lp_x_fi("tv"), cc, p, vm);
    lp_x_match(b, "av", LP_XPH_AV, lp_x_fi("av"), cc, p, vm);
    lp_x_match(b, "comp", LP_XPH_COMP, lp_x_fi("comp"), cc, p, vm);
    lp_x_match(b, "drive", LP_XPH_DRIVE, lp_x_fi("drive"), cc, p, vm);
    lp_x_match(b, "aeb", LP_XPH_AEB, lp_x_fi("aeb"), cc, p, vm);
    lp_x_match(b, "autoiso", LP_XPH_AUTOISO, lp_x_fi("autoiso0"), cc, p, vm);
    lp_x_match(b, "meter", LP_XPH_METER, lp_x_fi("meter"), cc, p, vm);
    lp_x_match(b, "expsim", LP_XPH_EXPSIM, lp_x_fi("expsim"), cc, p, vm);
    lp_x_match(b, "htp", LP_XPH_HTP, lp_x_fi("htp"), cc, p, vm);
    /* lens buffers (Canon's master copies) */
    lp_printf(b, "expo.lensdyn.len=%d\n", lp_xph_cnt[LP_XPH_LENSDYN] ? (int) lp_xph_last[LP_XPH_LENSDYN].len : -1);
    lp_printf(b, "expo.lensstat.len=%d\n", lp_xph_cnt[LP_XPH_LENSSTAT] ? (int) lp_xph_last[LP_XPH_LENSSTAT].len : -1);
    lp_x_hex(b, "expo.lensdyn", LP_X_LENSDYN, LP_X_LENSDYN_N);
    lp_x_hex(b, "expo.lensstat", LP_X_LENSSTAT, LP_X_LENSSTAT_N);
    /* LV ExactData ring: 20 records Bv, Tv, Av, Iso */
    lp_printf(b, "expo.exact.idx=%d\n", (int) ex_idx);
    for (int i = 0; i < LP_X_EXACT_N; i++)
    {
        uint32_t r = RD(LP_X_EXACT + 4 * i);
        snprintf(k, sizeof(k), "expo.exact[%d]", i);
        if (!ram_ok(r, LP_X_EXACT_SIZE)) { lp_bad(b, k, r); continue; }
        lp_printf(b, "%s=0x%08x,0x%04x,0x%04x,0x%04x,0x%04x\n", k, (int) r, (int) (lp_x_s16(r, 0x48) & 0xFFFF),
            (int) (lp_x_s16(r, 0x4A) & 0xFFFF), (int) (lp_x_s16(r, 0x4C) & 0xFFFF), (int) (lp_x_s16(r, 0x50) & 0xFFFF));
    }
    /* deliveries per id (also while the log is OFF) */
    for (int i = 0; i < LP_XPH_N; i++)
    {
        snprintf(k, sizeof(k), "expo.ph[0x%08x]", (int) lp_xph_ids[i]);
        lp_printf(b, "%s=%d,%d,%d,0x%08x,0x%08x\n", k, (int) lp_xph_cnt[i], (int) lp_xph_last[i].t, (int) lp_xph_last[i].len,
            (int) lp_xph_last[i].w0, (int) lp_xph_last[i].w1);
    }
    /* ring A, oldest first */
    uint32_t n = lp_xpr_n;
    lp_printf(b, "expo.pr.n=%d\n", (int) n);
    lp_printf(b, "# expo.pr[i]=t,id,len,w0,w1\n");
    for (uint32_t i = (n > LP_XPR_N ? n - LP_XPR_N : 0); i < n; i++)
    {
        const struct lp_xpr * e = &lp_xpr[i % LP_XPR_N];
        lp_printf(b, "expo.pr[%d]=%d,0x%08x,%d,0x%08x,0x%08x\n", (int) i, (int) e->t, (int) e->id, (int) e->len, (int) e->w0, (int) e->w1);
    }
    /* ring B, oldest first */
    n = lp_xcb_n;
    lp_printf(b, "expo.cb.n=%d\n", (int) n);
    lp_printf(b, "# expo.cb[i]=t,field,old,new,old_decoded,new_decoded (na = not readable)\n");
    for (uint32_t i = (n > LP_XCB_N ? n - LP_XCB_N : 0); i < n; i++)
    {
        const struct lp_xcb * e = &lp_xcb[i % LP_XCB_N];
        int f = e->f < LP_XF_N ? e->f : 0;
        if (e->ok_old) lp_x_dec(s1, sizeof(s1), lp_xf[f].dec, e->old); else snprintf(s1, sizeof(s1), "na");
        if (e->ok_new) lp_x_dec(s2, sizeof(s2), lp_xf[f].dec, e->val); else snprintf(s2, sizeof(s2), "na");
        lp_printf(b, "expo.cb[%d]=%d,%s,0x%x,0x%x,%s,%s\n", (int) i, (int) e->t, lp_xf[f].name, (int) e->old, (int) e->val, s1, s2);
    }
}


#ifdef CONFIG_R6_PROPW_TV
/* M10.2 (spec 6.1): the Tv write path (r6_propw.c) and its write ring, Tv values 0x%04x */
static void lp_pw_log(struct lp_buf * b)
{
    const struct r6pw_state * s = _r6pw_state();
    char w[40];
    int now = get_ms_clock(), hn = 0, hlen = 0, ht = 0, lk = 0;
    int16_t hv = 0;
    uint32_t cc = 0, p = 0, v = 0, v2 = 0;
    lp_printf(b, "# M10.2 Tor 1: the Tv write path (r6_propw.c); pw.w = write ring, oldest first\n");
    lp_printf(b, "pw.on=%d\n", _r6pw_on());
    lp_printf(b, "pw.id=0x80000005\n");
    lp_printf(b, "pw.len=2\n");
    lp_printf(b, "pw.win=%d,%d\n", R6PW_TV_MIN, R6PW_TV_MAX);
    lp_printf(b, "pw.grid=%d\n", R6PW_GRID);
    lp_printf(b, "pw.ms=%d,%d,%d,%d,%d,%d,%d\n", R6PW_CONFIRM_MS, R6PW_POLL_MS, R6PW_SPACING_MS, R6PW_SETTLE_MS, R6PW_STABLE_MS,
        R6PW_HOLD_MS, R6PW_PEND_MS);
    lp_printf(b, "pw.armed=%d,0x%04x\n", s->armed, s->base & 0xFFFF);
    lp_printf(b, "pw.last=0x%04x,%d\n", s->last & 0xFFFF, (int) s->t_res);
    lp_printf(b, "pw.open=%d,0x%04x,%d\n", s->open, s->open_want & 0xFFFF, (int) s->open_t);
    lp_printf(b, "pw.dead=%d,%d\n", s->dead, s->dead_why);
    lp_printf(b, "pw.sig=%d\n", s->sig);
    lp_printf(b, "pw.pend=%d,%d,%d\n", s->pend, s->pend_why, s->pend ? now - (int) s->pend_t0 : 0);
    lp_printf(b, "pw.offer=%d,0x%04x,0x%04x\n", s->offer, s->base & 0xFFFF, s->boot_rec & 0xFFFF);
    lp_printf(b, "pw.boot=%d,%d\n", s->boot_done, s->mk_boot);
    lp_printf(b, "pw.marker=%d\n", s->marker);
    lp_printf(b, "pw.landed=%d,0x%04x,%d,%d,0x%04x,%d\n", s->landed_ae, s->landed_v & 0xFFFF, s->landed_ok,
        s->landed_ae2, s->landed_v2 & 0xFFFF, s->landed_ok2);          /* review round 2 (S2-1): + the older record */
    lp_printf(b, "pw.lim=%d\n", s->lim);
    lp_printf(b, "pw.calls=%d\n", (int) s->calls);
    lp_printf(b, "pw.uns=%d\n", s->uns);
    _r6pw_log(&hn, &hv, &hlen, &ht);
    lp_printf(b, "pw.h=%d,0x%04x,%d,%d\n", hn, hv & 0xFFFF, hlen, ht);
    lp_printf(b, "pw.sticky=%d,%d,%d,%d\n", s->fgn_m, s->rec_moved, s->ovf, (int) s->t_ev);
    lp_printf(b, "pw.watch=0x%04x,0x%04x,%d,%d\n", s->close_v & 0xFFFF, s->close_rec & 0xFFFF, s->w_other, s->w_recv);
    _r6pw_locks(R6PW_WANT_NONE, R6PW_CLASS_HARD, &lk);              /* the hard rows now (read only) */
    lp_printf(b, "pw.lock_now=%d,%s\n", lk, lp_nosp(_r6pw_lock_name(lk), w, sizeof(w)));
    lp_printf(b, "pw.res=%d,%s,%d\n", s->last_res, lp_nosp(_r6pw_res_name(s->last_res), w, sizeof(w)), s->last_lock);
    {
        char mb[100];                                                  /* the newest notice as one token */
        const char * m = _r6pw_msg();
        int i = 0;
        for (; m[i] && i < (int) sizeof(mb) - 1; i++) mb[i] = m[i] == ' ' ? '_' : m[i] == '\n' ? '|' : m[i] == '=' ? ':' : m[i];
        mb[i] = 0;
        lp_printf(b, "pw.msg=%s\n", i ? mb : "-");
    }
    lp_uc_update();
    int vm = lp_x_ptrs(&cc, &p);
    if ((vm & 2) && lp_x_rd(p, 0x110, 2, &v) && lp_x_rd(p, 0x112, 2, &v2)) lp_printf(b, "pw.cfnr=0x%04x,0x%04x\n", (int) v, (int) v2);
    else lp_printf(b, "pw.cfnr=na\n");
    uint32_t s1 = (vm & 1) ? RD(cc + 0x3500) : 0;                    /* sheet 1 = [cc+0x3500], inside [cc+0x34FC..0x350C] */
    if ((vm & 1) && ram_ok(s1, 0x674))
        lp_printf(b, "pw.s1=0x%08x,0x%08x,0x%08x,0x%08x,0x%04x\n", (int) RD(s1), (int) RD(s1 + 0xA8), (int) RD(s1 + 0x670),
            (int) RD(s1 + 0xD4), (int) (RD(s1 + 0x61C) >> 16));
    else lp_bad(b, "pw.s1", s1);
    if (vm & 1) lp_printf(b, "pw.fpub=0x%08x,0x%08x\n", (int) RD(cc + 0xD24), (int) RD(cc + 0xD20));
    else lp_bad(b, "pw.fpub", cc);
    if (vm & 1) lp_printf(b, "pw.ccg=0x%02x\n", (int) lp_x_ccg(cc));
    else lp_bad(b, "pw.ccg", cc);
    lp_printf(b, "pw.w.n=%d\n", (int) _r6pw_ring_n());   /* the write ring: M10.2b in the ring tail (lp_ring_tail) */
}

static const char lp_pw_hdr[] = "# pw.w[i]=t,kind,res,lock,ev,k,old,want,rep,cc,mm,ds4,d_ret,d_h,d_cc,d_mm,d_conf,ndel,nfor,fval,dep,"
                 "sl0,sl1,c62c,state,bak,ae,fl\n";

/* one write-ring line (log index i) into t, as printed before M10.2b; returns its length (with the newline) */
static int lp_pw_line(char * t, int n, int i, const struct r6pw_rec * e)
{
    char w[40];
    snprintf(t, n, "pw.w[%d]=%d,%s,%s,%d,%d,%d,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,%d,%d,%d,%d,%d,%d,%d,0x%04x,0x%x,0x%02x,"
                 "0x%02x,%d,%d,0x%04x,%d,0x%x\n", i, (int) e->t, _r6pw_kind_name(e->kind),
        lp_nosp(_r6pw_res_name(e->res), w, sizeof(w)), e->lock, e->ev, e->k, e->old & 0xFFFF, e->want & 0xFFFF, e->rep & 0xFFFF,
        e->cc & 0xFFFF, e->mm & 0xFFFF, e->ds4 & 0xFFFF, e->d_ret, e->d_h, e->d_cc, e->d_mm, e->d_conf, e->ndel, e->nfor,
        e->fval & 0xFFFF, e->dep, e->sl0, e->sl1, e->c62c, e->state, e->bak & 0xFFFF, e->ae, e->fl);
    return strlen(t);
}
#endif

#if defined(CONFIG_R6_INTERVAL) && defined(CONFIG_R6_PROPW_TV)
/* M10.2b (camera M10.2: LVP00101 96,621 of 98,176 B; the M10.2 budget had undercounted, spec 6.3): the picture ring (the
 * newest LP_IV_SHOTS_LOG) and the write ring (the newest LP_PW_LOG) come last. Newest first and by turns, the write line
 * before the picture line in each turn (M10.2b review S2: the Tor 1 record goes last), each line taken only while it
 * fits above LP_TAIL_SLACK, then they are printed oldest first (keys and fields as before; pw.w[i] counts from the oldest
 * line printed). Everything before the tail is bounded by a static budget (spec 6.3, the true worst case leaves the
 * tail >= 4 KB), so a snapshot is never cut; a fuller log gives the tail fewer lines:
 * iv.shot.log / pw.w.log = lines printed, candidates */
#define LP_TAIL_KEYS 40                   /* the two .log lines */
static void __attribute__((noinline)) lp_ring_tail(struct lp_buf * b)
{
    char t[256];
    const struct r6_iv_stats * s = r6_iv_stats();
    struct r6pw_rec e;
    int ns = MIN((int) s->shots, LP_IV_SHOTS_LOG), nr = _r6pw_ring(0, &e), nw = MIN(nr, LP_PW_LOG), ks = 0, kw = 0;
    lp_printf(b, "%s", lp_shot_hdr);
    int room = b->cap - b->len - LP_TAIL_SLACK - LP_TAIL_KEYS - (int) sizeof(lp_pw_hdr);   /* down to LP_TAIL_SLACK */
    for (int used = 0, ms = ns, mw = nw, more = 1; more; )
    {
        more = 0;
        if (kw < mw)                                             /* the write line first (review S2) */
        {
            _r6pw_ring(nr - 1 - kw, &e);
            int l = lp_pw_line(t, sizeof(t), 99, &e);          /* the index printed later has at most 2 digits */
            if (used + l <= room) { used += l; kw++; more = 1; } else mw = kw;
        }
        if (ks < ms)
        {
            int l = lp_shot_line(t, sizeof(t), s, s->shots - 1 - ks);
            if (used + l <= room) { used += l; ks++; more = 1; } else ms = ks;
        }
    }
    for (int i = ks; i > 0; i--)
    {
        lp_shot_line(t, sizeof(t), s, s->shots - i);
        lp_printf(b, "%s", t);
    }
    lp_printf(b, "iv.shot.log=%d,%d\n", ks, ns);
    lp_printf(b, "%s", lp_pw_hdr);
    for (int i = 0; i < kw; i++)
    {
        _r6pw_ring(nr - kw + i, &e);
        lp_pw_line(t, sizeof(t), i, &e);
        lp_printf(b, "%s", t);
    }
    lp_printf(b, "pw.w.log=%d,%d\n", kw, nr);
}
#endif

/* ---- log snapshot -------------------------------------------------------- */
#define LP_LOG_SIZE (96 * 1024)   /* M7.2a camera: up to 51.5 KB; M7.3 adds ~7 KB (QEMU), ~60 KB expected;
                                   * transient malloc, freed after the write */
#define LP_LOG_TAIL 128           /* reserve for the trailer lines */
#define LP_FILE_MAX 99999
static char lp_snap_msg[32] = "";
static volatile int lp_snap_busy = 0;     /* one snapshot at a time */
static int lp_snap_seq = 0;               /* per boot */
static int lp_file_no = 0;                /* last file number used in this boot */

#ifdef CONFIG_R6_MOTION
/* M9.3: R6 Motion (r6_motion.c): counters and last values, the newest triggers, one r6_fy_locate_done now (works with
 * the detector off), the display. Comma lists: md.nr[] (b048 ring_f no_rec_new no_rec_done same_buf handout size
 * other), md.gate[] (r6_md_gate_name order), md.rebase[] (start gate global pic) */
static void lp_md_arr(struct lp_buf * b, const char * key, const uint32_t * v, int n)
{
    char t[120];
    int j = 0;
    t[0] = 0;
    for (int i = 0; i < n && j < (int) sizeof(t) - 12; i++) j += snprintf(t + j, sizeof(t) - j, i ? ",%d" : "%d", (int) v[i]);
    lp_printf(b, "%s=%s\n", key, t);
}

static void lp_md_log(struct lp_buf * b)
{
    const struct r6_md_stats * s = r6_md_stats();
    int n = 0;
    const char * k = r6_md_knames(&n);
    for (int i = 0; i < n; i++, k += strlen(k) + 1) lp_printf(b, "md.%s=%d\n", k, (int) ((const int32_t *) s)[i]);
    lp_printf(b, "md.state_name=%s\n", r6_md_state_name(s->state));
    lp_printf(b, "md.gate_name=%s\n", r6_md_gate_name(s->gate_last));
    lp_printf(b, "md.nr_last=%s\n", s->nr_last && s->nr_last[0] ? s->nr_last : "-");
    lp_md_arr(b, "md.nr", s->nr, R6MD_NNR);
    lp_md_arr(b, "md.gate", s->gate, R6MD_NGATES);
    lp_md_arr(b, "md.rebase", s->rebase, R6MD_NRB);
    lp_printf(b, "# md.last[i]=t,f2,state,score,hits_all,run,result (oldest first; result -1 open, 0 view, 1 shot, 2+k dropped)\n");
    uint32_t nl = s->nlast;
    int c = nl < R6MD_RING ? (int) nl : R6MD_RING;
    for (int i = 0; i < c; i++)
    {
        const struct r6_md_last * e = &s->last[(nl - c + i) % R6MD_RING];
        lp_printf(b, "md.last[%d]=%d,%d,%d,%d,%d,%d,%d\n", i, (int) e->t, (int) e->f2, e->state, e->score, e->hits, e->run, e->result);
    }
    struct r6_fy_loc l;
    int ok = r6_fy_locate_done(&l);
    lp_printf(b, "md.loc=%d,%s,%d,%d,%d,%d,%d,%d,%d\n", ok, ok ? "ok" : l.why, (int) l.f1, (int) l.f2, l.pick, l.pick1, l.gidx,
        (int) l.b048, l.src);
    const struct r6_grid_stats * g = r6_grid_stats();
    lp_printf(b, "md.disp=%d,%d,%d,0x%08x\n", r6_md_disp_wanted(), (int) g->md_drawn, (int) g->md_n, (int) g->md_sig);
}
#endif

#ifdef CONFIG_R6_ETTR
/* ---- M11.0: R6 ETTR's second file ETTnnnnn.LOG (docs/m11_plan.md 6.1 / 6.2) ----
 * Written right after LVPnnnnn.LOG (same number) into the same buffer; its last LP_ETT_BINS bytes hold the bins copy.
 * Sections: cfg, cur (a full measurement now, with the bins), the advisor, counters, bins (4 x 256), the ALO set, the
 * 20 RandomData slots (FrameNum, rd[0x5EC]), Tor 0e (E2: GmtAeParam, rd+0x52C, AeWbRandomData, rd+0xBF8 / 0xC04,
 * PetitYch, [0x1427F8] through [0x1778C+0x18]), then the rings fy, m, raw, bk (oldest first). Read only, every pointer
 * checked (ram_ok); the main log gets one line ettr.file= (before the ring tail: the static budget grows by it alone). */
#define LP_ETT_BINS      4096       /* 4 x 256 u32 */
#define LP_ETT_RANGE_PTR (0x1778C + 0x18)   /* E2: must hold LP_ETT_RANGE_OBJ */
#define LP_ETT_RANGE_OBJ 0x1427C4           /* + 0x34 = 0x1427F8 (property 0x80050012 copy, R_rom.md 1) */

static uint32_t lp_ppm(uint32_t x, uint32_t n) { return n ? (uint32_t) (((uint64_t) x * 1000000u) / n) : 0; }

/* the channel names as strings: lp_printf goes through Canon's vsnprintf, whose %c repeats the character up to the line
 * end (QEMU m110: "ett.cur.ch[YYYY...]"; m8c_test notes the same) */
static const char * const lp_ett_ch[4] = { "Y", "R", "G", "B" };

static void lp_ett_meas(struct lp_buf * b, const char * key, const struct r6_ettr_meas * m)
{
    char w[24];
    lp_printf(b, "# %s=t,valid,rr,set,bind,tries,numpix,frame,sim,ae600,m608,tv424,fr52c,us,H,s_ppm,b_ppm\n", key);
    lp_printf(b, "%s=%d,%s,%d,%d,0x%x,%d,%d,%d,%d,%d,0x%04x,0x%04x,%d,%d,%d,%d,%d\n", key, (int) m->t,
        lp_nosp(r6_ettr_valid_name(m->valid), w, sizeof(w)), m->rr == 0xFF ? -1 : m->rr, m->set == 0xFF ? -1 : m->set,
        m->bind, m->tries, (int) m->numpix, (int) m->frame, m->sim, (int) m->ae600, m->m608 & 0xFFFF, m->tv424 & 0xFFFF,
        (int) m->fr52c, (int) m->us, (int) m->H, (int) m->s, (int) m->b);
    lp_printf(b, "%s.rd=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x\n", key, (int) m->rd_b, (int) m->rd_a, (int) m->hp_b,
        (int) m->hp_a, (int) m->p0);
    lp_printf(b, "# %s.ch[c]=pq,p99.9,p99,p50,p5,mean_x10,ppm>=224,>=240,>=248,>=252,=255\n", key);
    for (int c = 0; c < 4; c++)
    {
        const struct r6_ettr_ch * h = &m->ch[c];
        lp_printf(b, "%s.ch[%s]=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", key, lp_ett_ch[c], h->pq, h->p999, h->p99, h->p50, h->p5,
            h->mean10, (int) lp_ppm(h->n224, m->numpix), (int) lp_ppm(h->n240, m->numpix), (int) lp_ppm(h->n248, m->numpix),
            (int) lp_ppm(h->n252, m->numpix), (int) lp_ppm(h->n255, m->numpix));
    }
}

static void lp_ett_log(struct lp_buf * b, uint32_t (*bins)[256], int no)
{
    const struct r6_ettr_stats * s = r6_ettr_stats();
    char k[48], w[32];
    lp_uc_update();
    lp_printf(b, "# ETTR M11.0 snapshot (R6 ETTR, docs/m11_plan.md 6.2), next to LVP%05d.LOG\n", no);
    lp_printf(b, "# format: key=value; shares in ppm of numPix (rings: 0.01 %%); Tv / rd+0x424 / rd[0x608] s16 Code768 0x%%04x\n");
    lp_printf(b, "snapshot_seq=%d\n", lp_snap_seq);
    lp_printf(b, "boot_ms=%d\n", get_ms_clock());
    /* cfg */
    lp_printf(b, "# ett.cfg=mode,target,q_ppm,lo,zo,longest,shortest,show_lv\n");
    lp_printf(b, "ett.cfg=%d,%d,%d,%d,%d,0x%04x,0x%04x,%d\n", r6_ettr_mode(), s->target, s->q_ppm, s->lo, s->zo,
        s->longest & 0xFFFF, s->shortest & 0xFFFF, r6_ettr_lv_wanted());
    lp_printf(b, "# ett.consts=T_ml,L_ml,AE_ml,win_n,win_ms,win_dh,win_trend,win_ds_ppm,win_dy10,idle_fps10,gap_ms,keep_ms,g_iso100,wide_aim,meas_ms,curve\n");
    {
        char cs[96];
        r6_ettr_cfg_text(cs, sizeof(cs));
        lp_printf(b, "ett.consts=%s\n", cs);
    }
    /* cur: a full measurement now, the bins copied for the bins section */
    struct r6_ettr_meas m;
    int ok = r6_ettr_read_full(bins, &m);
    lp_ett_meas(b, "ett.cur", &m);
    {
        uint32_t cc, p;
        int vm = lp_x_ptrs(&cc, &p);
        lp_printf(b, "# ett.flags=expsim_ml,lv_dispsize,picstyle_raw,p548,htp,alo,flicker,iso (na: sheet not readable)\n");
        if (vm & 2)
            lp_printf(b, "ett.flags=%d,%d,0x%02x,0x%x,0x%x,0x%08x,0x%x,0x%x\n", _expsim, lv_dispsize, lens_info.raw_picstyle,
                (int) RD(p + 0x548), (int) RD(p + 0x4FC), (int) RD(p + 0xA8), (int) RD(p + 0x4E4), (int) RD(p + 0x510));
        else
            lp_printf(b, "ett.flags=%d,%d,0x%02x,na\n", _expsim, lv_dispsize, lens_info.raw_picstyle);
    }
    /* the advisor and the task's newest measurement */
    const struct r6_ettr_adv * a = &s->adv;
    lp_printf(b, "# ett.adv=t,t_meas,why,why_name,st,st_name,k,k_raw,lim,rest,hint,tv,want,lo,hi,g,sv,H,s,b,ym10,n,span,hspread,"
                 "dz,dw(sRGB D(H)-D(Z_o), D(H)-D(200), Code768, uncapped)\n");
    lp_printf(b, "ett.adv=%d,%d,%d,%s,%d,", (int) a->t, (int) a->t_meas, a->why, lp_nosp(r6_ettr_why_name(a->why), w, sizeof(w)), a->st);
    lp_printf(b, "%s,%d,%d,%d,%d,0x%x,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        lp_nosp(r6_ettr_st_name(a->st), w, sizeof(w)), a->k, a->k_raw, a->lim, a->rest, a->hint, a->tv & 0xFFFF,
        a->want & 0xFFFF, a->lo & 0xFFFF, a->hi & 0xFFFF, a->g & 0xFFFF, a->sv & 0xFFFF, (int) a->H, (int) a->s, (int) a->b,
        (int) a->ym10, a->n, a->span, a->hspread, a->dz, a->dw);
    lp_ett_meas(b, "ett.task", &s->cur);
    lp_printf(b, "# ett.n=ticks,meas,meas_ok,tries,torn,unbound,retries,resets,fresh,us_last,us_max,us_avg,bk_us_max,bk_n\n");
    lp_printf(b, "ett.n=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", (int) s->ticks, (int) s->meas, (int) s->meas_ok,
        (int) s->tries, (int) s->torn, (int) s->unbound, (int) s->retries, (int) s->resets, (int) s->fresh_n,
        (int) s->us_last, (int) s->us_max, (int) s->us_avg, (int) s->bk_us_max, (int) s->bk_n);
    lp_printf(b, "# ett.ref=fps10,fps_cls,t_ref,f_ref,ae_ref,ref_why(1 Tv,2 settings,3 frame rate,4 gap/no LV,5 start,6 no proposal),h_n,h_ovf,tv,tv_len,range\n");
    lp_printf(b, "ett.ref=%d,%d,%d,%d,%d,%d,%d,%d,0x%04x,%d,0x%x\n", s->fps10, s->fps_cls, (int) s->t_ref, (int) s->f_ref,
        (int) s->ae_ref, s->ref_why, (int) s->h_n, (int) s->h_ovf, s->tv & 0xFFFF, s->tv_len, s->range);
    lp_printf(b, "ett.rings=%d,%d,%d,%d,%d,%d,%d,%d\n", (int) s->m_n, (int) s->raw_n, (int) s->bk_ring_n, (int) s->fy_n,
        (int) s->fy_torn, (int) s->fy_us_max, (int) s->lv_draws, (int) s->lv_erases);
    {
        char t[160];
        int j = 0;
        for (int i = 0; i < R6ETT_NM; i++) j += snprintf(t + j, sizeof(t) - j, i ? ",%d" : "%d", (int) s->nv[i]);
        lp_printf(b, "ett.nv=%s\n", t);                           /* per R6ETT_M_*: ok,nolv,read,torn,unbound,nosim,expsim,magnify,numpix,idle */
        j = 0;
        for (int i = 0; i < R6ETT_NW; i++) j += snprintf(t + j, sizeof(t) - j, i ? ",%d" : "%d", (int) s->why_n[i]);
        lp_printf(b, "ett.whyn=%s\n", t);                         /* per R6ETT_W_* */
    }
    /* bins: 4 x 256, 16 per line (the copy of ett.cur) */
    lp_printf(b, "# ett.bin[c][i]=bins i..i+15 of channel c (Y R G B) of ett.cur (read %s)\n", ok ? "ok" : "failed");
    if (ok)
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < 256; i += 16)
            {
                char t[200];
                int j = 0;
                for (int x = i; x < i + 16; x++) j += snprintf(t + j, sizeof(t) - j, x > i ? ",%d" : "%d", (int) bins[c][x]);
                lp_printf(b, "ett.bin[%s][%d]=%s\n", lp_ett_ch[c], i, t);
            }
    /* the ALO set of Canon's histogram (E2: ctx+0x28 / 0x2C, stride 8): edges and 16-bin sums */
    uint32_t hctx = RD(LP_HIST_STATE + 4), pp[4];
    lp_printf(b, "ett.hist.ctx=0x%08x,%d\n", (int) hctx, lp_hist_ptrs(hctx, pp));
    lp_printf(b, "# ett.alo[s][i]=ptr,sum,mean_x10,bin0..7,bin248..255,b16[0..15]\n");
    if (ram_ok(hctx, LP_HIST_CTX_SIZE))
        for (int sset = 0; sset < 2; sset++)
            for (int i = 0; i < 4; i++)
            {
                struct lp_hstat hs;
                uint32_t p = RD(hctx + (sset ? 0x2C : 0x28) + 8 * i);
                snprintf(k, sizeof(k), "ett.alo[%d][%d]", sset, i);
                if (!lp_hist_stats(p, &hs)) { lp_bad(b, k, p); continue; }
                char t[300];
                int j = snprintf(t, sizeof(t), "0x%08x,%d,%d", (int) p, (int) hs.sum, (int) hs.mean_x10);
                for (int x = 0; x < 8; x++) j += snprintf(t + j, sizeof(t) - j, ",%d", (int) hs.lo[x]);
                for (int x = 0; x < 8; x++) j += snprintf(t + j, sizeof(t) - j, ",%d", (int) hs.hi[x]);
                for (int x = 0; x < 16; x++) j += snprintf(t + j, sizeof(t) - j, ",%d", (int) hs.b16[x]);
                lp_printf(b, "%s=%s\n", k, t);
            }
    else lp_bad(b, "ett.alo", hctx);
    /* the 20 RandomData slots: FrameNum and the histogram pointer rd[0x5EC] (plan 2.10 question 6) */
    {
        uint32_t uw = 0, ix = 0;
        rd32(LP_DTS + 0x10, &uw);
        rd32(LP_DTS + 0x1C, &ix);
        lp_printf(b, "ett.rd.dts=%d,%d\n", (int) uw, (int) ix);
        lp_printf(b, "# ett.rd20[i]=ptr,FrameNum,rd[0x5EC]\n");
        for (int i = 0; i < LP_RD_RING_N; i++)
        {
            uint32_t p = RD(LP_RD_RING + 4 * i);
            snprintf(k, sizeof(k), "ett.rd20[%d]", i);
            if (!p || !ram_ok(p, LP_RD_SIZE)) { lp_bad(b, k, p); continue; }
            lp_printf(b, "%s=0x%08x,%d,0x%08x\n", k, (int) p, (int) RD(p), (int) RD(p + 0x5EC));
        }
    }
    /* Tor 0e (E2), newest RandomData record */
    {
        int src = 0;
        uint32_t rd = lp_rd_base(&src), q = 0, v = 0;
        lp_printf(b, "ett.tor0e.rd=0x%08x,%d\n", (int) rd, src);
        if (rd)
        {
            lp_flds(b, "ett.gmt", rd, 0x41C, 0x5C / 4);              /* GmtAeParam (23 words, rd+0x424 = the LV AE's Tv) */
            lp_fld(b, "ett.rd[0x52c]", rd, 0x52C);                   /* GmtAeRecFrameRate */
            lp_flds(b, "ett.aewb", rd, 0x538, 0x98 / 4);             /* AeWbRandomData (38 words) */
            lp_fld(b, "ett.rd[0xbf8]", rd, 0xBF8);                   /* fEnableFlicker */
            lp_fld(b, "ett.rd[0xc04]", rd, 0xC04);                   /* fGmtBadFrame */
            lp_flds(b, "ett.petit", rd, 0x624, 4);                   /* PetitYch descriptor (16 B, no pixels) */
        }
        if (rd32(LP_ETT_RANGE_PTR, &q) && q == LP_ETT_RANGE_OBJ && rd32(q + 0x34, &v))
            lp_printf(b, "ett.range=0x%08x,0x%08x\n", (int) q, (int) v);   /* bit 0 sim, 1 no sim, 4 out of range */
        else
            lp_printf(b, "ett.range=0x%08x,na\n", (int) q);
    }
    /* rings, oldest first */
    struct r6_ettr_fy f;
    int n = r6_ettr_ring_fy(0, &f);
    lp_printf(b, "# ett.fy[i]=t,f2,ok,us,npx,r250,g250,b250,u250,y235,y245,y250,y255,maxR,maxG,maxB,maxY (FaceYuv 2x2, Expo log ON)\n");
    for (int i = 0; i < n; i++)
    {
        r6_ettr_ring_fy(i, &f);
        lp_printf(b, "ett.fy[%d]=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i, (int) f.t, (int) f.f2, f.ok, (int) f.us,
            (int) f.npx, (int) f.r250, (int) f.g250, (int) f.b250, (int) f.u250, (int) f.y235, (int) f.y245, (int) f.y250,
            (int) f.y255, f.mr, f.mg, f.mb, f.my);
    }
    struct r6_ettr_m e;
    n = r6_ettr_ring_m(0, &e);
    lp_printf(b, "# ett.m[i]=t,frame,tv,want,H,valid,rr,flags(1|2 bound,4 same rec,8 fresh,16 window,32 range bit 4),pR,pG,pB,"
                 "sim,s,b,ym10,fps10,st,why,k,set,ae600,tv424,m608,rng([0x1427F8] low byte: 1 sim,2 no sim,16 out of range;0xff na)\n");
    for (int i = 0; i < n; i++)
    {
        r6_ettr_ring_m(i, &e);
        lp_printf(b, "ett.m[%d]=%d,%d,0x%04x,0x%04x,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,0x%04x,0x%04x,0x%02x\n", i,
            (int) e.t, (int) e.frame, e.tv & 0xFFFF, e.want & 0xFFFF, e.H, e.valid, e.rr == 0xFF ? -1 : e.rr, e.flags, e.pr, e.pg,
            e.pb, e.sim, e.s, e.b, e.ym10, e.fps10, e.st, e.why, e.k, e.set, e.ae600, e.tv424 & 0xFFFF, e.m608 & 0xFFFF, e.rng);
    }
    struct r6_ettr_raw r;
    n = r6_ettr_ring_raw(0, &r);
    lp_printf(b, "# ett.raw[i]=t,old,tv,src(1 ML),len,n,f0,ae0,t0,wn; ett.raw[i].k[j]=dt,dfr,flags(valid|set<<4|bound<<5|same<<6),ae,tv424,ym10,H,s,pR,pG,pB\n");
    for (int i = 0; i < n; i++)
    {
        r6_ettr_ring_raw(i, &r);
        lp_printf(b, "ett.raw[%d]=%d,0x%04x,0x%04x,%d,%d,%d,%d,%d,%d,%d\n", i, (int) r.t, r.old & 0xFFFF, r.tv & 0xFFFF, r.src,
            r.len, r.n, (int) r.f0, r.ae0, r.t0, (int) r.wn);
        for (int j = 0; j < r.n && j < R6ETTR_RAW_N; j++)
        {
            const struct r6_ettr_rt * t = &r.tk[j];
            lp_printf(b, "ett.raw[%d].k[%d]=%d,%d,0x%02x,%d,0x%04x,%d,%d,%d,%d,%d,%d\n", i, j, t->dt, t->dfr, t->flags, t->ae,
                t->tv424 & 0xFFFF, t->ym10, t->H, t->s, t->pr, t->pg, t->pb);
        }
    }
    struct r6_ettr_bk x;
    n = r6_ettr_ring_bk(0, &x);
    lp_printf(b, "# ett.bk[i]=t,what(1 conf,2 SW1),ev,k,want,tv,fn,frame,valid,set,bind,sim,H,pR,pG,pB,s,b,ym10,tv424,m608,ae600,us\n");
    for (int i = 0; i < n; i++)
    {
        r6_ettr_ring_bk(i, &x);
        lp_printf(b, "ett.bk[%d]=%d,%d,%d,%d,0x%04x,0x%04x,%d,%d,%d,%d,0x%x,%d,%d,%d,%d,%d,%d,%d,%d,0x%04x,0x%04x,%d,%d\n", i,
            (int) x.t, x.what, x.ev, x.k, x.want & 0xFFFF, x.tv & 0xFFFF, (int) x.fn, (int) x.frame, x.valid,
            x.set == 0xFF ? -1 : x.set, x.bind, x.sim, x.H, x.pr, x.pg, x.pb, x.s, x.b, x.ym10, x.tv424 & 0xFFFF, x.m608 & 0xFFFF,
            x.ae600, x.us);
    }
}

/* the number lp_write_log will take (the first free LVPn after lp_file_no; nothing creates a file in between: one
 * snapshot at a time), for the line ettr.file before the main log is written; lp_file_no is set to the number before it,
 * so lp_write_log's own scan starts there (one is_file more, not a second scan from 1); 0 = none */
static int lp_ett_next_no(void)
{
    char name[32];
    for (int i = 0, n = lp_file_no + 1; i < 1000 && n <= LP_FILE_MAX; i++, n++)
    {
        snprintf(name, sizeof(name), "ML/LOGS/LVP%05d.LOG", n);
        if (!is_file(name)) { lp_file_no = n - 1; return n; }
    }
    return 0;
}

/* ETTnnnnn.LOG next to the LVP file just written, a new file only (an existing one is kept: no overwrite); inlined into
 * lp_snapshot_do, its only caller (the FIO calls stay there, canon_gate allow list) */
static inline __attribute__((always_inline)) int lp_write_ett(struct lp_buf * b, const char * name)
{
    if (is_file(name)) return 0;
    FILE * f = FIO_CreateFile(name);
    if (!f) return 0;
    int w = FIO_WriteFile(f, b->p, b->len);
    FIO_CloseFile(f);
    return w == b->len;
}
#endif

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
    struct lp_buf b = { .p = malloc(LP_LOG_SIZE), .len = 0, .cap = LP_LOG_SIZE - LP_LOG_TAIL, .trunc = 0 };
    if (!b.p)
    {
        snprintf(lp_snap_msg, sizeof(lp_snap_msg), "no memory");
        return;
    }
#ifdef CONFIG_R6_ETTR
    int no = lp_ett_next_no();            /* M11.0: the number lp_write_log will take (ettr.file) */
#endif
    lp_printf(&b, "# LVPROBE M7.0 snapshot\n");
    lp_printf(&b, "# format: key=value; raw words 0x%%08x, counts decimal, 64-bit IBus 0xHI:LO;\n");
    lp_printf(&b, "# NULL = Canon pointer 0; invalid(...) / uncached_unverified(...) = not dereferenced\n");
    lp_printf(&b, "probe_stage=M7.3\n");
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
#ifdef CONFIG_R6_INTERVAL
    lp_iv_log(&b);
#endif
#ifdef CONFIG_R6_PROPW_TV
    lp_pw_log(&b);                        /* M10.2 */
#endif
#ifdef CONFIG_R6_MOTION
    lp_md_log(&b);                        /* M9.3 */
#endif
    lp_x_log(&b);                         /* M9.1 Tor 0 */
#ifdef CONFIG_R6_ETTR
    /* M11.0: one line (at most 30 B, inside the static budget before the ring tail): the second file, Mode, reason, state */
    lp_printf(&b, "ettr.file=ETT%05d.LOG,%d,%d,%d\n", no, r6_ettr_mode(), r6_ettr_mode() ? r6_ettr_stats()->adv.why : R6ETT_W_OFF,
        r6_ettr_mode() ? r6_ettr_stats()->adv.st : R6ETT_S_NONE);
#endif
#if defined(CONFIG_R6_INTERVAL) && defined(CONFIG_R6_PROPW_TV)
    lp_ring_tail(&b);                     /* M10.2b: the picture / write rings last, down to LP_TAIL_SLACK */
#endif
    int trunc = b.trunc;
    b.cap = LP_LOG_SIZE;                  /* the trailer always fits */
    lp_printf(&b, "log_truncated=%d\n", trunc);
    lp_printf(&b, "log_bytes=%d\n# end\n", b.len);
    int ok = lp_write_log(&b, name, sizeof(name));
    const char * base = name[0] ? name + 8 : "?";     /* without "ML/LOGS/" */
    if (ok) snprintf(lp_snap_msg, sizeof(lp_snap_msg), "%s, %d B", base, b.len);
    else snprintf(lp_snap_msg, sizeof(lp_snap_msg), "write FAILED (%s)", base);
#ifdef CONFIG_R6_ETTR
    if (ok)
    {
        /* M11.0: ETTnnnnn.LOG with the number of the LVP file just written, the same buffer filled again; its last
         * LP_ETT_BINS bytes hold the bins copy */
        char ename[32];
        int n = lp_file_no;
        uint32_t (*bins)[256] = (uint32_t (*)[256]) (b.p + LP_LOG_SIZE - LP_ETT_BINS);
        b.len = 0;
        b.trunc = 0;
        b.cap = LP_LOG_SIZE - LP_ETT_BINS - LP_LOG_TAIL;
        lp_ett_log(&b, bins, n);
        trunc = b.trunc;
        b.cap = LP_LOG_SIZE - LP_ETT_BINS;
        lp_printf(&b, "log_truncated=%d\n", trunc);
        lp_printf(&b, "log_bytes=%d\n# end\n", b.len);
        snprintf(ename, sizeof(ename), "ML/LOGS/ETT%05d.LOG", n);
        if (!lp_write_ett(&b, ename)) snprintf(lp_snap_msg, sizeof(lp_snap_msg), "%s, ETT FAILED", base);
    }
#endif
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
#ifdef CONFIG_R6_INTERVAL
    msleep(4000);            /* menu closed; user shows the LV screen */
    if (r6_iv_running())     /* M8.0: no card write while a series runs */
        snprintf(lp_snap_msg, sizeof(lp_snap_msg), "skipped (interval)");
    else
        lp_snapshot_do(0);
#else
    lp_snapshot_do(4000);    /* menu closed; user shows the LV screen */
#endif
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
 * M7.3: padded with spaces to a fixed width per font (and never narrower
 * than the widest text drawn on that line), and, because FONT_MED is
 * proportional (argnor23: space 7 px, digits 12 px, W 19 px), the pixel
 * tail of the previous, wider text is filled with COLOR_EMPTY (the pixel
 * width comes from bmp_printf). The ownership refresh then erases that tail
 * in layer 0 as well. */
#define LP_SLINES 15
#define LP_SLINE_MAX 64
#define LP_SLINE_PAD_MED   44     /* lines 0..6, FONT_MED */
#define LP_SLINE_PAD_SMALL 50     /* lines 7.., FONT_SMALL (8 px) */
static char lp_sl_prev[LP_SLINES][LP_SLINE_MAX];
static int lp_sl_w[LP_SLINES];
static int lp_sl_px[LP_SLINES];          /* pixel width drawn last time */
static int lp_sl_force = 1;
static uint32_t lp_sl_printed = 0;       /* lines printed (QEMU test reads it) */

static void lp_sline_put(int i, int font, int x, int y, char * t)
{
    t[LP_SLINE_MAX - 1] = 0;
    if (!lp_sl_force && !strcmp(t, lp_sl_prev[i])) return;
    strcpy(lp_sl_prev[i], t);
    int n = strlen(t);
    int w = MAX(i < 7 ? LP_SLINE_PAD_MED : LP_SLINE_PAD_SMALL, lp_sl_w[i]);
    while (n < w && n < LP_SLINE_MAX - 1) t[n++] = ' ';
    t[n] = 0;
    lp_sl_w[i] = n;
    int px = bmp_printf(font, x, y, "%s", t);
    if (px >= 0 && px < lp_sl_px[i])
        bmp_fill(COLOR_EMPTY, x + px, y, lp_sl_px[i] - px, fontspec_height(font));
    lp_sl_px[i] = MAX(px, 0);
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

    /* M7.3 o: ownership refresh, last conversion: blocks skipped / converted,
     * owned pixels, erased pixels and veto blocks (total), conversion us;
     * v: histogram overlay state, draws, erases; FaceYuv one-shot */
#ifdef CONFIG_R6_LAYER_OWNERSHIP
    const struct r6_layer_stats * ls = r6_layer_stats();
    lp_sline(13, fs, x, ys + 6 * dys, "o sk=%s cv=%s px=%s cl=%s vt=%s %dus",
             lp_cnt(c[0], ls->blk_skip), lp_cnt(c[1], ls->blk_conv), lp_cnt(c[2], ls->px_owned),
             lp_cnt(c[3], ls->px_cleared + ls->clear_px), lp_cnt(c[4], ls->veto_blk + ls->clear_veto_blk),
             (int) ls->us_last);
#else
    lp_sline(13, fs, x, ys + 6 * dys, "o off");
#endif
#ifdef FEATURE_R6_CANON_HISTOGRAM
    const struct r6_overlay_stats * os = r6_overlay_stats();
    lp_sline(14, fs, x, ys + 7 * dys, "v %s d=%s e=%s fy=%s",
             r6_overlay_reason(os->state), lp_cnt(c[0], os->draws), lp_cnt(c[1], os->erases),
             lp_fy_msg[0] ? lp_fy_msg : "-");
#else
    lp_sline(14, fs, x, ys + 7 * dys, "v off fy=%s", lp_fy_msg[0] ? lp_fy_msg : "-");
#endif
}

#define LP_STATUS_TICK_MS  200
#define LP_STATUS_DRAW_N   5      /* compose the text every 5 ticks = 1 s */
#define LP_STATUS_FORCE_N  25     /* print all lines every 25 ticks = 5 s */

static void lp_status_task(void * unused)
{
    int cnt = 0, tick = 0, menu_seen = 0;
    TASK_LOOP
    {
        if (lp_x_on) lp_x_sample();      /* M9.1 Tor 0: every tick, also with the ML menu open (RAM reads only) */
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
    if ((lp_status_on || lp_x_on) && !lp_status_task_started)     /* M9.1: also for the expo log alone */
    {
        lp_status_task_started = 1;
        task_create("lvprobe_status", 0x1e, 0x2000, lp_status_task, 0);
    }
}

/* M9.1 (menu, QEMU test): switch the expo log; ON takes a new baseline (ring B
 * restarts); the status task samples it, the status line keeps its mode */
void lvprobe_expo_enable(int on)
{
    if (on && !lp_x_on) lp_x_reset = 1;
    lp_x_on = on ? 1 : 0;
    lvprobe_status_enable(lp_status_on);
}

#ifdef CONFIG_R6_ETTR
/* M11.0: R6 ETTR samples FaceYuv 2 x 2 only while the Expo log is ON (docs/m11_plan.md 2.1) */
int lvprobe_expo_on(void) { return lp_x_on; }
#endif

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

#ifdef CONFIG_R6_AUTOSTART
/* M7.6 (internals.h CONFIG_R6_AUTOSTART): called once by module_load_task
 * (src/module.c) right after its LOADING.LCK check. Until then lv stays 0
 * (the LV flag starts OFF), so no overlay can draw before this decision.
 *   1  clean shutdown before this boot and this session's lockfile is
 *      written: LV flag ON; Global Draw keeps its saved value
 *   0  the last session did not end cleanly (module.c shows the NotifyBox)
 *  -1  not clean or unknown (no lockfile written, autoload disabled)
 * In both other cases the LV flag stays OFF and Global Draw is set OFF
 * (in RAM; saved as OFF at the next clean shutdown), as before M7.6. */
void r6_autostart(int clean)
{
    if (lp_autostart != -2) return;
    lp_autostart = clean;
    if (clean == 1)
    {
        r6_global_draw_boot(1);
        lp_follow_menu = 1;
        lv_follow_canon_set(1);
        return;
    }
    r6_global_draw_boot(0);
    if (clean < 0)
        NotifyBox(10000, "Last shutdown not seen as clean:\nLV flag and Global Draw OFF.");
}
#endif

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
#ifdef CONFIG_R6_INTERVAL
    if (r6_iv_running())     /* M8.0: no card write while a series runs */
    {
        snprintf(lp_snap_msg, sizeof(lp_snap_msg), "interval running");
        return;
    }
#endif
    lp_snap_busy = 1;
    snprintf(lp_snap_msg, sizeof(lp_snap_msg), "starting");
    run_in_separate_task(lp_snapshot_task, 0);
}

static MENU_UPDATE_FUNC(lp_snapshot_update)
{
    if (lp_snap_msg[0]) MENU_SET_VALUE("%s", lp_snap_msg);
}

/* RAM reads only (sparse grid, uncached alias, every pointer checked) */
static MENU_SELECT_FUNC(lp_fy_select)
{
    lvprobe_faceyuv_oneshot();
}

static MENU_UPDATE_FUNC(lp_fy_update)
{
    if (lp_fy_msg[0]) MENU_SET_VALUE("%s", lp_fy_msg);
}

static MENU_SELECT_FUNC(lp_x_toggle)
{
    lvprobe_expo_enable(!lp_x_on);
}

static MENU_UPDATE_FUNC(lp_x_update)
{
    if (lp_x_on) MENU_SET_RINFO("%d changes", (int) lp_xcb_n);
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
                #ifdef CONFIG_R6_AUTOSTART
                .help   = "ON: lv = !PROP_LV_ACTION. ON at boot after a clean shutdown, else OFF.",
                #else
                .help   = "ON: lv = !PROP_LV_ACTION. OFF (default, not saved): lv = 0 as in M6.",
                #endif
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
            {
                .name   = "FaceYuv sample",
                .select = lp_fy_select,
                .update = lp_fy_update,
                .help   = "One-shot: 32x24 FaceYuv words (Y mean as UYVY / as YUYV).",
                .help2  = "Read-only, uncached alias; details in the next log snapshot.",
            },
            {
                .name   = "Expo log",
                .priv   = &lp_x_on,
                .max    = 1,
                .select = lp_x_toggle,
                .update = lp_x_update,
                .help   = "M9.1: exposure values (Canon's CC copies) every 200 ms, deliveries.",
                .help2  = "Read-only; in the next Log snapshot (expo.*). Not saved: OFF at boot.",
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
