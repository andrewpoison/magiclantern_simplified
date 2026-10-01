/*
 * R6.190 M7.0: read-only LiveView census probe.
 *
 * Debug > "LV probe":
 *  - "LV flag follows Canon": lv = !PROP_LV_ACTION (propvalues.c,
 *    CONFIG_LV_FOLLOW_TOGGLE). Default off and not saved, so every boot
 *    starts with lv = 0 as in M6.
 *  - "Status line": a 200 ms task prints 7 short lines with bmp_printf into
 *    ML's own pixels of Canon's layer 0 (as approved in M6), also while
 *    DISPLAY_IS_ON == 0, with a running counter. "ON" leaves the refresh to
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
    int t0 = get_ms_clock(), t = t0;
    while (samples < LP_POLL_MAX_SAMPLES)
    {
        msleep(10);
        uint32_t v = RD(LP_VMIX + 0x10);
        if (v != prev) { changes++; prev = v; }
        if (RD(LP_VMIX + 0x18)) pending++;
        samples++;
        t = get_ms_clock();
        if (t < t0 || t - t0 >= 1000) break;
    }
    lp_printf(b, "vmix_poll_ms=%d\n", t - t0);
    lp_printf(b, "vmix_poll_samples=%d\n", samples);
    lp_printf(b, "vmix_buf_index_changes=%d\n", changes);
    lp_printf(b, "vmix_pending_samples=%d\n", pending);
}

/* ---- log snapshot -------------------------------------------------------- */
#define LP_LOG_SIZE (32 * 1024)
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

/* EVF output active per Canon's display mask (unknown -> no) */
static int lp_evf_active(void)
{
    uint32_t mask = 0;
    return fld(RD(LP_DISPDEV_PTR), 0x18, &mask) && (mask & LP_MASK_EVF);
}

static void lp_status_draw(int cnt)
{
    char s1[12], s2[12], s3[12], s4[12], s5[12];
    uint32_t v, lo, hi, w5 = 0, w6 = 0, fmt = 0;
    int len = 0;
    int font = FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK);
    int x = 40, y = 120, dy = 23;

    lp_uc_update();

    int seen = lv_action_raw_get(&v, &len);
    bmp_printf(font, x, y + 0 * dy, "LVP %d lv=%d act=%s/%d n=%d fol=%d R%d   ", cnt, lv,
               lp_hex(s1, seen, v), len, (int) lp_ring_count, lv_follow_canon_get(), (int) lp_forced_refresh);

    uint32_t dd = RD(LP_DISPDEV_PTR), mask = 0;
    int mask_ok = fld(dd, 0x18, &mask);
    bmp_printf(font, x, y + 1 * dy, "gm=%x gs=%d disp=%d dlg=%x ovl=%d   ",
               (int) RD(LP_GUI_MODE), gui_state, (int) RD(LP_DISPLAY_IS_ON),
               (int) RD(LP_LVAPP_DIALOG), (int) RD(LP_LV_OVERLAYS));

    uint32_t xcm = RD(LP_XCM_TABLE), nctx = 0;
    int nctx_ok = fld(xcm, 0xC, &nctx);
    bmp_printf(font, x, y + 2 * dy, "mask=%s vm=%d xcm=%s lvl=%x ss=%x   ",
               lp_hex(s1, mask_ok, mask), (int) RD(LP_VRAMMGR_MODE), lp_hex(s2, nctx_ok, nctx),
               (int) RD(LP_LEVEL_DIALOG), (int) RD(LP_SILENT_DIALOG));

    uint32_t dvc = RD(LP_DVC_PTR);
    int dvc_ok = dvc && ram_ok(dvc, 4);
    int p_ok = dvc_ok && rd64(dvc + 0x130, &lo, &hi) == 1;
    int i_ok = fld(dvc, 0x1C8 + 4 * 5, &w5) && fld(dvc, 0x1C8 + 4 * 6, &w6) && fld(dvc, 0x1C8 + 4 * 0xe, &fmt);
    bmp_printf(font, x, y + 3 * dy, "pnl=%s:%s f=%s %sx%s   ",
               lp_hex(s1, p_ok, hi), lp_hex(s2, p_ok, lo),
               lp_hex(s3, i_ok, fmt), lp_hex(s4, i_ok, w5), lp_hex(s5, i_ok, w6));

    int e_ok = dvc_ok && rd64(dvc + 0x140, &lo, &hi) == 1;
    bmp_printf(font, x, y + 4 * dy, "evf=%s:%s vmix=%d/%d   ",
               lp_hex(s1, e_ok, hi), lp_hex(s2, e_ok, lo),
               (int) RD(LP_VMIX + 0x10), (int) RD(LP_VMIX + 0x18));

    bmp_printf(font, x, y + 5 * dy, "ref=%d/%d/%dus free=%dK   ",
               (int) lp_ref_conv, (int) lp_ref_wait, (int) lp_ref_ximr,
               GetFreeMemForMalloc() / 1024);

    /* XOC layer enable of XCM 0, outputs 0 and 1; expsim (raw / ML); digic gain */
    uint32_t x0 = 0, x1 = 0;
    int x0_ok = fld(RD(LP_XOC_TABLE), 4, &x0), x1_ok = fld(RD(LP_XOC_TABLE + 4), 4, &x1);
    bmp_printf(font, x, y + 6 * dy, "xoc=%s/%s es=%d/%d g=%d   ",
               lp_hex(s1, x0_ok, x0), lp_hex(s2, x1_ok, x1),
               _expsim, get_expsim(), get_digic_iso_gain_photo());
}

static void lp_status_task(void * unused)
{
    int cnt = 0;
    TASK_LOOP
    {
        if (lp_status_on && !gui_menu_shown())
        {
            lp_status_draw(++cnt);
            /* redraw_task refreshes only while DISPLAY_IS_ON; "ON, refresh"
             * refreshes here as well, like menu_redraw_flood does, but only
             * for the EVF (mask bit 4), never during power-save display off
             * or shutdown (same shutdown check as bmp.c's refresh) */
            if (lp_status_on == 2 && !DISPLAY_IS_ON && ml_refresh_display_needed
                && !ml_shutdown_requested && lp_evf_active())
            {
                refresh_yuv_from_rgb();
                lp_forced_refresh++;
            }
        }
        msleep(200);
    }
}

void lvprobe_status_enable(int mode)
{
    lp_status_on = COERCE(mode, 0, 2);
    if (lp_status_on && !lp_status_task_started)
    {
        lp_status_task_started = 1;
        task_create("lvprobe_status", 0x1e, 0x2000, lp_status_task, 0);
    }
}

/* QEMU test only reads it; also shown as R<n> on the status line */
uint32_t lvprobe_forced_refresh_count(void) { return lp_forced_refresh; }

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
                .help   = "Shows lv, GUI, display and LV buffer state every 200 ms.",
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
